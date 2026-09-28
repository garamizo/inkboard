import threading
import time
from concurrent.futures import ThreadPoolExecutor
from datetime import timedelta

import pytest

from inkboard_server.sources.base import CachedSource, InlineExecutor, NoData


class Upstream:
    def __init__(self):
        self.calls = 0
        self.fail = False
        self.delay = 0.0
        self.block: dict[int, threading.Event] = {}

    def __call__(self, params):
        self.calls += 1
        if params.get("a") in self.block:
            self.block[params["a"]].wait(5)
        if self.delay:
            time.sleep(self.delay)
        if self.fail:
            raise RuntimeError("upstream down")
        return {"p": params}


def make(clock, tmp_path, up, **kw):
    kw.setdefault("ttl", timedelta(minutes=30))
    kw.setdefault("executor", InlineExecutor())
    return CachedSource("t", up, cache_dir=tmp_path, clock=clock, **kw)


def files(tmp_path):
    return sorted(p.name for p in (tmp_path / "t").glob("[0-9a-f]*.json"))


def test_key_is_order_independent(clock, tmp_path):
    src = make(clock, tmp_path, Upstream())
    assert src.key_for({"a": 1, "b": 2}) == src.key_for({"b": 2, "a": 1}) == 't:{"a":1,"b":2}'


def test_fresh_hit_does_not_refetch(clock, tmp_path):
    up = Upstream()
    src = make(clock, tmp_path, up)
    first = src.get({"a": 1})
    clock.advance(minutes=29)
    second = src.get({"a": 1})
    assert up.calls == 1
    assert second.fetched_at == first.fetched_at and not second.stale


def test_expired_refetches(clock, tmp_path):
    up = Upstream()
    src = make(clock, tmp_path, up)
    first = src.get({"a": 1})
    clock.advance(minutes=31)
    second = src.get({"a": 1})
    assert up.calls == 2
    assert second.fetched_at > first.fetched_at and not second.stale


def test_failure_returns_last_good_as_stale(clock, tmp_path):
    up = Upstream()
    src = make(clock, tmp_path, up)
    first = src.get({"a": 1})
    clock.advance(minutes=31)
    up.fail = True
    r = src.get({"a": 1})
    assert r.stale and r.data == first.data and r.fetched_at == first.fetched_at
    assert r.version != first.version


def test_failure_retry_spacing(clock, tmp_path):
    up = Upstream()
    src = make(clock, tmp_path, up)
    src.get({"a": 1})
    clock.advance(minutes=31)
    up.fail = True
    src.get({"a": 1})
    calls = up.calls
    for _ in range(10):
        assert src.get({"a": 1}).stale
    assert up.calls == calls  # within retry_after: no upstream hammering
    clock.advance(minutes=6)
    src.get({"a": 1})
    assert up.calls == calls + 1


def test_never_succeeded_raises_nodata(clock, tmp_path):
    up = Upstream()
    up.fail = True
    with pytest.raises(NoData) as exc:
        make(clock, tmp_path, up).get({"a": 1})
    assert exc.value.source == "t"


def test_missing_key_retry_spacing(clock, tmp_path):
    up = Upstream()
    up.fail = True
    src = make(clock, tmp_path, up)
    for _ in range(5):
        with pytest.raises(NoData):
            src.get({"a": 1})
    assert up.calls == 1
    clock.advance(minutes=6)
    up.fail = False
    assert not src.get({"a": 1}).stale
    assert up.calls == 2


def test_failed_keys_are_bounded(clock, tmp_path):
    up = Upstream()
    up.fail = True
    src = make(clock, tmp_path, up, max_entries=2)
    for k in range(100):
        with pytest.raises(NoData):
            src.get({"a": k})
    assert len(src._miss_failures) == 2
    assert src._inflight == {}


def test_stale_after_even_without_a_reported_failure(clock, tmp_path):
    up = Upstream()
    src = make(clock, tmp_path, up, retry_after=timedelta(hours=10))
    src.get({"a": 1})
    clock.advance(minutes=31)
    up.fail = True
    assert src.get({"a": 1}).stale  # failed refresh
    src2 = make(clock, tmp_path / "other", Upstream())
    src2.get({"a": 1})
    src2._start_refresh = lambda key, params: None  # refreshes never start
    clock.advance(minutes=115)
    assert not src2.get({"a": 1}).stale  # 115 min < TTL + 90 min
    clock.advance(minutes=10)
    assert src2.get({"a": 1}).stale      # 125 min >= TTL + 90 min


def test_expired_entry_returns_immediately_while_refreshing(clock, tmp_path):
    up = Upstream()
    pool = ThreadPoolExecutor(2)
    src = make(clock, tmp_path, up, executor=pool)
    first = src.get({"a": 1})
    clock.advance(minutes=31)
    up.block[1] = threading.Event()
    t0 = time.monotonic()
    r = src.get({"a": 1}, deadline=time.monotonic() + 0.1)
    assert time.monotonic() - t0 < 0.4
    assert r.fetched_at == first.fetched_at and not r.stale  # refresh in flight, not failed
    up.block[1].set()
    pool.shutdown(wait=True)
    assert src.get({"a": 1}).fetched_at > first.fetched_at


def test_cold_wait_bounded_by_deadline(clock, tmp_path):
    up = Upstream()
    up.delay = 1.0
    pool = ThreadPoolExecutor(2)
    src = make(clock, tmp_path, up, executor=pool)
    t0 = time.monotonic()
    with pytest.raises(NoData):
        src.get({"a": 1}, deadline=time.monotonic() + 0.1)
    assert time.monotonic() - t0 < 0.5
    pool.shutdown(wait=True)  # the fetch finished in the background
    assert src.get({"a": 1}).data == {"p": {"a": 1}}
    assert up.calls == 1


def test_eviction_during_refresh_keeps_single_flight(clock, tmp_path):
    up = Upstream()
    pool = ThreadPoolExecutor(4)
    src = make(clock, tmp_path, up, executor=pool, max_entries=2)
    src.get({"a": 1}, deadline=time.monotonic() + 2)
    clock.advance(minutes=31)
    up.block[1] = threading.Event()
    src.get({"a": 1})                                   # refresh of a=1 starts and blocks
    src.get({"a": 2}, deadline=time.monotonic() + 2)    # these two evict a=1
    src.get({"a": 3}, deadline=time.monotonic() + 2)
    with pytest.raises(NoData):
        src.get({"a": 1}, deadline=time.monotonic() + 0.1)  # evicted; its refresh is still in flight
    assert up.calls == 4                                # no second fetch for a=1
    up.block[1].set()
    pool.shutdown(wait=True)
    assert src.get({"a": 1}).data == {"p": {"a": 1}}


def test_single_flight_cold(clock, tmp_path):
    release = threading.Event()
    calls = []

    def slow(params):
        calls.append(1)
        release.wait(5)
        return 42

    src = CachedSource("t", slow, ttl=timedelta(minutes=30), cache_dir=tmp_path, clock=clock,
                       executor=ThreadPoolExecutor(4))
    results = []
    threads = [threading.Thread(target=lambda: results.append(src.get({"a": 1}, deadline=time.monotonic() + 5)))
               for _ in range(4)]
    for t in threads:
        t.start()
    time.sleep(0.1)
    release.set()
    for t in threads:
        t.join(5)
    assert len(calls) == 1
    assert [r.data for r in results] == [42] * 4


def test_disk_round_trip(clock, tmp_path):
    up = Upstream()
    first = make(clock, tmp_path, up).get({"a": 1})
    again = make(clock, tmp_path, up).get({"a": 1})  # new instance, same dir
    assert up.calls == 1
    assert again.fetched_at == first.fetched_at and again.data == first.data


def test_disk_cap_holds_across_restarts(clock, tmp_path):
    up = Upstream()
    for run in range(3):
        src = make(clock, tmp_path, up, max_entries=2)
        src.get({"a": run * 10})
        src.get({"a": run * 10 + 1})
        clock.advance(seconds=1)
    assert len(files(tmp_path)) == 2


def test_startup_prunes_oldest_files(clock, tmp_path):
    up = Upstream()
    src = make(clock, tmp_path, up, max_entries=10)
    for k in range(5):
        src.get({"a": k})
        time.sleep(0.01)  # distinct mtimes
    (tmp_path / "t" / ".stray.tmp").write_text("x")
    make(clock, tmp_path, up, max_entries=2)
    assert len(files(tmp_path)) == 2
    assert not (tmp_path / "t" / ".stray.tmp").exists()
    newest = make(clock, tmp_path, up, max_entries=2)
    newest.get({"a": 4})
    assert up.calls == 5  # a=4 was kept on disk, so no refetch


def test_daily_cap_persists_across_restarts(clock, tmp_path):
    up = Upstream()
    src = make(clock, tmp_path, up, daily_cap=4)   # new keys may use 2 of the 4
    src.get({"a": 1})
    src.get({"a": 2})
    restarted = make(clock, tmp_path, up, daily_cap=4)  # without persistence a=3 would be allowed
    with pytest.raises(NoData):
        restarted.get({"a": 3})
    assert up.calls == 2
    clock.advance(days=1)
    assert not restarted.get({"a": 4}).stale
    assert up.calls == 3


def test_health(clock, tmp_path):
    src = make(clock, tmp_path, Upstream(), daily_cap=10)
    src.get({"a": 1})
    h = src.health()
    assert h == {"entries": 1, "newest_fetch": clock().isoformat(), "upstream_calls": 1,
                 "calls_today": 1, "refreshing": 0}


def test_hourly_wakes_with_real_pool_stay_fresh(clock, tmp_path):
    # Review C1: a board waking every ~60 min (TTL 30 min) must get the refresh it
    # triggered, not the previous wake's data flagged stale.
    up = Upstream()
    up.delay = 0.05
    pool = ThreadPoolExecutor(2)
    src = make(clock, tmp_path, up, executor=pool)
    for jitter in (0, 25, 3, 30, 11, 0):
        r = src.get({"a": 1}, deadline=time.monotonic() + 10)
        assert not r.stale
        assert r.fetched_at == clock()
        clock.advance(minutes=60, seconds=jitter)
    pool.shutdown(wait=True)


def test_hung_refresh_serves_cached_within_deadline(clock, tmp_path):
    up = Upstream()
    pool = ThreadPoolExecutor(2)
    src = make(clock, tmp_path, up, executor=pool)
    first = src.get({"a": 1}, deadline=time.monotonic() + 5)
    clock.advance(minutes=31)
    up.block[1] = threading.Event()
    t0 = time.monotonic()
    r = src.get({"a": 1}, deadline=time.monotonic() + 0.2)
    assert time.monotonic() - t0 < 0.5
    assert r.fetched_at == first.fetched_at and not r.stale
    up.block[1].set()
    pool.shutdown(wait=True)


def test_new_keys_cannot_spend_the_refresh_reserve(clock, tmp_path):
    # Review I3: anonymous clients inventing locations must not starve refreshes of
    # keys that real boards already use.
    up = Upstream()
    src = make(clock, tmp_path, up, daily_cap=4)   # new keys may use at most half
    src.get({"a": 1})
    src.get({"a": 2})
    with pytest.raises(NoData):
        src.get({"a": 3})                          # third new key: over the 50% share
    clock.advance(minutes=31)
    assert not src.get({"a": 1}).stale             # refresh of a known key still allowed
    assert not src.get({"a": 2}).stale
    assert up.calls == 4
