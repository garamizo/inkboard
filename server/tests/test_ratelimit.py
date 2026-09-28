from inkboard_server.ratelimit import RateLimiter


class Tick:
    def __init__(self):
        self.t = 0.0

    def __call__(self):
        return self.t


def test_bucket_allows_capacity_then_waits():
    tick = Tick()
    rl = RateLimiter(capacity=3, per_seconds=3600, clock=tick)
    assert [rl.check("a") for _ in range(3)] == [None, None, None]
    wait = rl.check("a")
    assert wait is not None and 1199 <= wait <= 1201  # one token per 1200 s
    assert rl.check("b") is None  # per-key


def test_bucket_refills():
    tick = Tick()
    rl = RateLimiter(capacity=2, per_seconds=3600, clock=tick)
    rl.check("a"); rl.check("a")
    assert rl.check("a") is not None
    tick.t += 1800
    assert rl.check("a") is None


def test_prunes_idle_keys():
    tick = Tick()
    rl = RateLimiter(capacity=2, per_seconds=3600, clock=tick, max_keys=10)
    for i in range(10):
        rl.check(f"k{i}")
    tick.t += 7200
    rl.check("new")
    assert len(rl._buckets) <= 10
