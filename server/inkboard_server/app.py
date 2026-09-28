"""HTTP API v1 (spec §2): frames, calibration pattern, health, index."""
from __future__ import annotations

import html
import logging
import math
import threading
import time
from collections import OrderedDict
from dataclasses import dataclass
from functools import lru_cache
from typing import Callable

from fastapi import FastAPI, Request
from fastapi.responses import HTMLResponse, JSONResponse, PlainTextResponse, Response
from PIL import Image

from .compositor import fetch_widgets, frame_versions, render_frame
from .frame import calibration_pattern, etag_for, pack, png_bytes
from .query import QueryError, parse_query
from .ratelimit import RateLimiter
from .schedule import next_refresh_seconds, utc_offset_seconds
from .series import CATALOG
from .sources.base import utcnow
from .widgets import REGISTRY, RenderContext

log = logging.getLogger("inkboard.access")
FRAME_CACHE_SIZE = 500
REQUEST_BUDGET_S = 10.0  # the board gives up after 20 s (spec §6.2)


@dataclass(frozen=True)
class RenderedFrame:
    bits: bytes
    etag: str
    image: Image.Image


class FrameCache:
    def __init__(self, cap: int):
        self.cap = cap
        self._d: OrderedDict = OrderedDict()
        self._lock = threading.Lock()

    def get(self, key):
        with self._lock:
            v = self._d.get(key)
            if v is not None:
                self._d.move_to_end(key)
            return v

    def put(self, key, value) -> None:
        with self._lock:
            self._d[key] = value
            self._d.move_to_end(key)
            while len(self._d) > self.cap:
                self._d.popitem(last=False)


def etag_matches(header: str | None, etag: str) -> bool:
    if not header:
        return False
    tags = [t.strip().removeprefix("W/") for t in header.split(",")]
    return "*" in tags or etag in tags


@lru_cache(maxsize=1)
def _calibration() -> RenderedFrame:
    img = calibration_pattern()
    bits = pack(img)
    return RenderedFrame(bits, etag_for(bits), img)


def create_app(sources, *, clock: Callable = utcnow, limiter: RateLimiter | None = None,
               registry=None, client_ip_header: str | None = None,
               request_budget_s: float = REQUEST_BUDGET_S) -> FastAPI:
    registry = REGISTRY if registry is None else registry
    limiter = limiter or RateLimiter()
    cache = FrameCache(FRAME_CACHE_SIZE)
    app = FastAPI(title="inkboard", docs_url=None, redoc_url=None, openapi_url=None)

    @app.middleware("http")
    async def access_log(request: Request, call_next):
        start = time.perf_counter()
        response = await call_next(request)
        # no client IP in logs (spec §3.5)
        log.info("%s %s?%s %d %.0fms", request.method, request.url.path, request.url.query,
                 response.status_code, (time.perf_counter() - start) * 1000)
        return response

    def client_key(request: Request) -> str:
        # Behind the Cloudflare tunnel every socket peer is cloudflared; CF-Connecting-IP is
        # set (overwritten) by Cloudflare, so it is the real client. Never X-Forwarded-For.
        if client_ip_header and (value := request.headers.get(client_ip_header)):
            return value.strip()
        return request.client.host if request.client else "unknown"

    def rate_limited(request: Request) -> Response | None:
        wait = limiter.check(client_key(request))
        if wait is None:
            return None
        return PlainTextResponse("rate limited", status_code=429, headers={"Retry-After": str(math.ceil(wait))})

    def build(request: Request):
        try:
            req = parse_query(request.url.query, registry)
        except QueryError as e:
            return PlainTextResponse(str(e), status_code=400)
        now = clock().astimezone(req.tz)
        ctx = RenderContext(today=now.date(), tz=req.tz)
        widgets = [registry[s.type_name](s.size, req.options) for s in req.widgets]
        # Expired entries are served at once (refreshed in the background); only cold keys
        # wait, and never past this request's deadline.
        results = fetch_widgets(widgets, sources.scoped(time.monotonic() + request_budget_s), ctx)
        key = (req.canonical, ctx.today.isoformat(), frame_versions(results))
        frame = cache.get(key)
        if frame is None:
            img = render_frame(widgets, results, ctx)
            bits = pack(img)
            frame = RenderedFrame(bits, etag_for(bits), img)
            cache.put(key, frame)
        headers = {
            "ETag": frame.etag,
            "Cache-Control": "no-cache",
            "X-Next-Refresh-Seconds": str(next_refresh_seconds(now)),
            "X-UTC-Offset-Seconds": str(utc_offset_seconds(now)),
        }
        return frame, headers

    def serve(request: Request, as_png: bool) -> Response:
        limited = rate_limited(request)
        if limited:
            return limited
        out = build(request)
        if isinstance(out, Response):
            return out
        frame, headers = out
        if etag_matches(request.headers.get("if-none-match"), frame.etag):
            return Response(status_code=304, headers=headers)
        if as_png:
            return Response(png_bytes(frame.image), media_type="image/png", headers=headers)
        return Response(frame.bits, media_type="application/octet-stream", headers=headers)

    @app.get("/v1/frame.bin")
    def frame_bin(request: Request) -> Response:
        return serve(request, as_png=False)

    @app.get("/v1/frame.png")
    def frame_png(request: Request) -> Response:
        return serve(request, as_png=True)

    @app.get("/v1/test.bin")
    def test_bin() -> Response:
        f = _calibration()
        return Response(f.bits, media_type="application/octet-stream", headers={"ETag": f.etag})

    @app.get("/v1/test.png")
    def test_png() -> Response:
        return Response(png_bytes(_calibration().image), media_type="image/png")

    @app.get("/healthz")
    def healthz() -> JSONResponse:
        return JSONResponse(sources.health())

    @app.get("/", response_class=HTMLResponse)
    def index() -> str:
        return _index_html(registry)

    return app


def _index_html(registry) -> str:
    example = ("/v1/frame.png?w=market_trends:2/3,calendar_weather:1/3&lat=34.05&lon=-118.24"
               "&tz=America/Los_Angeles&units=imperial")
    widgets = "".join(
        f"<li><code>{html.escape(name)}</code>: sizes "
        f"{', '.join(sorted(s.token for s in cls.supported_sizes))}; params "
        f"{', '.join(sorted(cls.params)) or 'none'}</li>"
        for name, cls in sorted(registry.items()))
    series = "".join(f"<li><code>{s.id}</code>: {html.escape(s.label)} (FRED {s.fred_id})</li>"
                     for s in CATALOG.values())
    return (
        "<!doctype html><meta charset=utf-8><title>inkboard</title>"
        "<body style='font-family:sans-serif;max-width:720px;margin:2rem auto;padding:0 1rem'>"
        "<h1>inkboard</h1><p>Render server for 800×480 e-paper boards. "
        "Each board sends its layout in the query string.</p>"
        f"<p>Example: <a href='{html.escape(example)}'><code>{html.escape(example)}</code></a></p>"
        f"<h2>Widgets</h2><ul>{widgets}</ul><h2>Series</h2><ul>{series}</ul>"
        "<p>Global params: <code>w</code> (required), <code>tz</code> (IANA, default UTC).</p>"
        "<p>Data: FRED, S&amp;P Dow Jones Indices, Coinbase, Freddie Mac, Realtor.com, Open-Meteo. "
        "Free, non-commercial.</p></body>"
    )
