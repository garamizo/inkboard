"""A cache in front of an upstream fetch (spec §3.4).

Stale-while-revalidate: expired entries are served at once while one background refresh
runs; only keys with no data wait, bounded by the caller's deadline. Also: last-good
fallback marked stale, single-flight per key, LRU caps in memory and on disk (enforced
across restarts), retry spacing after failures, and a daily upstream budget that survives
restarts.
"""
from __future__ import annotations

import hashlib
import json
import logging
import threading
import time
import uuid
from collections import OrderedDict
from concurrent.futures import Executor, Future, ThreadPoolExecutor, wait
from dataclasses import dataclass
from datetime import date, datetime, timedelta, timezone
from pathlib import Path
from typing import Any, Callable

log = logging.getLogger(__name__)
_BACKGROUND = ThreadPoolExecutor(max_workers=8, thread_name_prefix="inkboard-fetch")


def utcnow() -> datetime:
    return datetime.now(timezone.utc)


@dataclass(frozen=True)
class SourceResult:
    key: str
    data: Any
    fetched_at: datetime  # last *successful* fetch
    stale: bool

    @property
    def version(self) -> tuple[str, str, bool]:
        return (self.key, self.fetched_at.isoformat(), self.stale)


class NoData(Exception):
    """No data for this key yet: never fetched successfully, or still fetching past the deadline."""

    def __init__(self, source: str):
        super().__init__(source)
        self.source = source


class InlineExecutor(Executor):
    """Runs submitted work in the caller's thread (deterministic tests)."""

    def submit(self, fn, /, *args, **kwargs) -> Future:
        f: Future = Future()
        try:
            f.set_result(fn(*args, **kwargs))
        except BaseException as exc:  # pragma: no cover - _refresh never raises
            f.set_exception(exc)
        return f


def _atomic_write(path: Path, text: str) -> None:
    tmp = path.with_name(f".{path.name}.{uuid.uuid4().hex}.tmp")
    tmp.write_text(text)
    tmp.replace(path)


@dataclass
class _Entry:
    data: Any
    fetched_at: datetime
    last_failure: datetime | None = None


class _DailyBudget:
    """Upstream calls per UTC day, persisted so a restart does not reset the count."""

    def __init__(self, cap: int | None, path: Path | None):
        self.cap, self.path = cap, path
        self.day: date | None = None
        self.calls = 0
        if cap is not None and path is not None and path.exists():
            try:
                raw = json.loads(path.read_text())
                self.day, self.calls = date.fromisoformat(raw["day"]), int(raw["calls"])
            except (OSError, ValueError, KeyError) as exc:
                log.warning("ignoring unreadable budget file %s: %r", path, exc)

    def take(self, now: datetime) -> bool:
        """Caller holds the source lock."""
        if self.cap is None:
            return True
        if self.day != now.date():
            self.day, self.calls = now.date(), 0
        if self.calls >= self.cap:
            return False
        self.calls += 1
        if self.path is not None:
            _atomic_write(self.path, json.dumps({"day": self.day.isoformat(), "calls": self.calls}))
        return True


class CachedSource:
    def __init__(
        self,
        name: str,
        fetch: Callable[[dict], Any],
        *,
        ttl: timedelta,
        cache_dir: Path | None = None,
        max_entries: int = 1000,
        daily_cap: int | None = None,
        retry_after: timedelta = timedelta(minutes=5),
        stale_after: timedelta | None = None,
        cold_wait_s: float = 8.0,
        executor: Executor | None = None,
        clock: Callable[[], datetime] = utcnow,
    ):
        self.name = name
        self.fetch = fetch
        self.ttl = ttl
        self.stale_after = stale_after or 2 * ttl
        self.max_entries = max_entries
        self.retry_after = retry_after
        self.cold_wait_s = cold_wait_s
        self.executor = executor or _BACKGROUND
        self.clock = clock
        self.upstream_calls = 0
        self.cache_dir = cache_dir / name if cache_dir else None
        self._lock = threading.Lock()
        self._entries: OrderedDict[str, _Entry] = OrderedDict()      # memory LRU
        self._files: OrderedDict[str, str | None] = OrderedDict()    # disk LRU: file name -> key if known
        self._miss_failures: OrderedDict[str, datetime] = OrderedDict()
        self._inflight: dict[str, Future] = {}
        if self.cache_dir:
            self.cache_dir.mkdir(parents=True, exist_ok=True)
            self._scan_disk()
        self._budget = _DailyBudget(daily_cap, self.cache_dir / "_budget.json" if self.cache_dir else None)

    # -- public ------------------------------------------------------------------

    def key_for(self, params: dict) -> str:
        return self.name + ":" + json.dumps(params, sort_keys=True, separators=(",", ":"))

    def get(self, params: dict, deadline: float | None = None) -> SourceResult:
        """`deadline` is a time.monotonic() value; it bounds only the wait for a cold key."""
        key = self.key_for(params)
        entry = self._lookup(key)
        if entry is not None and self.clock() - entry.fetched_at < self.ttl:
            return self._result(key, entry)
        future = self._start_refresh(key, params)
        if future is not None and entry is None:
            wait([future], timeout=self._wait_budget(deadline))
        entry = self._lookup(key)
        if entry is None:
            raise NoData(self.name)
        return self._result(key, entry)

    def health(self) -> dict:
        with self._lock:
            newest = max((e.fetched_at for e in self._entries.values()), default=None)
            return {
                "entries": len(self._files) if self.cache_dir else len(self._entries),
                "newest_fetch": newest.isoformat() if newest else None,
                "upstream_calls": self.upstream_calls,
                "calls_today": self._budget.calls,
                "refreshing": len(self._inflight),
            }

    # -- refresh -----------------------------------------------------------------

    def _result(self, key: str, entry: _Entry) -> SourceResult:
        age = self.clock() - entry.fetched_at
        if age < self.ttl:
            return SourceResult(key, entry.data, entry.fetched_at, False)
        failed = entry.last_failure is not None and entry.last_failure >= entry.fetched_at
        return SourceResult(key, entry.data, entry.fetched_at, failed or age >= self.stale_after)

    def _wait_budget(self, deadline: float | None) -> float:
        budget = self.cold_wait_s
        if deadline is not None:
            budget = min(budget, deadline - time.monotonic())
        return max(0.0, budget)

    def _start_refresh(self, key: str, params: dict) -> Future | None:
        now = self.clock()
        with self._lock:
            running = self._inflight.get(key)
            if running is not None:
                return running
            entry = self._entries.get(key)
            last_failure = entry.last_failure if entry else self._miss_failures.get(key)
            if last_failure is not None and now - last_failure < self.retry_after:
                return None
            if not self._budget.take(now):
                log.warning("%s: daily upstream budget reached; serving cached data", self.name)
                self._record_failure(key, now)
                return None
            self.upstream_calls += 1
            future: Future = Future()
            self._inflight[key] = future
        self.executor.submit(self._refresh, key, params, future)
        return future

    def _refresh(self, key: str, params: dict, future: Future) -> None:
        try:
            data = self.fetch(params)
        except Exception as exc:  # any upstream problem degrades to stale / NoData
            log.warning("%s: fetch failed for %s: %r", self.name, key, exc)
            with self._lock:
                self._record_failure(key, self.clock())
        else:
            self._store(key, _Entry(data, self.clock()))
            with self._lock:
                self._miss_failures.pop(key, None)
        finally:
            with self._lock:
                self._inflight.pop(key, None)
            future.set_result(None)

    def _record_failure(self, key: str, now: datetime) -> None:
        """Caller holds the lock."""
        entry = self._entries.get(key)
        if entry is not None:
            entry.last_failure = now
            return
        self._miss_failures[key] = now
        self._miss_failures.move_to_end(key)
        while len(self._miss_failures) > self.max_entries:
            self._miss_failures.popitem(last=False)

    # -- storage -----------------------------------------------------------------

    def _fname(self, key: str) -> str:
        return hashlib.sha256(key.encode()).hexdigest()[:32] + ".json"

    def _scan_disk(self) -> None:
        for stray in self.cache_dir.glob(".*.tmp"):
            stray.unlink(missing_ok=True)
        found = sorted(self.cache_dir.glob("[0-9a-f]*.json"), key=lambda p: p.stat().st_mtime)
        for old in found[:-self.max_entries] if len(found) > self.max_entries else []:
            old.unlink(missing_ok=True)
        for p in found[-self.max_entries:]:
            self._files[p.name] = None

    def _lookup(self, key: str) -> _Entry | None:
        fname = self._fname(key)
        with self._lock:
            entry = self._entries.get(key)
            if entry is not None:
                self._entries.move_to_end(key)
                if fname in self._files:
                    self._files.move_to_end(fname)
                return entry
            on_disk = fname in self._files
        if not on_disk:
            return None
        entry = self._load(key, fname)
        if entry is not None:
            self._store(key, entry, persist=False)
        return entry

    def _load(self, key: str, fname: str) -> _Entry | None:
        path = self.cache_dir / fname
        try:
            raw = json.loads(path.read_text())
            if raw["key"] != key:
                return None
            return _Entry(raw["data"], datetime.fromisoformat(raw["fetched_at"]))
        except (OSError, ValueError, KeyError) as exc:
            log.warning("%s: ignoring unreadable cache file %s: %r", self.name, fname, exc)
            return None

    def _store(self, key: str, entry: _Entry, persist: bool = True) -> None:
        fname = self._fname(key)
        doomed: list[str] = []
        with self._lock:
            self._entries[key] = entry
            self._entries.move_to_end(key)
            while len(self._entries) > self.max_entries:
                self._entries.popitem(last=False)
            if self.cache_dir:
                self._files[fname] = key
                self._files.move_to_end(fname)
                while len(self._files) > self.max_entries:
                    old_name, old_key = self._files.popitem(last=False)
                    doomed.append(old_name)
                    if old_key is not None:
                        self._entries.pop(old_key, None)
        for old_name in doomed:
            (self.cache_dir / old_name).unlink(missing_ok=True)
        if persist and self.cache_dir:
            _atomic_write(self.cache_dir / fname, json.dumps(
                {"key": key, "data": entry.data, "fetched_at": entry.fetched_at.isoformat()}))
