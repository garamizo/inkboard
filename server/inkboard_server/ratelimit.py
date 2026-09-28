"""In-process per-client token bucket (spec §3.5)."""
import ipaddress
import threading
import time
from collections import OrderedDict
from typing import Callable


def client_bucket(key: str) -> str:
    """IPv6 clients are limited per /64 (one host usually controls a whole /64)."""
    try:
        ip = ipaddress.ip_address(key)
    except ValueError:
        return key
    if ip.version == 6:
        return str(ipaddress.ip_network(f"{ip}/64", strict=False))
    return key


class RateLimiter:
    def __init__(self, capacity: int = 30, per_seconds: float = 3600, clock: Callable[[], float] = time.monotonic,
                 max_keys: int = 10000):
        self.capacity = capacity
        self.rate = capacity / per_seconds  # tokens per second
        self.clock = clock
        self.max_keys = max_keys
        self._buckets: OrderedDict[str, tuple[float, float]] = OrderedDict()  # LRU, bounded
        self._lock = threading.Lock()

    def check(self, key: str) -> float | None:
        """None if allowed, otherwise the seconds until the next token."""
        key = client_bucket(key)
        now = self.clock()
        with self._lock:
            tokens, last = self._buckets.get(key, (float(self.capacity), now))
            tokens = min(self.capacity, tokens + (now - last) * self.rate)
            allowed = tokens >= 1
            self._buckets[key] = (tokens - 1 if allowed else tokens, now)
            self._buckets.move_to_end(key)
            while len(self._buckets) > self.max_keys:
                self._buckets.popitem(last=False)
            return None if allowed else (1 - tokens) / self.rate
