"""In-process per-key token bucket (spec §3.5)."""
import threading
import time
from typing import Callable


class RateLimiter:
    def __init__(self, capacity: int = 30, per_seconds: float = 3600, clock: Callable[[], float] = time.monotonic,
                 max_keys: int = 10000):
        self.capacity = capacity
        self.rate = capacity / per_seconds  # tokens per second
        self.clock = clock
        self.max_keys = max_keys
        self._buckets: dict[str, tuple[float, float]] = {}
        self._lock = threading.Lock()

    def check(self, key: str) -> float | None:
        now = self.clock()
        with self._lock:
            tokens, last = self._buckets.get(key, (float(self.capacity), now))
            tokens = min(self.capacity, tokens + (now - last) * self.rate)
            if tokens >= 1:
                self._buckets[key] = (tokens - 1, now)
                if len(self._buckets) > self.max_keys:
                    self._prune(now)
                return None
            self._buckets[key] = (tokens, now)
            return (1 - tokens) / self.rate

    def _prune(self, now: float) -> None:
        full = [k for k, (tok, last) in self._buckets.items()
                if tok + (now - last) * self.rate >= self.capacity]
        for k in full:
            del self._buckets[k]
