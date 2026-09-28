import io
import time
from concurrent.futures import ThreadPoolExecutor

import pytest
from fastapi.testclient import TestClient
from PIL import Image

from inkboard_server.app import create_app, etag_matches
from inkboard_server.ratelimit import RateLimiter
from inkboard_server.sources import make_sources

DEFAULT_Q = ("w=market_trends:2/3,calendar_weather:1/3&lat=34.05&lon=-118.24"
             "&tz=America/Los_Angeles&units=imperial")


@pytest.fixture
def client(sources, clock):
    return TestClient(create_app(sources, clock=clock, limiter=RateLimiter(capacity=1000)))


def test_frame_bin_200(client):
    r = client.get(f"/v1/frame.bin?{DEFAULT_Q}")
    assert r.status_code == 200
    assert r.headers["content-type"] == "application/octet-stream"
    assert len(r.content) == 48000
    assert r.headers["etag"].startswith('"') and len(r.headers["etag"]) == 18
    assert r.headers["cache-control"] == "no-cache"
    assert 300 <= int(r.headers["x-next-refresh-seconds"]) <= 21600
    assert int(r.headers["x-next-refresh-seconds"]) == 61 * 60  # FIXED_NOW is 10:00 LA
    assert r.headers["x-utc-offset-seconds"] in ("-25200", "-28800")


def test_304_on_matching_etag(client):
    first = client.get(f"/v1/frame.bin?{DEFAULT_Q}")
    again = client.get(f"/v1/frame.bin?{DEFAULT_Q}", headers={"If-None-Match": first.headers["etag"]})
    assert again.status_code == 304 and again.content == b""
    assert again.headers["etag"] == first.headers["etag"]
    assert "x-next-refresh-seconds" in again.headers


def test_etag_matches():
    assert etag_matches('"abc"', '"abc"')
    assert etag_matches('W/"abc", "def"', '"abc"')
    assert etag_matches("*", '"abc"')
    assert not etag_matches(None, '"abc"') and not etag_matches('"x"', '"abc"')


def test_staleness_cannot_hide(client, upstream, clock, golden):
    first = client.get(f"/v1/frame.bin?{DEFAULT_Q}")
    clock.advance(hours=7)  # past both TTLs, still the same local date (17:00)
    upstream.fail = True
    r = client.get(f"/v1/frame.bin?{DEFAULT_Q}", headers={"If-None-Match": first.headers["etag"]})
    assert r.status_code == 200
    assert r.headers["etag"] != first.headers["etag"]
    png = client.get(f"/v1/frame.png?{DEFAULT_Q}")
    footer = Image.open(io.BytesIO(png.content)).convert("1").crop((0, 464, 400, 480))
    golden("api_footer_stale", footer)


def test_refetch_with_same_data_changes_etag(client, clock):
    first = client.get(f"/v1/frame.bin?{DEFAULT_Q}")
    clock.advance(minutes=31)  # weather TTL expires; refetch succeeds with identical data
    r = client.get(f"/v1/frame.bin?{DEFAULT_Q}", headers={"If-None-Match": first.headers["etag"]})
    assert r.status_code == 200 and r.headers["etag"] != first.headers["etag"]


def test_cross_timezone_weather_entries(client, upstream):
    base = "/v1/frame.bin?w=calendar_weather:1&lat=34.05&lon=-118.24&units=imperial&tz="
    assert client.get(base + "America/Los_Angeles").status_code == 200
    assert client.get(base + "Asia/Tokyo").status_code == 200  # 02:00 next day in Tokyo
    assert upstream.calls["weather"] == 2


def test_cold_start_upstream_down(client, upstream):
    upstream.fail = True
    r = client.get(f"/v1/frame.bin?{DEFAULT_Q}")
    assert r.status_code == 200 and len(r.content) == 48000


class SlowUpstream:
    """Answers from fixtures until .hang is set; then every call sleeps, then fails."""

    def __init__(self, inner):
        self.inner, self.hang = inner, False

    def fred(self, params):
        if self.hang:
            time.sleep(2)
            raise RuntimeError("timeout")
        return self.inner.fred(params)

    def weather(self, params):
        if self.hang:
            time.sleep(2)
            raise RuntimeError("timeout")
        return self.inner.weather(params)


def test_slow_upstream_after_expiry_answers_fast(tmp_path, upstream, clock):
    slow = SlowUpstream(upstream)
    pool = ThreadPoolExecutor(8)
    src = make_sources(slow.fred, slow.weather, cache_dir=tmp_path, clock=clock, executor=pool)
    c = TestClient(create_app(src, clock=clock, limiter=RateLimiter(capacity=1000), request_budget_s=0.5))
    assert c.get(f"/v1/frame.bin?{DEFAULT_Q}").status_code == 200  # cold, fast upstream
    clock.advance(hours=7)                                         # every entry expired
    slow.hang = True
    t0 = time.monotonic()
    r = c.get(f"/v1/frame.bin?{DEFAULT_Q}")
    assert r.status_code == 200 and len(r.content) == 48000
    assert time.monotonic() - t0 < 1.0                             # served cached data, did not wait
    pool.shutdown(wait=True)


def test_slow_upstream_cold_is_bounded_by_budget(tmp_path, upstream, clock):
    slow = SlowUpstream(upstream)
    slow.hang = True
    pool = ThreadPoolExecutor(8)
    src = make_sources(slow.fred, slow.weather, cache_dir=tmp_path, clock=clock, executor=pool)
    c = TestClient(create_app(src, clock=clock, limiter=RateLimiter(capacity=1000), request_budget_s=0.5))
    t0 = time.monotonic()
    r = c.get(f"/v1/frame.bin?{DEFAULT_Q}")
    assert r.status_code == 200 and len(r.content) == 48000        # "No data yet" columns
    assert time.monotonic() - t0 < 1.5                             # one budget, not one per source
    pool.shutdown(wait=True)


def test_bad_query_400(client):
    r = client.get("/v1/frame.bin?lat=1")
    assert r.status_code == 400
    assert r.headers["content-type"].startswith("text/plain")
    assert r.text.startswith("w: required")


def test_rate_limit_429(sources, clock):
    c = TestClient(create_app(sources, clock=clock, limiter=RateLimiter(capacity=2)))
    assert c.get(f"/v1/frame.bin?{DEFAULT_Q}").status_code == 200
    assert c.get("/v1/frame.bin?bad").status_code == 400  # bad requests also spend tokens
    r = c.get(f"/v1/frame.bin?{DEFAULT_Q}")
    assert r.status_code == 429 and int(r.headers["retry-after"]) > 0
    assert c.get("/healthz").status_code == 200  # exempt


def test_rate_limit_keys_on_client_ip_header(sources, clock):
    c = TestClient(create_app(sources, clock=clock, limiter=RateLimiter(capacity=1),
                              client_ip_header="CF-Connecting-IP"))
    a, b = {"CF-Connecting-IP": "203.0.113.1"}, {"CF-Connecting-IP": "203.0.113.2"}
    assert c.get(f"/v1/frame.bin?{DEFAULT_Q}", headers=a).status_code == 200
    assert c.get(f"/v1/frame.bin?{DEFAULT_Q}", headers=a).status_code == 429
    assert c.get(f"/v1/frame.bin?{DEFAULT_Q}", headers=b).status_code == 200  # different client


def test_png_matches_bin(client):
    bits = client.get(f"/v1/frame.bin?{DEFAULT_Q}").content
    png = client.get(f"/v1/frame.png?{DEFAULT_Q}")
    assert png.headers["content-type"] == "image/png"
    assert Image.open(io.BytesIO(png.content)).convert("1").tobytes() == bits


def test_frame_cache_reuses_render(client, monkeypatch):
    import inkboard_server.app as app_mod
    calls = []
    real = app_mod.render_frame
    monkeypatch.setattr(app_mod, "render_frame", lambda *a: calls.append(1) or real(*a))
    client.get(f"/v1/frame.bin?{DEFAULT_Q}")
    client.get(f"/v1/frame.png?{DEFAULT_Q}")
    assert len(calls) == 1


def test_calibration_endpoints(client):
    assert len(client.get("/v1/test.bin").content) == 48000
    assert client.get("/v1/test.png").headers["content-type"] == "image/png"


def test_healthz_and_index(client):
    h = client.get("/healthz").json()
    assert set(h) == {"fred", "weather"}
    page = client.get("/")
    assert page.status_code == 200 and "market_trends" in page.text and "calendar_weather" in page.text
