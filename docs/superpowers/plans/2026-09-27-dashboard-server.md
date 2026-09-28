# Dashboard Server Implementation Plan (plan 1 of 2)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the public render server that turns a board's query string into an
800×480 1-bit frame (`/v1/frame.bin`, `/v1/frame.png`), with the `market_trends` and
`calendar_weather` widgets.

**Architecture:**
- A stateless FastAPI app parses the query into a `FrameRequest` and instantiates the
  registered widgets.
- Each widget fetches through `CachedSource`s (TTL, last-good fallback, disk
  persistence), then renders into its column of a Pillow "L" image.
- The compositor adds the dividers and footer, and thresholds the image to 1 bit.
- Frames are cached by (canonical query, local date, source versions), and the ETag is
  a hash of the frame bytes.

**Tech Stack:** Python 3.12, uv, FastAPI + uvicorn, Pillow, httpx, pytest, Docker.

**Spec:** `docs/superpowers/specs/2026-09-27-dashboard-design.md`. Read it before
starting any task. Section numbers below (§) refer to it.

**Plan 2 (firmware)** is written after this plan ships and `/v1/test.bin` has been
checked on hardware. It is not part of this plan.

## Global Constraints

- All server code lives in `server/` of the `feat/dashboard` worktree
  (`/home/garamizo/inkboard-dashboard`). Run every command from `server/` unless a
  step says otherwise.
- Python `>=3.12`. Dependencies are exactly: fastapi, uvicorn[standard], pillow and
  httpx; the dev group adds pytest. Add nothing else.
- Frame geometry: 800×480, 1 bpp, rows top to bottom, MSB = leftmost pixel, **bit 1 =
  white**, 48,000 bytes (§2.3).
- Columns are 266 / 534 / 800 px wide, and widgets are 464 px tall. The footer takes
  y 464–480.
- The threshold is 140 (`> 140` → white). No dithering.
- The ETag is `"` + the first 16 hex characters of the sha256 of the frame bytes + `"`.
- `X-Next-Refresh-Seconds` is always in 300–21600.
- Rate limit: 30 requests per IP per hour on `/v1/frame.*`. The client IP comes from
  the header in `INKBOARD_CLIENT_IP_HEADER` (`CF-Connecting-IP` in production), never from
  `X-Forwarded-For`. Query length is at most 1024. At most 3 widgets and at most 4 series.
- Public exposure goes through a Cloudflare Tunnel (`cloudflared` in the same compose
  project). The server port is bound to `127.0.0.1` only. See `docs/cloudflare-tunnel.md`.
- Cache TTLs: FRED 6 h; weather 30 min. Weather LRU cap 2,000 entries and a daily cap
  of 8,000 upstream calls. Frame cache 500 entries.
- The weather cache key is `(lat rounded to 0.1, lon rounded to 0.1, units, tz)`.
- Default series: `sp500,btc,mortgage30,home_la`. Default years: 5. Default tz: `UTC`.
  Default units: `imperial`.
- Tests never touch the live network.
- Every commit message ends with a blank line followed by
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Review Focus

1. **A request just after local midnight while the weather cache still holds
   yesterday's forecast.** Today's H/L must come from the entry whose date is today,
   the forecast rows must start tomorrow, and nothing may crash when today's entry is
   missing. This is pinned in Task 9 (`test_build_payload_after_midnight`,
   `test_build_payload_without_today`).
2. **FRED returns `"."` for missing values and has holiday or weekend gaps.** These
   must be skipped and forward-filled, never parsed as errors or zeros. Pinned in
   Task 4 (`test_parse_observations_skips_missing`) and Task 7
   (`test_resample_forward_fills`).
3. **Cold start with the upstreams down.** The response must be 200 with "No data yet"
   columns, not a 500. Pinned in Task 11 (`test_cold_start_upstream_down`).
4. **An upstream outage while boards keep polling.** There must be at most one
   upstream attempt per key per retry window, whether or not the key was ever cached.
   Pinned in Task 3 (`test_failure_retry_spacing`, `test_missing_key_retry_spacing`).
5. **Encoded or repeated query params** (`2%2F3`, `w` given twice). The encoded form
   must behave exactly like the decoded one, and a repeat must be a 400, not a silent
   last-wins. Pinned in Task 5 (`test_url_encoded_w`, `test_duplicate_param`).

## File Structure

```
server/
  pyproject.toml            deps + pytest config
  uv.lock                   generated
  .gitignore                .venv, .cache, *.actual.png
  .dockerignore             .venv, .cache, tests/goldens/*.actual.png
  Dockerfile, compose.yaml  deployment (Task 12)
  README.md                 run / test / deploy notes (Task 12)
  inkboard_server/
    __init__.py
    frame.py                geometry, threshold, pack, ETag, PNG, calibration pattern
    schedule.py             next_refresh_seconds, utc_offset_seconds
    series.py               market series catalog
    market_data.py          weekly grid, resample, normalize, summarize, ticks
    query.py                query string → FrameRequest
    compositor.py           fetch_widgets, frame_versions, compose, render_frame
    ratelimit.py            per-IP token bucket
    app.py                  create_app: routes, frame cache, headers
    main.py                 production entry point (env → sources → app)
    fonts/                  DejaVu TTFs + LICENSE
    draw/
      __init__.py
      fonts.py              font(size, bold, condensed)
      lines.py              line styles, markers, legend sample
      icons.py              wmo_info, icon
      calendar.py           month_weeks, month_grid
    sources/
      __init__.py           Sources, make_sources, build_sources
      base.py               SourceResult, NoData, CachedSource
      fred.py               FRED fetch + parse
      openmeteo.py          Open-Meteo fetch + parse + cache params
    widgets/
      __init__.py           re-exports base; imports concrete widgets to register them
      base.py               Size, Box, ParamSpec, RenderContext, WidgetData, Widget, registry
      params.py             reusable query-param parsers
      market_trends.py
      calendar_weather.py
  tests/
    conftest.py             goldens, FakeClock, FakeUpstream, fixture sources
    fixtures/               recorded upstream JSON + record_fixtures.py
    goldens/                committed expected PNGs
    test_*.py
```

---

### Task 1: Scaffold, frame packing, and calibration pattern

**Files:**
- Create: `server/pyproject.toml`, `server/.gitignore`, `server/inkboard_server/__init__.py`,
  `server/inkboard_server/frame.py`, `server/inkboard_server/draw/__init__.py`,
  `server/inkboard_server/draw/fonts.py`, `server/inkboard_server/fonts/*`,
  `server/tests/__init__.py`, `server/tests/conftest.py`
- Test: `server/tests/test_frame.py`

**Interfaces:**
- Produces:
  - `frame.WIDTH = 800`, `frame.HEIGHT = 480`, `frame.FRAME_BYTES = 48000`,
    `frame.THRESHOLD = 140`
  - `frame.new_canvas() -> Image` ("L", white)
  - `frame.to_1bit(img: Image) -> Image` (mode "1")
  - `frame.pack(img1: Image) -> bytes`
  - `frame.etag_for(bits: bytes) -> str`
  - `frame.png_bytes(img1: Image) -> bytes`
  - `frame.calibration_pattern() -> Image` (mode "1")
  - `draw.fonts.font(size: int, bold: bool = False, condensed: bool = False) -> FreeTypeFont`
  - The pytest fixture `golden(name: str, img: Image)` and the option `--update-goldens`

- [ ] **Step 1: Create the project files**

`server/pyproject.toml`:
```toml
[project]
name = "inkboard-server"
version = "0.1.0"
description = "Render server for inkboard e-paper dashboards"
requires-python = ">=3.12"
dependencies = [
    "fastapi>=0.115",
    "uvicorn[standard]>=0.30",
    "pillow>=11",
    "httpx>=0.27",
]

[dependency-groups]
dev = ["pytest>=8"]

[build-system]
requires = ["hatchling"]
build-backend = "hatchling.build"

[tool.hatch.build.targets.wheel]
packages = ["inkboard_server"]

[tool.pytest.ini_options]
testpaths = ["tests"]
```

`server/.gitignore`:
```
.venv/
.cache/
tests/goldens/*.actual.png
__pycache__/
```

`server/inkboard_server/__init__.py`:
```python
"""inkboard render server."""
```

`server/inkboard_server/draw/__init__.py` and `server/tests/__init__.py`: empty files.

Copy the fonts and license:
```bash
mkdir -p inkboard_server/fonts
cp /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf \
   /usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf \
   /usr/share/fonts/truetype/dejavu/DejaVuSansCondensed.ttf \
   /usr/share/fonts/truetype/dejavu/DejaVuSansCondensed-Bold.ttf inkboard_server/fonts/
cp /usr/share/doc/fonts-dejavu-core/copyright inkboard_server/fonts/LICENSE
uv sync
```
Expected: `uv sync` creates `.venv` and `uv.lock` with no errors.

`server/inkboard_server/draw/fonts.py`:
```python
"""Bundled DejaVu fonts, cached per size and weight."""
from functools import lru_cache
from pathlib import Path

from PIL import ImageFont

FONT_DIR = Path(__file__).resolve().parent.parent / "fonts"


@lru_cache(maxsize=None)
def font(size: int, bold: bool = False, condensed: bool = False) -> ImageFont.FreeTypeFont:
    name = "DejaVuSans" + ("Condensed" if condensed else "") + ("-Bold" if bold else "") + ".ttf"
    return ImageFont.truetype(str(FONT_DIR / name), size)
```

`server/tests/conftest.py`:
```python
from pathlib import Path

import pytest
from PIL import Image

GOLDEN_DIR = Path(__file__).parent / "goldens"
FIXTURES = Path(__file__).parent / "fixtures"


def pytest_addoption(parser):
    parser.addoption("--update-goldens", action="store_true", help="rewrite golden PNGs")


@pytest.fixture
def golden(request):
    """Compare a mode-"1" image with tests/goldens/<name>.png pixel for pixel."""
    update = request.config.getoption("--update-goldens")

    def check(name: str, img: Image.Image) -> None:
        path = GOLDEN_DIR / f"{name}.png"
        if update:
            GOLDEN_DIR.mkdir(exist_ok=True)
            img.save(path)
            return
        assert path.exists(), f"missing golden {path.name}: run pytest --update-goldens, then inspect it"
        want = Image.open(path)
        assert (want.mode, want.size) == (img.mode, img.size), f"{name}: mode/size changed"
        if want.tobytes() != img.tobytes():
            actual = path.with_name(f"{name}.actual.png")
            img.save(actual)
            pytest.fail(f"{name} differs from its golden; wrote {actual.name}")

    return check
```

- [ ] **Step 2: Write the failing tests**

`server/tests/test_frame.py`:
```python
import io

import pytest
from PIL import Image

from inkboard_server.frame import (
    FRAME_BYTES, HEIGHT, WIDTH, calibration_pattern, etag_for, new_canvas, pack, png_bytes, to_1bit,
)


def test_pack_is_msb_first_and_white_is_one():
    img = new_canvas()
    img.putpixel((0, 0), 0)
    bits = pack(to_1bit(img))
    assert len(bits) == FRAME_BYTES == 48000
    assert bits[0] == 0x7F
    assert bits[1] == 0xFF
    assert bits[100] == 0xFF  # second row starts at byte 100


def test_threshold_140_is_black_141_is_white():
    img = new_canvas()
    img.putpixel((0, 0), 140)
    img.putpixel((1, 0), 141)
    out = to_1bit(img)
    assert out.getpixel((0, 0)) == 0
    assert out.getpixel((1, 0)) == 255


def test_wrong_size_rejected():
    with pytest.raises(ValueError, match="800x480"):
        to_1bit(Image.new("L", (10, 10), 255))


def test_etag_is_quoted_16_hex_and_stable():
    bits = pack(to_1bit(new_canvas()))
    tag = etag_for(bits)
    assert tag == etag_for(bits)
    assert tag.startswith('"') and tag.endswith('"') and len(tag) == 18
    int(tag[1:-1], 16)


def test_png_matches_packed_bits():
    img1 = to_1bit(new_canvas())
    decoded = Image.open(io.BytesIO(png_bytes(img1))).convert("1")
    assert decoded.size == (WIDTH, HEIGHT)
    assert decoded.tobytes() == pack(img1)


def test_calibration_pattern_corners():
    img = calibration_pattern()
    assert img.mode == "1"
    assert img.getpixel((5, 5)) == 0             # top-left solid square
    assert img.getpixel((WIDTH - 20, 20)) == 255  # top-right bracket is hollow
    assert img.getpixel((WIDTH - 20, HEIGHT - 20)) == 0  # bottom-right dot
```

- [ ] **Step 3: Run the tests to verify they fail**

Run: `uv run pytest tests/test_frame.py -v`
Expected: FAIL with `ModuleNotFoundError: No module named 'inkboard_server.frame'`

- [ ] **Step 4: Implement `frame.py`**

`server/inkboard_server/frame.py`:
```python
"""Frame geometry, 1-bit conversion, packing, ETag and the hardware calibration pattern."""
import hashlib
import io

from PIL import Image, ImageDraw

from .draw.fonts import font

WIDTH, HEIGHT = 800, 480
FRAME_BYTES = WIDTH * HEIGHT // 8
THRESHOLD = 140


def new_canvas() -> Image.Image:
    return Image.new("L", (WIDTH, HEIGHT), 255)


def to_1bit(img: Image.Image) -> Image.Image:
    """Threshold an "L" image into mode "1" (no dithering: text and lines stay crisp)."""
    if img.size != (WIDTH, HEIGHT):
        raise ValueError(f"frame must be {WIDTH}x{HEIGHT}, got {img.size[0]}x{img.size[1]}")
    bw = img.convert("L").point(lambda p: 255 if p > THRESHOLD else 0)
    return bw.convert("1", dither=Image.Dither.NONE)


def pack(img1: Image.Image) -> bytes:
    """Rows top to bottom, MSB = leftmost pixel, bit 1 = white (Pillow's mode "1" layout)."""
    bits = img1.tobytes()
    if len(bits) != FRAME_BYTES:
        raise ValueError(f"packed frame is {len(bits)} bytes, expected {FRAME_BYTES}")
    return bits


def etag_for(bits: bytes) -> str:
    return '"' + hashlib.sha256(bits).hexdigest()[:16] + '"'


def png_bytes(img1: Image.Image) -> bytes:
    buf = io.BytesIO()
    img1.save(buf, "PNG", optimize=True)
    return buf.getvalue()


def calibration_pattern() -> Image.Image:
    """Asymmetric marks so rotation, mirroring and inversion are all visible on the panel."""
    img = new_canvas()
    d = ImageDraw.Draw(img)
    d.rectangle([0, 0, 39, 39], fill=0)                                     # top-left: solid square
    d.line([(WIDTH - 40, 1), (WIDTH - 2, 1), (WIDTH - 2, 39)], fill=0, width=4)  # top-right: bracket
    d.line([(1, HEIGHT - 40), (1, HEIGHT - 2), (39, HEIGHT - 2)], fill=0, width=4)  # bottom-left: bracket
    d.ellipse([WIDTH - 40, HEIGHT - 40, WIDTH - 1, HEIGHT - 1], fill=0)       # bottom-right: dot
    d.text((52, 10), "TOP LEFT (solid square)", font=font(20, bold=True), fill=0)
    d.text((WIDTH - 52, HEIGHT - 10), "BOTTOM RIGHT (dot)", font=font(20, bold=True), fill=0, anchor="rb")
    d.text((WIDTH // 2, 110), "inkboard calibration", font=font(36, bold=True), fill=0, anchor="mm")
    d.text((WIDTH // 2, 155), "Black text on white means the invert setting is right.",
           font=font(18), fill=0, anchor="mm")
    for y in range(200, 400, 8):
        for x in range(200, 600, 8):
            if ((x - 200) // 8 + (y - 200) // 8) % 2 == 0:
                d.rectangle([x, y, x + 7, y + 7], fill=0)
    return to_1bit(img)
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `uv run pytest tests/test_frame.py -v`
Expected: 6 passed

- [ ] **Step 6: Commit**

```bash
git add server/
git commit -m "Server: scaffold package, frame packing and calibration pattern

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 2: Wake schedule

**Files:**
- Create: `server/inkboard_server/schedule.py`
- Test: `server/tests/test_schedule.py`

**Interfaces:**
- Produces:
  - `schedule.MIN_SLEEP_S = 300`, `MAX_SLEEP_S = 21600`, `WAKE_OFFSET_S = 60`
  - `next_top_of_hour(now: datetime) -> datetime` (aware, in `now`'s tz)
  - `next_refresh_seconds(now: datetime) -> int`
  - `utc_offset_seconds(now: datetime) -> int`

- [ ] **Step 1: Write the failing tests**

`server/tests/test_schedule.py`:
```python
from datetime import datetime
from zoneinfo import ZoneInfo

import pytest

from inkboard_server.schedule import next_refresh_seconds, next_top_of_hour, utc_offset_seconds

LA = ZoneInfo("America/Los_Angeles")


def test_mid_hour():
    assert next_refresh_seconds(datetime(2026, 9, 27, 18, 20, tzinfo=LA)) == 41 * 60


def test_late_evening_lands_after_midnight():
    now = datetime(2026, 9, 27, 23, 30, tzinfo=LA)
    assert next_top_of_hour(now) == datetime(2026, 9, 28, 0, 0, tzinfo=LA)
    assert next_refresh_seconds(now) == 31 * 60  # wakes 00:01, date already changed


def test_short_interval_clamped_to_300():
    assert next_refresh_seconds(datetime(2026, 9, 27, 11, 58, tzinfo=LA)) == 300


def test_exactly_on_the_hour_goes_to_next_hour():
    assert next_refresh_seconds(datetime(2026, 9, 27, 12, 0, tzinfo=LA)) == 61 * 60


def test_spring_forward():
    # 2026-03-08 02:00 PST -> 03:00 PDT; 01:30 PST is 09:30Z, next top is 10:00Z = 03:00 PDT
    now = datetime(2026, 3, 8, 1, 30, tzinfo=LA)
    assert next_top_of_hour(now).hour == 3
    assert next_refresh_seconds(now) == 31 * 60


def test_fall_back():
    # 2026-11-01 02:00 PDT -> 01:00 PST; 01:30 PDT (fold=0) is 08:30Z, next top is 09:00Z = 01:00 PST
    now = datetime(2026, 11, 1, 1, 30, tzinfo=LA)
    top = next_top_of_hour(now)
    assert (top.hour, top.fold) == (1, 1)
    assert next_refresh_seconds(now) == 31 * 60


def test_half_hour_offset_zone():
    kolkata = ZoneInfo("Asia/Kolkata")
    assert next_refresh_seconds(datetime(2026, 9, 27, 10, 10, tzinfo=kolkata)) == 51 * 60


def test_naive_rejected():
    with pytest.raises(ValueError):
        next_refresh_seconds(datetime(2026, 9, 27, 10, 0))


def test_utc_offset():
    assert utc_offset_seconds(datetime(2026, 9, 27, 12, 0, tzinfo=LA)) == -7 * 3600
    assert utc_offset_seconds(datetime(2026, 1, 27, 12, 0, tzinfo=LA)) == -8 * 3600
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `uv run pytest tests/test_schedule.py -v`
Expected: FAIL with `ModuleNotFoundError: No module named 'inkboard_server.schedule'`

- [ ] **Step 3: Implement `schedule.py`**

`server/inkboard_server/schedule.py`:
```python
"""When the device should wake next: one minute past the next local top of the hour."""
from datetime import datetime, timedelta, timezone

MIN_SLEEP_S = 300
MAX_SLEEP_S = 21600
WAKE_OFFSET_S = 60


def next_top_of_hour(now: datetime) -> datetime:
    """The first instant after `now` whose local time is hh:00.

    Found by stepping in UTC 15 minutes at a time (every real UTC offset is a multiple
    of 15 minutes), so DST changes and half-hour zones need no special cases.
    """
    if now.tzinfo is None:
        raise ValueError("now must be timezone-aware")
    t = now.astimezone(timezone.utc).replace(second=0, microsecond=0)
    t = t.replace(minute=t.minute - t.minute % 15)
    while True:
        t += timedelta(minutes=15)
        local = t.astimezone(now.tzinfo)
        if local.minute == 0 and t > now:
            return local


def next_refresh_seconds(now: datetime) -> int:
    target = next_top_of_hour(now).astimezone(timezone.utc) + timedelta(seconds=WAKE_OFFSET_S)
    seconds = int((target - now.astimezone(timezone.utc)).total_seconds())
    return max(MIN_SLEEP_S, min(MAX_SLEEP_S, seconds))


def utc_offset_seconds(now: datetime) -> int:
    if now.tzinfo is None:
        raise ValueError("now must be timezone-aware")
    return int(now.utcoffset().total_seconds())
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `uv run pytest tests/test_schedule.py -v`
Expected: 9 passed

- [ ] **Step 5: Commit**

```bash
git add server/inkboard_server/schedule.py server/tests/test_schedule.py
git commit -m "Server: wake schedule (next local hour + 1 min, DST-safe)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 3: CachedSource

**Files:**
- Create: `server/inkboard_server/sources/__init__.py` (empty for now),
  `server/inkboard_server/sources/base.py`
- Modify: `server/tests/conftest.py` (add `FakeClock` and the `clock` fixture)
- Test: `server/tests/test_cached_source.py`

**Design (revised after the plan's Codex review):**
- **Stale-while-revalidate.** A fresh entry is returned as is. For an *expired* entry, the
  source starts one background refresh and returns the old data **immediately**, so a slow
  or dead upstream never delays a response.
- **Cold keys.** A key with no data at all has nothing to fall back on, so it waits for its
  refresh, but no longer than `min(cold_wait_s, deadline - now)`. `deadline` is a
  `time.monotonic()` value supplied per request (Task 11). On timeout it raises `NoData`;
  the fetch keeps running, and the next request benefits.
- **Staleness.** A result is stale when the entry's most recent refresh failed, or when the
  entry is older than `stale_after` (default 2 × TTL, which covers refreshes that never
  report back).
- **Single-flight** comes from an `_inflight: dict[key, Future]` table that holds only
  running refreshes. It is independent of cache eviction, so evicting a key while its
  refresh runs can't start a second fetch. There are no per-key locks to leak.
- **Failure memory.** `_miss_failures` (keys that never succeeded) is an LRU capped at
  `max_entries`.
- **Disk bound across restarts.**
  - At start-up the source scans its directory, deletes files beyond `max_entries`
    (oldest by mtime first) along with any stray temp files, and seeds a disk LRU index.
  - Every store updates that index and deletes the oldest file when over the cap.
  - Writes go to a uniquely named temp file, then `replace`.
- **The daily upstream budget persists** in `<cache_dir>/<name>/_budget.json` as
  `{"day", "calls"}`, written atomically on every call. A restart continues the same
  day's count.
- **Executors.** Refreshes run on an `Executor`. Production uses a shared 8-thread pool.
  Tests use `InlineExecutor`, which runs the refresh in the caller's thread so results are
  deterministic: an expired entry then comes back already refreshed, or already marked
  stale.

**Interfaces:**
- Produces (in `sources/base.py`):
  - `utcnow() -> datetime`
  - `SourceResult(key: str, data: Any, fetched_at: datetime, stale: bool)`, with the
    property `.version -> tuple[str, str, bool]`
  - `NoData(Exception)`, with the attribute `.source: str`
  - `InlineExecutor(Executor)`
  - `CachedSource(name, fetch: Callable[[dict], Any], *, ttl: timedelta, cache_dir: Path | None = None, max_entries: int = 1000, daily_cap: int | None = None, retry_after: timedelta = timedelta(minutes=5), stale_after: timedelta | None = None, cold_wait_s: float = 8.0, executor: Executor | None = None, clock=utcnow)`
    with:
    - `.key_for(params: dict) -> str`
    - `.get(params: dict, deadline: float | None = None) -> SourceResult` (raises `NoData`)
    - `.health() -> dict`
    - `.upstream_calls: int`
    - `.name: str`
- Produces (in `tests/conftest.py`): `FakeClock(t: datetime)`, which is callable and has
  `.advance(**timedelta_kwargs)`, and the fixture `clock`.

- [ ] **Step 1: Add FakeClock to conftest**

Append to `server/tests/conftest.py`:
```python
from datetime import datetime, timedelta, timezone


class FakeClock:
    def __init__(self, t: datetime):
        self.t = t

    def __call__(self) -> datetime:
        return self.t

    def advance(self, **kw) -> None:
        self.t += timedelta(**kw)


@pytest.fixture
def clock():
    return FakeClock(datetime(2026, 9, 27, 17, 0, tzinfo=timezone.utc))
```

- [ ] **Step 2: Write the failing tests**

`server/tests/test_cached_source.py`:
```python
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
    clock.advance(minutes=45)
    assert not src2.get({"a": 1}).stale  # 45 min < 2 x TTL
    clock.advance(minutes=20)
    assert src2.get({"a": 1}).stale      # 65 min >= 2 x TTL


def test_expired_entry_returns_immediately_while_refreshing(clock, tmp_path):
    up = Upstream()
    pool = ThreadPoolExecutor(2)
    src = make(clock, tmp_path, up, executor=pool)
    first = src.get({"a": 1})
    clock.advance(minutes=31)
    up.block[1] = threading.Event()
    t0 = time.monotonic()
    r = src.get({"a": 1})
    assert time.monotonic() - t0 < 0.2
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
    src = make(clock, tmp_path, up, daily_cap=2)
    src.get({"a": 1})
    src.get({"a": 2})
    restarted = make(clock, tmp_path, up, daily_cap=2)
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
```

- [ ] **Step 3: Run the tests to verify they fail**

Run: `uv run pytest tests/test_cached_source.py -v`
Expected: FAIL with `ModuleNotFoundError: No module named 'inkboard_server.sources'`

- [ ] **Step 4: Implement `sources/base.py`**

Create an empty `server/inkboard_server/sources/__init__.py`, then
`server/inkboard_server/sources/base.py`:
```python
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
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `uv run pytest tests/test_cached_source.py -v`
Expected: 18 passed

- [ ] **Step 6: Commit**

```bash
git add server/inkboard_server/sources server/tests/conftest.py server/tests/test_cached_source.py
git commit -m "Server: CachedSource (stale-while-revalidate, deadlines, single-flight, persistent caps)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 4: FRED and Open-Meteo sources, the Sources facade, and recorded fixtures

**Files:**
- Create: `server/inkboard_server/sources/fred.py`, `server/inkboard_server/sources/openmeteo.py`,
  `server/tests/fixtures/record_fixtures.py`, `server/tests/fixtures/*.json` (generated)
- Modify: `server/inkboard_server/sources/__init__.py`, `server/tests/conftest.py`
- Test: `server/tests/test_sources.py`

**Interfaces:**
- Consumes: `CachedSource`, `SourceResult`, `utcnow` (Task 3)
- Produces:
  - `fred.parse_observations(payload: dict) -> list[list]`, returning `[["YYYY-MM-DD", float], ...]`
  - `fred.make_fetch(client: httpx.Client, api_key: str, clock) -> Callable[[dict], list]`
  - `openmeteo.weather_params(lat: float, lon: float, units: str, tz: str) -> dict`
  - `openmeteo.parse_forecast(payload: dict) -> dict`, returning
    `{"current": {"temp": float, "code": int}, "daily": [{"date": str, "code": int, "hi": float, "lo": float}, ...]}`
  - `openmeteo.make_fetch(client: httpx.Client) -> Callable[[dict], dict]`
  - `Sources(fred: CachedSource, weather: CachedSource, deadline: float | None = None)` with:
    - `.scoped(deadline: float) -> Sources` (a view whose calls pass that per-request deadline)
    - `.fred_series(fred_id: str) -> SourceResult`
    - `.weather(lat, lon, units, tz: str) -> SourceResult`
    - `.health() -> dict`
    - the attributes `.fred_src`, `.weather_src`
  - `make_sources(fred_fetch, weather_fetch, *, cache_dir: Path | None, clock=utcnow, executor: Executor | None = None) -> Sources`
  - `build_sources(cache_dir: Path | None, fred_api_key: str, *, client: httpx.Client | None = None, clock=utcnow) -> Sources`
  - conftest additions: `FIXTURES_TODAY: date`, `LA: ZoneInfo`, `FIXED_NOW: datetime`
    (10:00 LA on `FIXTURES_TODAY`), `FakeUpstream`, and the fixtures `upstream` and
    `sources`. The `clock` fixture now starts at `FIXED_NOW`.

- [ ] **Step 1: Record the fixtures (the only step that uses the network)**

`server/tests/fixtures/record_fixtures.py`:
```python
"""One-off: record upstream responses used by the tests.

Run from server/:  uv run python tests/fixtures/record_fixtures.py
FRED data comes from the keyless graph CSV and is rewritten in the API's JSON shape,
so recording needs no API key. Commit the resulting JSON files.
"""
import csv
import io
import json
from datetime import date
from pathlib import Path

import httpx

HERE = Path(__file__).parent
FRED_IDS = ["SP500", "CBBTCUSD", "MORTGAGE30US", "MEDLISPRI31080", "DGS10", "DTWEXBGS"]


def record_fred(client: httpx.Client, series_id: str) -> None:
    r = client.get("https://fred.stlouisfed.org/graph/fredgraph.csv",
                   params={"id": series_id, "cosd": "2015-01-01", "coed": date.today().isoformat()})
    r.raise_for_status()
    rows = list(csv.reader(io.StringIO(r.text)))[1:]
    obs = [{"date": d, "value": v if v else "."} for d, v in rows]
    (HERE / f"fred_{series_id}.json").write_text(json.dumps({"observations": obs}))


def record_weather(client: httpx.Client) -> None:
    r = client.get("https://api.open-meteo.com/v1/forecast", params={
        "latitude": 34.1, "longitude": -118.2,
        "current": "temperature_2m,weather_code",
        "daily": "weather_code,temperature_2m_max,temperature_2m_min",
        "temperature_unit": "fahrenheit", "timezone": "America/Los_Angeles", "forecast_days": 8,
    })
    r.raise_for_status()
    (HERE / "weather_la.json").write_text(json.dumps(r.json(), indent=1))


if __name__ == "__main__":
    with httpx.Client(timeout=30, follow_redirects=True) as c:
        for sid in FRED_IDS:
            record_fred(c, sid)
        record_weather(c)
    print("recorded", sorted(p.name for p in HERE.glob("*.json")))
```

Run: `uv run python tests/fixtures/record_fixtures.py`
Expected: prints the 7 JSON file names. Check that `weather_la.json` has 8 entries in
`daily.time` and that each `fred_*.json` has more than 100 observations.

- [ ] **Step 2: Extend conftest with fixture-backed sources**

Append to `server/tests/conftest.py`, and **replace** the Task 3 `clock` fixture so it
starts at `FIXED_NOW`:
```python
import json
from collections import Counter
from datetime import date, time
from zoneinfo import ZoneInfo

from inkboard_server.sources import make_sources
from inkboard_server.sources import fred as fred_mod
from inkboard_server.sources.base import InlineExecutor
from inkboard_server.sources import openmeteo

LA = ZoneInfo("America/Los_Angeles")
WEATHER_LA = json.loads((FIXTURES / "weather_la.json").read_text())
FIXTURES_TODAY = date.fromisoformat(WEATHER_LA["daily"]["time"][0])
FIXED_NOW = datetime.combine(FIXTURES_TODAY, time(10, 0), LA)


class FakeUpstream:
    """Serves recorded fixtures; set .fail to simulate an outage."""

    def __init__(self):
        self.fail = False
        self.calls = Counter()

    def fred(self, params):
        self.calls["fred"] += 1
        if self.fail:
            raise RuntimeError("fred down")
        payload = json.loads((FIXTURES / f"fred_{params['series_id']}.json").read_text())
        return fred_mod.parse_observations(payload)

    def weather(self, params):
        self.calls["weather"] += 1
        if self.fail:
            raise RuntimeError("open-meteo down")
        return openmeteo.parse_forecast(WEATHER_LA)


@pytest.fixture
def clock():
    return FakeClock(FIXED_NOW.astimezone(timezone.utc))


@pytest.fixture
def upstream():
    return FakeUpstream()


@pytest.fixture
def sources(tmp_path, upstream, clock):
    return make_sources(upstream.fred, upstream.weather, cache_dir=tmp_path, clock=clock,
                        executor=InlineExecutor())  # deterministic: refreshes finish inside get()
```
Delete the earlier `clock` fixture (the one returning 2026-09-27 17:00 UTC) so only one
remains. The Task 3 tests only rely on the clock being a fixed UTC-aware time, which
still holds.

- [ ] **Step 3: Write the failing tests**

`server/tests/test_sources.py`:
```python
import json

import httpx
import pytest

from inkboard_server.sources import fred, openmeteo
from inkboard_server.sources.base import NoData

from .conftest import FIXTURES, FIXTURES_TODAY


def test_parse_observations_skips_missing():
    payload = {"observations": [{"date": "2026-01-01", "value": "."},
                                {"date": "2026-01-02", "value": "5.5"},
                                {"date": "2026-01-03", "value": ""}]}
    assert fred.parse_observations(payload) == [["2026-01-02", 5.5]]


def test_parse_observations_empty_raises():
    with pytest.raises(ValueError):
        fred.parse_observations({"observations": [{"date": "2026-01-01", "value": "."}]})


def test_recorded_fred_fixtures_parse():
    for sid in ("SP500", "CBBTCUSD", "MORTGAGE30US", "MEDLISPRI31080", "DGS10", "DTWEXBGS"):
        obs = fred.parse_observations(json.loads((FIXTURES / f"fred_{sid}.json").read_text()))
        assert len(obs) > 100, sid


def test_fred_fetch_request(clock):
    seen = {}

    def handler(request):
        seen.update(dict(request.url.params))
        return httpx.Response(200, json={"observations": [{"date": "2026-01-02", "value": "1"}]})

    client = httpx.Client(transport=httpx.MockTransport(handler))
    out = fred.make_fetch(client, "KEY", clock)({"series_id": "SP500"})
    assert out == [["2026-01-02", 1.0]]
    assert seen == {"series_id": "SP500", "api_key": "KEY", "file_type": "json",
                    "observation_start": f"{clock().year - 11}-01-01"}


def test_fred_fetch_without_key_fails(clock):
    client = httpx.Client(transport=httpx.MockTransport(lambda r: httpx.Response(200)))
    with pytest.raises(RuntimeError, match="FRED_API_KEY"):
        fred.make_fetch(client, "", clock)({"series_id": "SP500"})


def test_weather_params_round_and_normalize_negative_zero():
    assert openmeteo.weather_params(34.05, -118.24, "imperial", "America/Los_Angeles") == {
        "lat": 34.0, "lon": -118.2, "units": "imperial", "tz": "America/Los_Angeles"}
    p = openmeteo.weather_params(-0.04, 0.0, "metric", "UTC")
    assert str(p["lat"]) == "0.0"


def test_parse_forecast_drops_null_days():
    payload = {"current": {"temperature_2m": 70.2, "weather_code": 1},
               "daily": {"time": ["2026-09-27", "2026-09-28"], "weather_code": [0, None],
                         "temperature_2m_max": [80, 81], "temperature_2m_min": [60, 61]}}
    assert openmeteo.parse_forecast(payload) == {
        "current": {"temp": 70.2, "code": 1},
        "daily": [{"date": "2026-09-27", "code": 0, "hi": 80.0, "lo": 60.0}]}


def test_openmeteo_fetch_request():
    seen = {}

    def handler(request):
        seen.update(dict(request.url.params))
        return httpx.Response(200, json=json.loads((FIXTURES / "weather_la.json").read_text()))

    client = httpx.Client(transport=httpx.MockTransport(handler))
    out = openmeteo.make_fetch(client)({"lat": 34.1, "lon": -118.2, "units": "metric", "tz": "Asia/Tokyo"})
    assert out["daily"][0]["date"] == FIXTURES_TODAY.isoformat()
    assert seen["temperature_unit"] == "celsius"
    assert seen["timezone"] == "Asia/Tokyo"
    assert seen["forecast_days"] == "8"


def test_weather_cache_key_includes_tz(sources, upstream):
    sources.weather(34.05, -118.24, "imperial", "America/Los_Angeles")
    sources.weather(34.05, -118.24, "imperial", "Asia/Tokyo")
    sources.weather(34.04, -118.21, "imperial", "America/Los_Angeles")  # same rounded cell
    assert upstream.calls["weather"] == 2


def test_sources_nodata_when_down(sources, upstream):
    upstream.fail = True
    with pytest.raises(NoData):
        sources.fred_series("SP500")


def test_scoped_passes_deadline(sources, upstream):
    view = sources.scoped(123.0)
    assert view.deadline == 123.0 and view.fred_src is sources.fred_src
    assert view.fred_series("SP500").data == sources.fred_series("SP500").data
    assert upstream.calls["fred"] == 1


def test_sources_health(sources):
    sources.fred_series("SP500")
    h = sources.health()
    assert set(h) == {"fred", "weather"}
    assert h["fred"]["entries"] == 1
```

- [ ] **Step 4: Run the tests to verify they fail**

Run: `uv run pytest tests/test_sources.py -v`
Expected: FAIL with `ImportError: cannot import name 'make_sources'`

- [ ] **Step 5: Implement the sources**

`server/inkboard_server/sources/fred.py`:
```python
"""FRED series observations (https://fred.stlouisfed.org/docs/api/fred/)."""
from datetime import date
from typing import Callable

import httpx

FRED_URL = "https://api.stlouisfed.org/fred/series/observations"
HISTORY_YEARS = 11  # covers the 10-year max window; start changes only once a year


def parse_observations(payload: dict) -> list[list]:
    out = []
    for o in payload.get("observations", []):
        value = o.get("value", ".")
        if value in (".", ""):
            continue  # FRED marks missing days (holidays) with "."
        out.append([o["date"], float(value)])
    if not out:
        raise ValueError("FRED returned no observations")
    return out


def make_fetch(client: httpx.Client, api_key: str, clock) -> Callable[[dict], list]:
    def fetch(params: dict) -> list:
        if not api_key:
            raise RuntimeError("FRED_API_KEY is not set")
        start = date(clock().year - HISTORY_YEARS, 1, 1).isoformat()
        r = client.get(FRED_URL, params={"series_id": params["series_id"], "api_key": api_key,
                                          "file_type": "json", "observation_start": start}, timeout=10)
        r.raise_for_status()
        return parse_observations(r.json())

    return fetch
```

`server/inkboard_server/sources/openmeteo.py`:
```python
"""Open-Meteo forecast (https://open-meteo.com/en/docs), free tier, non-commercial."""
from typing import Callable

import httpx

URL = "https://api.open-meteo.com/v1/forecast"
FORECAST_DAYS = 8


def _round(v: float) -> float:
    return float(f"{v:.1f}") + 0.0  # + 0.0 turns -0.0 into 0.0


def weather_params(lat: float, lon: float, units: str, tz: str) -> dict:
    """Cache params: ~10 km cells, and tz because daily boundaries depend on it."""
    return {"lat": _round(lat), "lon": _round(lon), "units": units, "tz": tz}


def parse_forecast(payload: dict) -> dict:
    cur, d = payload["current"], payload["daily"]
    daily = [
        {"date": t, "code": int(c), "hi": float(h), "lo": float(lo)}
        for t, c, h, lo in zip(d["time"], d["weather_code"], d["temperature_2m_max"], d["temperature_2m_min"])
        if c is not None and h is not None and lo is not None
    ]
    return {"current": {"temp": float(cur["temperature_2m"]), "code": int(cur["weather_code"])},
            "daily": daily}


def make_fetch(client: httpx.Client) -> Callable[[dict], dict]:
    def fetch(params: dict) -> dict:
        r = client.get(URL, params={
            "latitude": params["lat"], "longitude": params["lon"],
            "current": "temperature_2m,weather_code",
            "daily": "weather_code,temperature_2m_max,temperature_2m_min",
            "temperature_unit": "fahrenheit" if params["units"] == "imperial" else "celsius",
            "timezone": params["tz"], "forecast_days": FORECAST_DAYS,
        }, timeout=10)
        r.raise_for_status()
        return parse_forecast(r.json())

    return fetch
```

`server/inkboard_server/sources/__init__.py`:
```python
"""Data sources behind CachedSource, and the Sources facade widgets use."""
from concurrent.futures import Executor
from datetime import timedelta
from pathlib import Path

import httpx

from . import fred, openmeteo
from .base import CachedSource, NoData, SourceResult, utcnow

__all__ = ["CachedSource", "NoData", "SourceResult", "Sources", "build_sources", "make_sources"]

FRED_TTL = timedelta(hours=6)
WEATHER_TTL = timedelta(minutes=30)
WEATHER_MAX_ENTRIES = 2000
WEATHER_DAILY_CAP = 8000
USER_AGENT = "inkboard-server (+https://inkboard.signalwave.dev)"


class Sources:
    def __init__(self, fred_src: CachedSource, weather_src: CachedSource, deadline: float | None = None):
        self.fred_src = fred_src
        self.weather_src = weather_src
        self.deadline = deadline

    def scoped(self, deadline: float) -> "Sources":
        """Same caches; cold fetches give up at `deadline` (time.monotonic())."""
        return Sources(self.fred_src, self.weather_src, deadline)

    def fred_series(self, fred_id: str) -> SourceResult:
        return self.fred_src.get({"series_id": fred_id}, self.deadline)

    def weather(self, lat: float, lon: float, units: str, tz: str) -> SourceResult:
        return self.weather_src.get(openmeteo.weather_params(lat, lon, units, tz), self.deadline)

    def health(self) -> dict:
        return {"fred": self.fred_src.health(), "weather": self.weather_src.health()}


def make_sources(fred_fetch, weather_fetch, *, cache_dir: Path | None, clock=utcnow,
                 executor: Executor | None = None) -> Sources:
    return Sources(
        CachedSource("fred", fred_fetch, ttl=FRED_TTL, cache_dir=cache_dir, max_entries=64,
                     executor=executor, clock=clock),
        CachedSource("weather", weather_fetch, ttl=WEATHER_TTL, cache_dir=cache_dir,
                     max_entries=WEATHER_MAX_ENTRIES, daily_cap=WEATHER_DAILY_CAP, executor=executor, clock=clock),
    )


def build_sources(cache_dir: Path | None, fred_api_key: str, *, client: httpx.Client | None = None,
                  clock=utcnow) -> Sources:
    client = client or httpx.Client(headers={"User-Agent": USER_AGENT})
    return make_sources(fred.make_fetch(client, fred_api_key, clock), openmeteo.make_fetch(client),
                        cache_dir=cache_dir, clock=clock)
```

- [ ] **Step 6: Run the whole suite**

Run: `uv run pytest -v`
Expected: all pass (the Task 1–3 tests plus 12 new ones).

- [ ] **Step 7: Commit**

```bash
git add server/inkboard_server/sources server/tests
git commit -m "Server: FRED + Open-Meteo sources, Sources facade, recorded fixtures

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 5: Widget base and query parsing

**Files:**
- Create: `server/inkboard_server/widgets/__init__.py`, `server/inkboard_server/widgets/base.py`,
  `server/inkboard_server/widgets/params.py`, `server/inkboard_server/series.py`,
  `server/inkboard_server/query.py`
- Test: `server/tests/test_query.py`

**Interfaces:**
- Consumes: `SourceResult` (Task 3), `Sources` (Task 4)
- Produces:
  - `widgets.base`:
    - `Size` (`THIRD`, `TWO_THIRDS`, `FULL`), with `.thirds`, `.width` and `.token`, and
      `Size.from_token(str)`
    - `WIDGET_H = 464`
    - `Box(x, y, w, h)`
    - `REQUIRED`
    - `ParamSpec(parse, default=REQUIRED, fmt=str)`
    - `RenderContext(today: date, tz: ZoneInfo)`
    - `WidgetData(payload, sources: list[SourceResult])` with `.stale`
    - `Widget`, with `.size`, `.options`, `fetch(sources, ctx)`, `render(img, box, data, ctx)`
      and `attributions()`
    - `REGISTRY: dict[str, type[Widget]]`
    - `register(cls)`
  - `widgets.params`:
    - `coordinate(lo, hi) -> Callable[[str], float]` (rounds to 0.1)
    - `fmt_coord(v: float) -> str`
    - `int_in_range(lo, hi)`
    - `one_of(*choices)`
    - `id_list(allowed: Iterable[str], max_n: int) -> Callable[[str], tuple[str, ...]]`
  - `series`:
    - `SeriesDef(id, fred_id, label, short, fmt, attribution: tuple[str, ...])`
    - `CATALOG: dict[str, SeriesDef]`
    - `DEFAULT_SERIES: tuple[str, ...]`
  - `query`:
    - `MAX_QUERY_LEN = 1024`, `MAX_WIDGETS = 3`, `GLOBAL_PARAMS = {"w", "tz"}`
    - `QueryError(ValueError)`
    - `WidgetSpec(type_name, size)`
    - `FrameRequest(widgets, tz, options, canonical)`
    - `parse_query(raw: str, registry=None) -> FrameRequest` (`None` means the global `REGISTRY`)

- [ ] **Step 1: Write the failing tests**

`server/tests/test_query.py`:
```python
from datetime import date

import pytest

from inkboard_server.query import FrameRequest, QueryError, parse_query
from inkboard_server.widgets.base import REQUIRED, ParamSpec, Size, Widget
from inkboard_server.widgets.params import coordinate, fmt_coord, int_in_range, one_of


class FakeA(Widget):
    type_name = "fake_a"
    supported_sizes = frozenset(Size)
    params = {"lat": ParamSpec(coordinate(-90, 90), fmt=fmt_coord),
              "units": ParamSpec(one_of("imperial", "metric"), default="imperial")}

    def fetch(self, sources, ctx): ...
    def render(self, img, box, data, ctx): ...


class FakeB(Widget):
    type_name = "fake_b"
    supported_sizes = frozenset({Size.THIRD})
    params = {"n": ParamSpec(int_in_range(1, 10), default=3)}

    def fetch(self, sources, ctx): ...
    def render(self, img, box, data, ctx): ...


REG = {"fake_a": FakeA, "fake_b": FakeB}


def parse(q: str) -> FrameRequest:
    return parse_query(q, REG)


def err(q: str) -> str:
    with pytest.raises(QueryError) as e:
        parse(q)
    return str(e.value)


def test_valid_with_defaults():
    r = parse("w=fake_a:2/3,fake_b:1/3&lat=34.05")
    assert [(s.type_name, s.size) for s in r.widgets] == [("fake_a", Size.TWO_THIRDS), ("fake_b", Size.THIRD)]
    assert r.tz.key == "UTC"
    assert dict(r.options) == {"lat": 34.0, "units": "imperial", "n": 3}
    assert r.canonical == "lat=34.0&n=3&tz=UTC&units=imperial&w=fake_a%3A2%2F3%2Cfake_b%3A1%2F3"


def test_canonical_ignores_order_and_rounding_noise():
    a = parse("w=fake_a:1&lat=34.04&tz=America/Los_Angeles")
    b = parse("tz=America/Los_Angeles&lat=34.0&w=fake_a:1&units=imperial")
    assert a.canonical == b.canonical


def test_url_encoded_w():
    assert parse("w=fake_a%3A1&lat=1").widgets == parse("w=fake_a:1&lat=1").widgets


def test_sizes_must_sum_to_three():
    assert err("w=fake_a:2/3,fake_a:2/3&lat=1") == "w: sizes add up to 4/3, need 3/3"
    assert err("w=fake_a:2/3&lat=1") == "w: sizes add up to 2/3, need 3/3"


def test_w_errors():
    assert err("lat=1").startswith("w: required")
    assert err("w=nope:1") == "w: unknown widget 'nope'"
    assert err("w=fake_a") == "w: expected type:size, got 'fake_a'"
    assert err("w=fake_a:1/2&lat=1") == "w: size must be 1/3, 2/3 or 1, got '1/2'"
    assert err("w=fake_b:1") == "w: fake_b does not support size 1"
    assert err("w=fake_b:1/3,fake_b:1/3,fake_b:1/3,fake_b:1/3") == "w: at most 3 widgets"


def test_duplicate_param():
    assert err("w=fake_a:1&w=fake_a:1&lat=1") == "w: given more than once"


def test_unknown_and_unused_params():
    assert err("w=fake_a:1&lat=1&foo=2") == "foo: unknown parameter"
    assert err("w=fake_a:1&lat=1&n=2") == "n: not used by any widget in w"


def test_required_param_missing():
    assert err("w=fake_a:1") == "lat: required by fake_a"


def test_param_value_errors():
    assert err("w=fake_a:1&lat=91").startswith("lat: ")
    assert err("w=fake_a:1&lat=nan").startswith("lat: ")
    assert err("w=fake_a:1&lat=abc").startswith("lat: ")
    assert err("w=fake_a:1&lat=1&units=kelvin").startswith("units: ")
    assert err("w=fake_b:1/3,fake_a:2/3&lat=1&n=11").startswith("n: ")


def test_tz_validation():
    assert parse("w=fake_a:1&lat=1&tz=Asia/Kolkata").tz.key == "Asia/Kolkata"
    assert err("w=fake_a:1&lat=1&tz=Nope/Zone") == "tz: unknown timezone 'Nope/Zone'"
    assert err("w=fake_a:1&lat=1&tz=../../etc/passwd").startswith("tz: unknown timezone")
    assert err("w=fake_a:1&lat=1&tz=America").startswith("tz: unknown timezone")


def test_query_limits():
    assert err("w=fake_a:1&lat=1&" + "x" * 1100) == "query longer than 1024 bytes"
    assert err("w=fake_a:1&lat=1&") == "malformed query string"
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `uv run pytest tests/test_query.py -v`
Expected: FAIL with `ModuleNotFoundError: No module named 'inkboard_server.query'`

- [ ] **Step 3: Implement the base, params, series and query modules**

`server/inkboard_server/widgets/base.py`:
```python
"""Widget contract: sizes, boxes, params, fetch/render split, and the registry."""
from __future__ import annotations

from abc import ABC, abstractmethod
from dataclasses import dataclass, field
from datetime import date
from enum import Enum
from typing import Any, Callable, ClassVar
from zoneinfo import ZoneInfo

from PIL import Image

from ..sources.base import SourceResult

WIDGET_H = 464  # 480 minus the 16 px footer


class Size(Enum):
    THIRD = (1, 266)
    TWO_THIRDS = (2, 534)
    FULL = (3, 800)

    @property
    def thirds(self) -> int:
        return self.value[0]

    @property
    def width(self) -> int:
        return self.value[1]

    @property
    def token(self) -> str:
        return {1: "1/3", 2: "2/3", 3: "1"}[self.thirds]

    @classmethod
    def from_token(cls, token: str) -> "Size":
        for s in cls:
            if s.token == token:
                return s
        raise ValueError(token)


@dataclass(frozen=True)
class Box:
    x: int
    y: int
    w: int
    h: int


class _Required:
    def __repr__(self) -> str:
        return "REQUIRED"


REQUIRED: Any = _Required()


@dataclass(frozen=True)
class ParamSpec:
    parse: Callable[[str], Any]           # raises ValueError with a short reason
    default: Any = REQUIRED
    fmt: Callable[[Any], str] = str       # canonical text for the frame-cache key


@dataclass(frozen=True)
class RenderContext:
    today: date        # local date in the request tz
    tz: ZoneInfo


@dataclass
class WidgetData:
    payload: Any
    sources: list[SourceResult] = field(default_factory=list)

    @property
    def stale(self) -> bool:
        return any(s.stale for s in self.sources)


class Widget(ABC):
    type_name: ClassVar[str]
    supported_sizes: ClassVar[frozenset[Size]]
    params: ClassVar[dict[str, ParamSpec]] = {}

    def __init__(self, size: Size, options: dict[str, Any]):
        self.size = size
        self.options = options

    @abstractmethod
    def fetch(self, sources, ctx: RenderContext) -> WidgetData:
        """Read through cached sources. May raise NoData."""

    @abstractmethod
    def render(self, img: Image.Image, box: Box, data: WidgetData, ctx: RenderContext) -> None:
        """Draw into `box` of an "L" image. Pure: no I/O, output depends only on inputs."""

    def attributions(self) -> list[str]:
        return []


REGISTRY: dict[str, type[Widget]] = {}


def register(cls: type[Widget]) -> type[Widget]:
    REGISTRY[cls.type_name] = cls
    return cls
```

`server/inkboard_server/widgets/params.py`:
```python
"""Reusable query-param parsers. Each raises ValueError with a short reason."""
import math
from typing import Callable, Iterable


def coordinate(lo: float, hi: float) -> Callable[[str], float]:
    def parse(text: str) -> float:
        v = float(text)
        if not math.isfinite(v) or not lo <= v <= hi:
            raise ValueError(f"must be a number from {lo:g} to {hi:g}")
        return float(f"{v:.1f}") + 0.0  # 0.1 deg cells; -0.0 -> 0.0

    return parse


def fmt_coord(v: float) -> str:
    return f"{v:.1f}"


def int_in_range(lo: int, hi: int) -> Callable[[str], int]:
    def parse(text: str) -> int:
        try:
            v = int(text)
        except ValueError:
            raise ValueError(f"must be a whole number from {lo} to {hi}") from None
        if not lo <= v <= hi:
            raise ValueError(f"must be a whole number from {lo} to {hi}")
        return v

    return parse


def one_of(*choices: str) -> Callable[[str], str]:
    def parse(text: str) -> str:
        if text not in choices:
            raise ValueError("must be one of " + ", ".join(choices))
        return text

    return parse


def id_list(allowed: Iterable[str], max_n: int) -> Callable[[str], tuple[str, ...]]:
    allowed = tuple(allowed)

    def parse(text: str) -> tuple[str, ...]:
        ids = tuple(text.split(","))
        if not 1 <= len(ids) <= max_n:
            raise ValueError(f"give 1 to {max_n} ids")
        if len(set(ids)) != len(ids):
            raise ValueError("ids must be unique")
        unknown = [i for i in ids if i not in allowed]
        if unknown:
            raise ValueError(f"unknown id {unknown[0]!r}; choose from " + ", ".join(allowed))
        return ids

    return parse
```

A value parser that raises a bare `ValueError` (for example `float("abc")`) gets
reported as `lat: could not convert string to float: 'abc'`. That's acceptable: it
starts with the param name.

`server/inkboard_server/series.py`:
```python
"""Market series a board can request with series=... (spec §5.1)."""
from dataclasses import dataclass
from typing import Callable


@dataclass(frozen=True)
class SeriesDef:
    id: str
    fred_id: str
    label: str
    short: str
    fmt: Callable[[float], str]
    attribution: tuple[str, ...]


CATALOG: dict[str, SeriesDef] = {s.id: s for s in (
    SeriesDef("sp500", "SP500", "S&P 500", "S&P", lambda v: f"{v:,.0f}", ("FRED", "S&P DJI")),
    SeriesDef("btc", "CBBTCUSD", "Bitcoin", "BTC", lambda v: f"${v / 1000:.1f}k", ("FRED", "Coinbase")),
    SeriesDef("mortgage30", "MORTGAGE30US", "Mortgage", "Mort", lambda v: f"{v:.2f}%", ("FRED", "Freddie Mac")),
    SeriesDef("home_la", "MEDLISPRI31080", "LA home", "Home", lambda v: f"${v / 1e6:.2f}M", ("FRED", "Realtor.com")),
    SeriesDef("ust10y", "DGS10", "10-yr Treasury", "10y", lambda v: f"{v:.2f}%", ("FRED",)),
    SeriesDef("usd_broad", "DTWEXBGS", "Dollar index", "USD", lambda v: f"{v:.1f}", ("FRED",)),
)}

DEFAULT_SERIES: tuple[str, ...] = ("sp500", "btc", "mortgage30", "home_la")
```

`server/inkboard_server/widgets/__init__.py`:
```python
"""Widgets. Importing this package registers every concrete widget."""
from .base import REGISTRY, WIDGET_H, Box, ParamSpec, RenderContext, Size, Widget, WidgetData, register

__all__ = ["REGISTRY", "WIDGET_H", "Box", "ParamSpec", "RenderContext", "Size", "Widget", "WidgetData", "register"]
```

`server/inkboard_server/query.py`:
```python
"""Query string -> validated FrameRequest (spec §2.2). Every error is a one-line QueryError."""
from __future__ import annotations

from dataclasses import dataclass
from types import MappingProxyType
from typing import Any, Mapping
from urllib.parse import parse_qsl, urlencode
from zoneinfo import ZoneInfo

from .widgets.base import REQUIRED, REGISTRY, Size, Widget

MAX_QUERY_LEN = 1024
MAX_WIDGETS = 3
GLOBAL_PARAMS = frozenset({"w", "tz"})


class QueryError(ValueError):
    pass


@dataclass(frozen=True)
class WidgetSpec:
    type_name: str
    size: Size


@dataclass(frozen=True)
class FrameRequest:
    widgets: tuple[WidgetSpec, ...]
    tz: ZoneInfo
    options: Mapping[str, Any]
    canonical: str


def parse_tz(value: str) -> ZoneInfo:
    try:
        return ZoneInfo(value)
    except (KeyError, ValueError, OSError):
        raise QueryError(f"tz: unknown timezone {value!r}") from None


def parse_w(value: str, registry: Mapping[str, type[Widget]]) -> tuple[WidgetSpec, ...]:
    items = value.split(",")
    if len(items) > MAX_WIDGETS:
        raise QueryError(f"w: at most {MAX_WIDGETS} widgets")
    specs = []
    for item in items:
        type_name, sep, token = item.partition(":")
        if not sep:
            raise QueryError(f"w: expected type:size, got {item!r}")
        cls = registry.get(type_name)
        if cls is None:
            raise QueryError(f"w: unknown widget {type_name!r}")
        try:
            size = Size.from_token(token)
        except ValueError:
            raise QueryError(f"w: size must be 1/3, 2/3 or 1, got {token!r}") from None
        if size not in cls.supported_sizes:
            raise QueryError(f"w: {type_name} does not support size {token}")
        specs.append(WidgetSpec(type_name, size))
    total = sum(s.size.thirds for s in specs)
    if total != 3:
        raise QueryError(f"w: sizes add up to {total}/3, need 3/3")
    return tuple(specs)


def parse_query(raw: str, registry: Mapping[str, type[Widget]] | None = None) -> FrameRequest:
    registry = REGISTRY if registry is None else registry
    if len(raw) > MAX_QUERY_LEN:
        raise QueryError(f"query longer than {MAX_QUERY_LEN} bytes")
    try:
        pairs = parse_qsl(raw, keep_blank_values=True, strict_parsing=bool(raw))
    except ValueError:
        raise QueryError("malformed query string") from None
    params: dict[str, str] = {}
    for k, v in pairs:
        if k in params:
            raise QueryError(f"{k}: given more than once")
        params[k] = v
    if "w" not in params:
        raise QueryError("w: required, e.g. w=market_trends:2/3,calendar_weather:1/3")
    specs = parse_w(params["w"], registry)
    tz = parse_tz(params.get("tz", "UTC"))

    used: dict[str, tuple[Any, str]] = {}
    for s in specs:
        for name, spec in registry[s.type_name].params.items():
            used.setdefault(name, (spec, s.type_name))
    known = GLOBAL_PARAMS | {n for cls in registry.values() for n in cls.params}
    for k in params:
        if k not in known:
            raise QueryError(f"{k}: unknown parameter")
        if k not in GLOBAL_PARAMS and k not in used:
            raise QueryError(f"{k}: not used by any widget in w")

    options: dict[str, Any] = {}
    for name, (spec, owner) in sorted(used.items()):
        if name in params:
            try:
                options[name] = spec.parse(params[name])
            except ValueError as e:
                raise QueryError(f"{name}: {e}") from None
        elif spec.default is REQUIRED:
            raise QueryError(f"{name}: required by {owner}")
        else:
            options[name] = spec.default

    canonical = [("w", ",".join(f"{s.type_name}:{s.size.token}" for s in specs)), ("tz", tz.key)]
    canonical += [(n, used[n][0].fmt(options[n])) for n in options]
    return FrameRequest(specs, tz, MappingProxyType(options), urlencode(sorted(canonical)))
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `uv run pytest tests/test_query.py -v`
Expected: 11 passed

- [ ] **Step 5: Commit**

```bash
git add server/inkboard_server/widgets server/inkboard_server/series.py server/inkboard_server/query.py server/tests/test_query.py
git commit -m "Server: widget base, registry, series catalog, query parsing

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 6: Drawing helpers (lines, markers, icons, month grid)

**Files:**
- Create: `server/inkboard_server/draw/lines.py`, `server/inkboard_server/draw/icons.py`,
  `server/inkboard_server/draw/calendar.py`
- Test: `server/tests/test_draw.py`

**Interfaces:**
- Consumes: `font` (Task 1)
- Produces:
  - `lines`:
    - `LineStyle(width, dash: tuple[int, int] | None, marker: str)`
    - `STYLES: tuple[LineStyle, ...]` (4 entries)
    - `MARKER_STEP = 56`
    - `styled_line(d, pts, style)`
    - `draw_marker(d, x, y, kind, r=4)`
    - `marker_xs(x0, x1, k, n, step=MARKER_STEP) -> list[float]`
    - `y_at(pts, x) -> float`
    - `line_with_markers(d, pts, style, k, n)`
    - `legend_sample(d, x, y, style, length=24)`
  - `icons`: `wmo_info(code: int) -> tuple[str, str]` and `icon(d, kind, cx, cy, r)`
  - `calendar`: `month_weeks(year, month) -> list[list[int]]` and
    `month_grid(d, x, y, w, today: date, big=False) -> int` (the height used)

- [ ] **Step 1: Write the failing tests**

`server/tests/test_draw.py`:
```python
from datetime import date

import pytest
from PIL import Image, ImageDraw

from inkboard_server.draw.calendar import month_grid, month_weeks
from inkboard_server.draw.icons import icon, wmo_info
from inkboard_server.draw.lines import (
    STYLES, LineStyle, draw_marker, legend_sample, line_with_markers, marker_xs, styled_line, y_at,
)


def canvas(w=200, h=100):
    img = Image.new("L", (w, h), 255)
    return img, ImageDraw.Draw(img)


def black(img):
    return sum(1 for p in img.getdata() if p < 128)


def test_marker_xs_stagger():
    assert marker_xs(0, 200, 0, 4) == [7.0, 63.0, 119.0, 175.0]
    sets = [set(marker_xs(0, 400, k, 4)) for k in range(4)]
    for i in range(4):
        for j in range(i + 1, 4):
            assert not sets[i] & sets[j]


def test_y_at_interpolates():
    pts = [(0, 10), (10, 20), (20, 0)]
    assert y_at(pts, 5) == 15
    assert y_at(pts, 15) == 10
    assert y_at(pts, -5) == 10 and y_at(pts, 50) == 0


def test_solid_line_width():
    img, d = canvas()
    styled_line(d, [(10, 50), (190, 50)], LineStyle(3, None, "square"))
    rows = {y for y in range(100) if img.getpixel((100, y)) < 128}
    assert len(rows) == 3


def test_dashed_line_has_gaps():
    img, d = canvas()
    styled_line(d, [(0, 50), (100, 50)], LineStyle(2, (7, 4), "circle"))
    row = [img.getpixel((x, 50)) < 128 for x in range(100)]
    assert 50 < sum(row) < 90


@pytest.mark.parametrize("kind", ["square", "circle", "triangle", "diamond"])
def test_markers_draw_near_point(kind):
    img, d = canvas()
    draw_marker(d, 100, 50, kind)
    assert black(img) > 0
    assert black(img.crop((92, 42, 109, 59))) == black(img)


def test_line_with_markers_and_legend_draw():
    img, d = canvas(400, 100)
    line_with_markers(d, [(0, 50), (399, 50)], STYLES[1], 1, 4)
    legend_sample(d, 10, 80, STYLES[3])
    assert black(img) > 100


@pytest.mark.parametrize("code,expected", [
    (0, ("Clear", "sun")), (1, ("Mostly clear", "sun")), (2, ("Partly cloudy", "part")),
    (3, ("Overcast", "cloud")), (45, ("Fog", "fog")), (48, ("Fog", "fog")),
    (51, ("Rain", "rain")), (67, ("Rain", "rain")), (80, ("Rain", "rain")), (82, ("Rain", "rain")),
    (71, ("Snow", "snow")), (77, ("Snow", "snow")), (85, ("Snow", "snow")), (86, ("Snow", "snow")),
    (95, ("Storms", "storm")), (99, ("Storms", "storm")), (4, ("—", "cloud")), (68, ("—", "cloud")),
])
def test_wmo_info(code, expected):
    assert wmo_info(code) == expected


@pytest.mark.parametrize("kind", ["sun", "part", "cloud", "fog", "rain", "snow", "storm"])
def test_icons_stay_in_bounds(kind):
    img, d = canvas(200, 200)
    icon(d, kind, 100, 100, 30)
    assert black(img) > 30
    r = int(30 * 1.1) + 2
    assert black(img.crop((100 - r, 100 - r, 100 + r + 1, 100 + r + 1))) == black(img)


def test_month_weeks_start_sunday():
    assert month_weeks(2026, 2)[0] == [1, 2, 3, 4, 5, 6, 7]         # Feb 2026 starts on Sunday
    aug = month_weeks(2026, 8)                                      # Aug 2026 starts on Saturday
    assert aug[0] == [0, 0, 0, 0, 0, 0, 1] and len(aug) == 6


def test_month_grid_highlights_today():
    img, d = canvas(266, 200)
    h = month_grid(d, 0, 0, 266, date(2026, 9, 27))
    assert 120 < h < 200
    # Sep 27, 2026 is a Sunday in the 5th week row: column 0, row index 5 (after header)
    cw, ch = 266 / 7, 22
    cx, cy = int(cw / 2), int(ch * 5 + ch / 2 + 2)
    assert img.getpixel((cx - 12, cy)) < 128  # inverted cell background is black
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `uv run pytest tests/test_draw.py -v`
Expected: FAIL with `ModuleNotFoundError: No module named 'inkboard_server.draw.lines'`

- [ ] **Step 3: Implement the drawing modules**

`server/inkboard_server/draw/lines.py`:
```python
"""Chart line styles for 1-bit output: width + dash pattern + marker shape (spec §5.1)."""
import math
from dataclasses import dataclass

from PIL import ImageDraw


@dataclass(frozen=True)
class LineStyle:
    width: int
    dash: tuple[int, int] | None  # (on, off) px; None = solid
    marker: str                   # square | circle | triangle | diamond


STYLES = (
    LineStyle(3, None, "square"),
    LineStyle(2, (7, 4), "circle"),
    LineStyle(1, None, "triangle"),
    LineStyle(2, (2, 3), "diamond"),
)
MARKER_STEP = 56


def styled_line(d: ImageDraw.ImageDraw, pts, style: LineStyle) -> None:
    if len(pts) < 2:
        return
    if style.dash is None:
        d.line(pts, fill=0, width=style.width, joint="curve" if style.width > 2 else None)
        return
    on, off = style.dash
    acc, drawing = 0.0, True
    for (x0, y0), (x1, y1) in zip(pts, pts[1:]):
        seg = math.hypot(x1 - x0, y1 - y0)
        t = 0.0
        while t < seg:
            limit = on if drawing else off
            step = min(limit - acc, seg - t)
            if drawing:
                a, b = t / seg, (t + step) / seg
                d.line([(x0 + (x1 - x0) * a, y0 + (y1 - y0) * a),
                        (x0 + (x1 - x0) * b, y0 + (y1 - y0) * b)], fill=0, width=style.width)
            t += step
            acc += step
            if acc >= limit:
                acc, drawing = 0.0, not drawing


def draw_marker(d: ImageDraw.ImageDraw, x: float, y: float, kind: str, r: int = 4) -> None:
    if kind == "square":
        d.rectangle([x - r, y - r, x + r, y + r], fill=0)
    elif kind == "circle":
        d.ellipse([x - r - 1, y - r - 1, x + r + 1, y + r + 1], fill=255, outline=0, width=2)
    elif kind == "triangle":
        d.polygon([(x, y - r - 1), (x + r + 1, y + r), (x - r - 1, y + r)], fill=0)
    elif kind == "diamond":
        d.polygon([(x, y - r - 2), (x + r + 2, y), (x, y + r + 2), (x - r - 2, y)], fill=255, outline=0, width=2)
    else:
        raise ValueError(f"unknown marker {kind!r}")


def marker_xs(x0: float, x1: float, k: int, n: int, step: float = MARKER_STEP) -> list[float]:
    """Marker positions for series k of n: every `step` px, offset so series don't stack."""
    xs, x = [], x0 + step * (k + 0.5) / n
    while x < x1 - 4:
        xs.append(x)
        x += step
    return xs


def y_at(pts, x: float) -> float:
    """y of the polyline at x (pts sorted by x); clamps outside the range."""
    if x <= pts[0][0]:
        return pts[0][1]
    for (xa, ya), (xb, yb) in zip(pts, pts[1:]):
        if xa <= x <= xb:
            return ya if xb == xa else ya + (yb - ya) * (x - xa) / (xb - xa)
    return pts[-1][1]


def line_with_markers(d: ImageDraw.ImageDraw, pts, style: LineStyle, k: int, n: int) -> None:
    styled_line(d, pts, style)
    if not pts:
        return
    for x in marker_xs(pts[0][0], pts[-1][0], k, n):
        draw_marker(d, x, y_at(pts, x), style.marker)
    draw_marker(d, pts[-1][0], pts[-1][1], style.marker)


def legend_sample(d: ImageDraw.ImageDraw, x: float, y: float, style: LineStyle, length: int = 24) -> None:
    styled_line(d, [(x, y), (x + length, y)], style)
    draw_marker(d, x + length / 2, y, style.marker)
```

`server/inkboard_server/draw/icons.py`:
```python
"""Geometric weather icons (no icon font) and the WMO code mapping (spec §4)."""
import math

from PIL import ImageDraw

from .fonts import font


def wmo_info(code: int) -> tuple[str, str]:
    """(label, icon kind) for an Open-Meteo WMO weather code."""
    if code == 0:
        return "Clear", "sun"
    if code == 1:
        return "Mostly clear", "sun"
    if code == 2:
        return "Partly cloudy", "part"
    if code == 3:
        return "Overcast", "cloud"
    if code in (45, 48):
        return "Fog", "fog"
    if 51 <= code <= 67 or 80 <= code <= 82:
        return "Rain", "rain"
    if 71 <= code <= 77 or code in (85, 86):
        return "Snow", "snow"
    if 95 <= code <= 99:
        return "Storms", "storm"
    return "—", "cloud"


def _sun(d, cx, cy, r):
    w = max(2, int(r * 0.12))
    d.ellipse([cx - r * .45, cy - r * .45, cx + r * .45, cy + r * .45], outline=0, width=w)
    for k in range(8):
        a = k * math.pi / 4
        d.line([(cx + math.cos(a) * r * .65, cy + math.sin(a) * r * .65),
                (cx + math.cos(a) * r * .95, cy + math.sin(a) * r * .95)], fill=0, width=max(2, int(r * .1)))


def _cloud(d, cx, cy, r):
    w = max(2, int(r * .1))
    parts = [(cx - r * .45, cy + r * .1, r * .38), (cx, cy - r * .15, r * .5), (cx + r * .45, cy + r * .12, r * .36)]
    for x, y, rr in parts:
        d.ellipse([x - rr, y - rr, x + rr, y + rr], outline=0, width=w)
    for x, y, rr in parts:
        d.ellipse([x - rr + w, y - rr + w, x + rr - w, y + rr - w], fill=255)
    d.rectangle([cx - r * .45, cy + r * .1, cx + r * .45, cy + r * .48 - w], fill=255)
    d.line([(cx - r * .45, cy + r * .48), (cx + r * .45, cy + r * .48)], fill=0, width=w)


def icon(d: ImageDraw.ImageDraw, kind: str, cx: float, cy: float, r: float) -> None:
    if kind == "sun":
        _sun(d, cx, cy, r)
    elif kind == "part":
        _sun(d, cx - r * .3, cy - r * .3, r * .7)
        _cloud(d, cx + r * .1, cy + r * .15, r * .8)
    elif kind == "cloud":
        _cloud(d, cx, cy, r)
    elif kind in ("rain", "snow", "storm"):
        _cloud(d, cx, cy - r * .2, r * .85)
        for k in range(3):
            x = cx - r * .35 + k * r * .35
            if kind == "snow":
                d.text((x, cy + r * .55), "*", font=font(max(8, int(r * .5)), bold=True), fill=0, anchor="mm")
            elif kind == "storm" and k == 1:
                d.line([(x + r * .1, cy + r * .3), (x - r * .1, cy + r * .55), (x + r * .1, cy + r * .55),
                        (x - r * .1, cy + r * .85)], fill=0, width=max(2, int(r * .1)))
            else:
                d.line([(x, cy + r * .35), (x - r * .12, cy + r * .75)], fill=0, width=max(2, int(r * .1)))
    elif kind == "fog":
        for k in range(4):
            y = cy - r * .4 + k * r * .27
            d.line([(cx - r * .7, y), (cx + r * .7, y)], fill=0, width=max(2, int(r * .1)))
    else:
        raise ValueError(f"unknown icon {kind!r}")
```

`server/inkboard_server/draw/calendar.py`:
```python
"""Month grid, Sunday first, today inverted."""
import calendar
from datetime import date

from PIL import ImageDraw

from .fonts import font


def month_weeks(year: int, month: int) -> list[list[int]]:
    return calendar.Calendar(firstweekday=6).monthdayscalendar(year, month)


def month_grid(d: ImageDraw.ImageDraw, x: float, y: float, w: float, today: date, big: bool = False) -> int:
    weeks = month_weeks(today.year, today.month)
    cw, ch = w / 7, (26 if big else 22)
    head, num, num_bold = font(12 if big else 11, bold=True), font(15 if big else 13), font(15 if big else 13, bold=True)
    for i, letter in enumerate("SMTWTFS"):
        d.text((x + cw * i + cw / 2, y + ch / 2), letter, font=head, fill=0, anchor="mm")
    d.line([(x + 4, y + ch), (x + w - 4, y + ch)], fill=0)
    for r, week in enumerate(weeks):
        for c, day in enumerate(week):
            if not day:
                continue
            cx, cy = x + cw * c + cw / 2, y + ch * (r + 1) + ch / 2 + 2
            if day == today.day:
                d.rounded_rectangle([cx - cw / 2 + 3, cy - ch / 2 + 1, cx + cw / 2 - 3, cy + ch / 2 - 1], 4, fill=0)
                d.text((cx, cy), str(day), font=num_bold, fill=255, anchor="mm")
            else:
                d.text((cx, cy), str(day), font=num, fill=0, anchor="mm")
    return int(ch * (len(weeks) + 1) + 4)
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `uv run pytest tests/test_draw.py -v`
Expected: all passed. If `test_icons_stay_in_bounds[storm]` or `[snow]` fails by a few
pixels, shrink the offending offsets in `icon` (not the test bound): the bound is the
contract that icons don't bleed into neighbouring text.

- [ ] **Step 5: Commit**

```bash
git add server/inkboard_server/draw server/tests/test_draw.py
git commit -m "Server: drawing helpers (line styles, markers, weather icons, month grid)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 7: Market data pipeline

**Files:**
- Create: `server/inkboard_server/market_data.py`
- Test: `server/tests/test_market_data.py`

**Interfaces:**
- Produces:
  - `SeriesSummary(id, points: tuple[tuple[date, float], ...], last, ratio, yoy: float | None, last_date: date, window_start: date, short_history: bool)`
  - `weekly_grid(today, years) -> list[date]`
  - `resample(obs, grid) -> list[tuple[date, float]]`
  - `summarize(series_id, obs: list[tuple[date, float]], today, years) -> SeriesSummary`
    (raises `ValueError`)
  - `y_range(series: Sequence[SeriesSummary]) -> tuple[float, float]`
  - `log_ticks(lo, hi) -> list[float]`

- [ ] **Step 1: Write the failing tests**

`server/tests/test_market_data.py`:
```python
from datetime import date, timedelta

import pytest

from inkboard_server.market_data import log_ticks, resample, summarize, weekly_grid, y_range

SUN = date(2026, 9, 27)  # a Sunday
WED = date(2026, 9, 30)


def test_weekly_grid_on_sunday():
    g = weekly_grid(SUN, 1)
    assert g[-1] == SUN
    assert all(d.weekday() == 6 for d in g)
    assert SUN - timedelta(days=366) <= g[0] <= SUN - timedelta(days=358)


def test_weekly_grid_midweek_ends_today():
    g = weekly_grid(WED, 1)
    assert g[-1] == WED and g[-2] == SUN


def test_resample_forward_fills():
    obs = [(date(2026, 1, 1), 10.0), (date(2026, 2, 1), 20.0)]
    grid = [date(2025, 12, 28), date(2026, 1, 4), date(2026, 1, 25), date(2026, 2, 1), date(2026, 2, 8)]
    assert resample(obs, grid) == [(date(2026, 1, 4), 10.0), (date(2026, 1, 25), 10.0),
                                   (date(2026, 2, 1), 20.0), (date(2026, 2, 8), 20.0)]


def daily(start: date, days: int, f):
    return [(start + timedelta(days=i), f(i)) for i in range(days)]


def test_summarize_normalizes_to_mean():
    obs = daily(date(2020, 1, 1), 2500, lambda i: 100.0 + i)
    s = summarize("x", obs, SUN, 5)
    mean_ratio = sum(v for _, v in s.points) / len(s.points)
    assert mean_ratio == pytest.approx(1.0)
    assert s.ratio == pytest.approx(s.points[-1][1])
    assert s.last == 100.0 + (SUN - date(2020, 1, 1)).days  # observations after today are ignored
    assert s.yoy == pytest.approx(s.last / (s.last - 364) - 1, rel=1e-3)
    assert not s.short_history


def test_summarize_ignores_future_and_nonpositive():
    obs = daily(date(2024, 1, 1), 2000, lambda i: 0.0 if i == 5 else 50.0)
    s = summarize("x", obs, SUN, 1)
    assert s.last_date <= SUN
    assert all(v > 0 for _, v in s.points)


def test_summarize_short_history():
    obs = daily(date(2025, 6, 1), 400, lambda i: 10.0)
    s = summarize("x", obs, SUN, 5)
    assert s.short_history and s.window_start >= date(2025, 6, 1)
    assert s.yoy == pytest.approx(0.0)


def test_summarize_without_year_of_history_has_no_yoy():
    s = summarize("x", daily(date(2026, 6, 1), 100, lambda i: 10.0), SUN, 5)
    assert s.yoy is None


def test_summarize_no_data_in_window_raises():
    with pytest.raises(ValueError, match="x"):
        summarize("x", [(date(2010, 1, 1), 1.0)], date(2009, 1, 1), 5)


def test_y_range_and_ticks():
    a = summarize("a", daily(date(2021, 1, 1), 2100, lambda i: 1.0 + i / 700), SUN, 5)
    lo, hi = y_range([a])
    assert lo < min(v for _, v in a.points) and hi > max(v for _, v in a.points)
    assert log_ticks(0.4, 2.2) == [0.5, 1, 1.5, 2]
    assert log_ticks(0.2, 3.5) == [0.25, 0.5, 1, 1.5, 2, 3]
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `uv run pytest tests/test_market_data.py -v`
Expected: FAIL with `ModuleNotFoundError: No module named 'inkboard_server.market_data'`

- [ ] **Step 3: Implement `market_data.py`**

`server/inkboard_server/market_data.py`:
```python
"""Weekly resampling and normalization for the market chart (spec §5.1 data pipeline)."""
from __future__ import annotations

from dataclasses import dataclass
from datetime import date, timedelta
from typing import Sequence

TICKS = (0.25, 0.5, 1, 1.5, 2, 3)


@dataclass(frozen=True)
class SeriesSummary:
    id: str
    points: tuple[tuple[date, float], ...]  # normalized: value / window mean
    last: float                             # latest raw value
    ratio: float                            # last / window mean
    yoy: float | None                       # last vs the weekly point 52 weeks earlier
    last_date: date                         # date of the latest observation used
    window_start: date
    short_history: bool


def weekly_grid(today: date, years: int) -> list[date]:
    """Sundays in the last `years` years, plus today if today is not a Sunday."""
    start = today - timedelta(days=round(365.25 * years))
    d = start + timedelta(days=(6 - start.weekday()) % 7)
    grid = []
    while d <= today:
        grid.append(d)
        d += timedelta(days=7)
    if not grid or grid[-1] != today:
        grid.append(today)
    return grid


def resample(obs: list[tuple[date, float]], grid: list[date]) -> list[tuple[date, float]]:
    """Last observation on or before each grid date; grid dates before the first obs are dropped."""
    out, i, last = [], 0, None
    for g in grid:
        while i < len(obs) and obs[i][0] <= g:
            last = obs[i][1]
            i += 1
        if last is not None:
            out.append((g, last))
    return out


def summarize(series_id: str, obs: list[tuple[date, float]], today: date, years: int) -> SeriesSummary:
    obs = sorted((d, v) for d, v in obs if d <= today and v > 0)
    grid = weekly_grid(today, years)
    pts = resample(obs, grid)
    if not pts:
        raise ValueError(f"{series_id}: no observations in the window")
    mean = sum(v for _, v in pts) / len(pts)
    last = pts[-1][1]
    year_ago = today - timedelta(weeks=52)
    prior = [v for d, v in pts if d <= year_ago]
    return SeriesSummary(
        id=series_id,
        points=tuple((d, v / mean) for d, v in pts),
        last=last,
        ratio=last / mean,
        yoy=(last / prior[-1] - 1) if prior else None,
        last_date=obs[-1][0],
        window_start=pts[0][0],
        short_history=pts[0][0] > grid[0] + timedelta(days=31),
    )


def y_range(series: Sequence[SeriesSummary]) -> tuple[float, float]:
    values = [v for s in series for _, v in s.points]
    return min(values) * 0.92, max(values) * 1.08


def log_ticks(lo: float, hi: float) -> list[float]:
    return [g for g in TICKS if lo <= g <= hi]
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `uv run pytest tests/test_market_data.py -v`
Expected: 9 passed

- [ ] **Step 5: Commit**

```bash
git add server/inkboard_server/market_data.py server/tests/test_market_data.py
git commit -m "Server: market data pipeline (weekly resample, normalize, 1-yr change)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 8: `market_trends` widget

**Files:**
- Create: `server/inkboard_server/widgets/market_trends.py`
- Modify: `server/inkboard_server/widgets/__init__.py` (import the module),
  `server/tests/conftest.py` (add `render_widget` and `ctx`)
- Test: `server/tests/test_market_trends.py`
- Goldens: `server/tests/goldens/market_trends_{third,two_thirds,full}.png`,
  `market_trends_short_series.png`

**Interfaces:**
- Consumes:
  - `Widget`, `register`, `Size`, `Box`, `ParamSpec`, `RenderContext` and `WidgetData`
    (Task 5)
  - `id_list` and `int_in_range` (Task 5)
  - `CATALOG` and `DEFAULT_SERIES` (Task 5)
  - `summarize`, `y_range` and `log_ticks` (Task 7)
  - `STYLES`, `line_with_markers` and `legend_sample` (Task 6)
  - `font` (Task 1)
  - `Sources.fred_series` (Task 4)
- Produces:
  - `MarketPayload(series: tuple[SeriesSummary, ...], years: int, today: date)`
  - `MarketTrends` (type_name `market_trends`)
  - conftest: `render_widget(widget, data, ctx) -> Image` (mode "1", widget-sized) and
    the fixture `ctx` (`RenderContext(FIXTURES_TODAY, LA)`)

- [ ] **Step 1: Add the render helper to conftest**

Append to `server/tests/conftest.py`:
```python
from inkboard_server.frame import THRESHOLD
from inkboard_server.widgets.base import WIDGET_H, Box, RenderContext


def render_widget(widget, data, ctx):
    img = Image.new("L", (widget.size.width, WIDGET_H), 255)
    widget.render(img, Box(0, 0, widget.size.width, WIDGET_H), data, ctx)
    return img.point(lambda p: 255 if p > THRESHOLD else 0).convert("1")


@pytest.fixture
def ctx():
    return RenderContext(FIXTURES_TODAY, LA)
```

- [ ] **Step 2: Write the failing tests**

`server/tests/test_market_trends.py`:
```python
from dataclasses import replace
from datetime import date

import pytest

from inkboard_server.market_data import summarize
from inkboard_server.series import DEFAULT_SERIES
from inkboard_server.sources.base import NoData
from inkboard_server.widgets import REGISTRY
from inkboard_server.widgets.base import Size, WidgetData
from inkboard_server.widgets.market_trends import MarketTrends

from .conftest import render_widget

OPTS = {"series": DEFAULT_SERIES, "years": 5}


def test_registered():
    assert REGISTRY["market_trends"] is MarketTrends


def test_fetch_builds_one_summary_per_series(sources, ctx):
    data = MarketTrends(Size.TWO_THIRDS, OPTS).fetch(sources, ctx)
    assert [s.id for s in data.payload.series] == list(DEFAULT_SERIES)
    assert len(data.sources) == 4 and not data.stale
    assert all(s.points[-1][0] == ctx.today for s in data.payload.series)


def test_fetch_propagates_nodata(sources, upstream, ctx):
    upstream.fail = True
    with pytest.raises(NoData):
        MarketTrends(Size.FULL, OPTS).fetch(sources, ctx)


def test_attributions_ordered_unique():
    w = MarketTrends(Size.FULL, OPTS)
    assert w.attributions() == ["FRED", "S&P DJI", "Coinbase", "Freddie Mac", "Realtor.com"]
    assert MarketTrends(Size.FULL, {"series": ("ust10y",), "years": 5}).attributions() == ["FRED"]


@pytest.mark.parametrize("size,name", [(Size.THIRD, "third"), (Size.TWO_THIRDS, "two_thirds"), (Size.FULL, "full")])
def test_golden(size, name, sources, ctx, golden):
    w = MarketTrends(size, OPTS)
    golden(f"market_trends_{name}", render_widget(w, w.fetch(sources, ctx), ctx))


def test_golden_short_series_label(sources, ctx, golden):
    # A series with less history than the window is labeled "(since YYYY)" and starts later on the x-axis.
    w = MarketTrends(Size.TWO_THIRDS, {"series": ("sp500", "home_la"), "years": 5})
    data = w.fetch(sources, ctx)
    home = sources.fred_series("MEDLISPRI31080").data
    recent = [(date.fromisoformat(d), v) for d, v in home if d >= "2024-01-01"]
    short = summarize("home_la", recent, ctx.today, 5)
    assert short.short_history
    payload = replace(data.payload, series=(data.payload.series[0], short))
    golden("market_trends_short_series", render_widget(w, WidgetData(payload, data.sources), ctx))
```

- [ ] **Step 3: Run the tests to verify they fail**

Run: `uv run pytest tests/test_market_trends.py -v`
Expected: FAIL with `ModuleNotFoundError: No module named 'inkboard_server.widgets.market_trends'`

- [ ] **Step 4: Implement the widget**

`server/inkboard_server/widgets/market_trends.py`:
```python
"""Market trends: several series on one log chart, each divided by its own window mean."""
from __future__ import annotations

import math
from dataclasses import dataclass
from datetime import date

from PIL import Image, ImageDraw

from ..draw.fonts import font
from ..draw.lines import STYLES, legend_sample, line_with_markers
from ..market_data import SeriesSummary, log_ticks, summarize, y_range
from ..series import CATALOG, DEFAULT_SERIES
from .base import Box, ParamSpec, RenderContext, Size, Widget, WidgetData, register
from .params import id_list, int_in_range


@dataclass(frozen=True)
class MarketPayload:
    series: tuple[SeriesSummary, ...]
    years: int
    today: date


@register
class MarketTrends(Widget):
    type_name = "market_trends"
    supported_sizes = frozenset(Size)
    params = {
        "series": ParamSpec(id_list(CATALOG, 4), default=DEFAULT_SERIES, fmt=",".join),
        "years": ParamSpec(int_in_range(1, 10), default=5),
    }

    def fetch(self, sources, ctx: RenderContext) -> WidgetData:
        results, summaries = [], []
        for sid in self.options["series"]:
            r = sources.fred_series(CATALOG[sid].fred_id)
            obs = [(date.fromisoformat(d), v) for d, v in r.data]
            summaries.append(summarize(sid, obs, ctx.today, self.options["years"]))
            results.append(r)
        return WidgetData(MarketPayload(tuple(summaries), self.options["years"], ctx.today), results)

    def attributions(self) -> list[str]:
        out: list[str] = []
        for sid in self.options["series"]:
            for a in CATALOG[sid].attribution:
                if a not in out:
                    out.append(a)
        return out

    def render(self, img: Image.Image, box: Box, data: WidgetData, ctx: RenderContext) -> None:
        p: MarketPayload = data.payload
        c = Image.new("L", (box.w, box.h), 255)
        d = ImageDraw.Draw(c)
        narrow = self.size is Size.THIRD
        pad = 12
        d.text((pad, pad), "Markets", font=font(18 if narrow else 22, bold=True), fill=0)
        sub = f"{p.years}-yr · ×avg · log" if narrow else f"{p.years}-yr, × own average, log"
        d.text((pad, pad + (23 if narrow else 28)), sub, font=font(12), fill=0)
        asof = min(s.last_date for s in p.series)
        d.text((box.w - pad, pad + 4), f"as of {asof:%b} {asof.day}", font=font(11), fill=0, anchor="ra")

        row_h = 20 if narrow else 22
        table_h = len(p.series) * row_h + 36
        top, bottom = pad + 50, box.h - pad - 18 - table_h
        self._chart(d, p, pad + 30, top, box.w - pad - 6, bottom, narrow)
        self._table(d, p, pad, bottom + 40, box.w - pad, row_h, narrow)
        img.paste(c, (box.x, box.y))

    def _chart(self, d, p: MarketPayload, left, top, right, bottom, narrow) -> None:
        lo, hi = y_range(p.series)
        llo, lhi = math.log(lo), math.log(hi)
        start = min(s.points[0][0] for s in p.series)
        span = max((p.today - start).days, 1)

        def X(dt: date) -> float:
            return left + (dt - start).days / span * (right - left)

        def Y(v: float) -> float:
            return bottom - (math.log(v) - llo) / (lhi - llo) * (bottom - top)

        axis = font(11)
        for g in log_ticks(lo, hi):
            y = Y(g)
            if g == 1:
                d.line([(left, y), (right, y)], fill=0, width=2)
            else:
                for x in range(int(left), int(right), 6):
                    d.point((x, y), fill=0)
            d.text((left - 4, y), f"{g:g}×", font=axis, fill=0, anchor="rm")
        d.line([(left, bottom), (right, bottom)], fill=0)
        for yr in range(start.year + 1, p.today.year + 1):
            x = X(date(yr, 1, 1))
            d.line([(x, bottom), (x, bottom + 4)], fill=0)
            d.text((x, bottom + 6), f"'{yr % 100:02d}" if narrow else str(yr), font=axis, fill=0, anchor="mt")
        n = len(p.series)
        for k, s in enumerate(p.series):
            line_with_markers(d, [(X(dt), Y(v)) for dt, v in s.points], STYLES[k], k, n)

    def _table(self, d, p: MarketPayload, x0, ty, x1, row_h, narrow) -> None:
        value_font, name_font, head_font = font(12 if narrow else 13), font(12 if narrow else 13, bold=True), font(11)
        cols = [x1 - 60, x1] if narrow else [x1 - 150, x1 - 70, x1]
        heads = ["now", "×avg"] if narrow else ["now", "×avg", "1 yr"]
        for x, h in zip(cols, heads):
            d.text((x, ty - 4), h, font=head_font, fill=0, anchor="rb")
        d.line([(x0, ty), (x1, ty)], fill=0)
        for k, s in enumerate(p.series):
            sd = CATALOG[s.id]
            y = ty + 12 + k * row_h
            legend_sample(d, x0, y, STYLES[k])
            name = sd.short if narrow else sd.label
            if s.short_history and not narrow:
                name += f" (since {s.window_start.year})"
            d.text((x0 + 32, y), name, font=name_font, fill=0, anchor="lm")
            values = [sd.fmt(s.last), f"{s.ratio:.2f}×"]
            if not narrow:
                values.append("—" if s.yoy is None else f"{s.yoy * 100:+.0f}%")
            for x, v in zip(cols, values):
                d.text((x, y), v, font=value_font, fill=0, anchor="rm")
```

Replace `server/inkboard_server/widgets/__init__.py` with:
```python
"""Widgets. Importing this package registers every concrete widget."""
from .base import REGISTRY, WIDGET_H, Box, ParamSpec, RenderContext, Size, Widget, WidgetData, register
from . import market_trends  # noqa: F401  (registers)

__all__ = ["REGISTRY", "WIDGET_H", "Box", "ParamSpec", "RenderContext", "Size", "Widget", "WidgetData", "register"]
```

- [ ] **Step 5: Generate the goldens and inspect them**

Run: `uv run pytest tests/test_market_trends.py --update-goldens -q`, then open each
`tests/goldens/market_trends_*.png` with the Read tool.

Check each image against spec §5.1 and against the reference mockup
`docs/superpowers/specs/assets/2026-09-27-dashboard-mockup.png`:
- The header and "as of" are present.
- There are 4 distinct lines, each with markers.
- ×1 is solid.
- The year ticks are present.
- The legend table has markers and no clipped text.
- At 1/3 width the table has 2 value columns.

If anything is clipped or overlapping, fix the layout constants and regenerate.

- [ ] **Step 6: Run the tests to verify they pass against the goldens**

Run: `uv run pytest tests/test_market_trends.py -v`
Expected: 8 passed

- [ ] **Step 7: Commit**

```bash
git add server/inkboard_server/widgets server/tests/conftest.py server/tests/test_market_trends.py server/tests/goldens
git commit -m "Server: market_trends widget (log chart, markers, legend table) + goldens

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 9: `calendar_weather` widget

**Files:**
- Create: `server/inkboard_server/widgets/calendar_weather.py`
- Modify: `server/inkboard_server/widgets/__init__.py` (import the module)
- Test: `server/tests/test_calendar_weather.py`
- Goldens: `server/tests/goldens/calendar_weather_{third,two_thirds,full}.png`

**Interfaces:**
- Consumes:
  - `Widget`, `register`, `Size`, `ParamSpec`, `RenderContext` and `WidgetData` (Task 5)
  - `coordinate`, `fmt_coord` and `one_of` (Task 5)
  - `wmo_info` and `icon` (Task 6)
  - `month_grid` (Task 6)
  - `font` (Task 1)
  - `Sources.weather` (Task 4)
- Produces:
  - `DayForecast(day: date, code: int, hi: float, lo: float)`
  - `WeatherPayload(temp: float, code: int, today: DayForecast | None, upcoming: tuple[DayForecast, ...])`
  - `build_payload(data: dict, today: date) -> WeatherPayload`
  - `deg(v: float) -> str`
  - `CalendarWeather` (type_name `calendar_weather`, params `lat`, `lon`, `units`)

- [ ] **Step 1: Write the failing tests**

`server/tests/test_calendar_weather.py`:
```python
from datetime import date, timedelta

import pytest

from inkboard_server.widgets import REGISTRY
from inkboard_server.widgets.base import Size, WidgetData
from inkboard_server.widgets.calendar_weather import CalendarWeather, build_payload, deg

from .conftest import render_widget

OPTS = {"lat": 34.1, "lon": -118.2, "units": "imperial"}


def raw(start: date, n: int) -> dict:
    return {"current": {"temp": 70.4, "code": 2},
            "daily": [{"date": (start + timedelta(days=i)).isoformat(), "code": 0, "hi": 80.0 + i, "lo": 60.0}
                      for i in range(n)]}


def test_registered():
    assert REGISTRY["calendar_weather"] is CalendarWeather


def test_build_payload_normal():
    p = build_payload(raw(date(2026, 9, 27), 8), date(2026, 9, 27))
    assert p.today.day == date(2026, 9, 27) and p.today.hi == 80.0
    assert [d.day for d in p.upcoming][:2] == [date(2026, 9, 28), date(2026, 9, 29)]
    assert len(p.upcoming) == 7


def test_build_payload_after_midnight():
    # cache still holds yesterday's forecast (daily[0] == yesterday)
    p = build_payload(raw(date(2026, 9, 27), 8), date(2026, 9, 28))
    assert p.today.day == date(2026, 9, 28) and p.today.hi == 81.0
    assert p.upcoming[0].day == date(2026, 9, 29)


def test_build_payload_without_today():
    p = build_payload(raw(date(2026, 9, 20), 3), date(2026, 9, 28))
    assert p.today is None and p.upcoming == ()


def test_render_without_today_does_not_crash(ctx):
    w = CalendarWeather(Size.THIRD, OPTS)
    render_widget(w, WidgetData(build_payload(raw(date(2020, 1, 1), 3), ctx.today)), ctx)


def test_deg_rounding():
    assert deg(-0.4) == "0°"
    assert deg(72.5) == "72°"  # banker's rounding is fine; just never "-0°"
    assert deg(-3.6) == "-4°"


def test_fetch_uses_request_tz(sources, upstream, ctx):
    data = CalendarWeather(Size.THIRD, OPTS).fetch(sources, ctx)
    assert data.sources[0].key.endswith('"tz":"America/Los_Angeles","units":"imperial"}')
    assert data.payload.today.day == ctx.today
    assert upstream.calls["weather"] == 1


def test_attributions():
    assert CalendarWeather(Size.THIRD, OPTS).attributions() == ["Open-Meteo"]


@pytest.mark.parametrize("size,name", [(Size.THIRD, "third"), (Size.TWO_THIRDS, "two_thirds"), (Size.FULL, "full")])
def test_golden(size, name, sources, ctx, golden):
    w = CalendarWeather(size, OPTS)
    golden(f"calendar_weather_{name}", render_widget(w, w.fetch(sources, ctx), ctx))
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `uv run pytest tests/test_calendar_weather.py -v`
Expected: FAIL with `ModuleNotFoundError: No module named 'inkboard_server.widgets.calendar_weather'`

- [ ] **Step 3: Implement the widget**

`server/inkboard_server/widgets/calendar_weather.py`:
```python
"""Weather-first calendar: current conditions, forecast rows, date and month grid (spec §5.2)."""
from __future__ import annotations

from dataclasses import dataclass
from datetime import date

from PIL import Image, ImageDraw

from ..draw.calendar import month_grid
from ..draw.fonts import font
from ..draw.icons import icon, wmo_info
from .base import Box, ParamSpec, RenderContext, Size, Widget, WidgetData, register
from .params import coordinate, fmt_coord, one_of


@dataclass(frozen=True)
class DayForecast:
    day: date
    code: int
    hi: float
    lo: float


@dataclass(frozen=True)
class WeatherPayload:
    temp: float
    code: int
    today: DayForecast | None
    upcoming: tuple[DayForecast, ...]


def build_payload(data: dict, today: date) -> WeatherPayload:
    """Pick days by date, not position: the cache may still hold yesterday's forecast."""
    days = [DayForecast(date.fromisoformat(x["date"]), x["code"], x["hi"], x["lo"]) for x in data["daily"]]
    return WeatherPayload(
        temp=data["current"]["temp"],
        code=data["current"]["code"],
        today=next((x for x in days if x.day == today), None),
        upcoming=tuple(x for x in days if x.day > today),
    )


def deg(v: float) -> str:
    return f"{int(round(v)) + 0}°"


@register
class CalendarWeather(Widget):
    type_name = "calendar_weather"
    supported_sizes = frozenset(Size)
    params = {
        "lat": ParamSpec(coordinate(-90, 90), fmt=fmt_coord),
        "lon": ParamSpec(coordinate(-180, 180), fmt=fmt_coord),
        "units": ParamSpec(one_of("imperial", "metric"), default="imperial"),
    }

    def fetch(self, sources, ctx: RenderContext) -> WidgetData:
        o = self.options
        r = sources.weather(o["lat"], o["lon"], o["units"], ctx.tz.key)
        return WidgetData(build_payload(r.data, ctx.today), [r])

    def attributions(self) -> list[str]:
        return ["Open-Meteo"]

    def render(self, img: Image.Image, box: Box, data: WidgetData, ctx: RenderContext) -> None:
        p: WeatherPayload = data.payload
        c = Image.new("L", (box.w, box.h), 255)
        d = ImageDraw.Draw(c)
        pad, today = 14, ctx.today
        if self.size is Size.THIRD:
            iw = box.w - 2 * pad
            y = pad
            y += self._now(d, pad, y, p) + 4
            y += self._rows(d, pad, y, iw, p.upcoming[:5]) + 8
            d.line([(pad, y), (box.w - pad, y)], fill=0)
            y += 8
            d.text((pad, y), f"{today:%a}, {today:%B} {today.day}", font=font(20, bold=True), fill=0)
            month_grid(d, pad, y + 30, iw, today)
        else:
            full = self.size is Size.FULL
            split = int(box.w * (0.36 if full else 0.45))
            lx, lw = pad, split - 2 * pad
            rx, rw = split + pad, box.w - split - 2 * pad
            d.line([(split, pad), (split, box.h - pad)], fill=0)
            y = pad + 6
            y += self._now(d, lx, y, p) + 14
            self._rows(d, lx, y, lw, p.upcoming[:7 if full else 5])
            y = pad + 6
            d.text((rx, y), f"{today:%A}", font=font(18, bold=True), fill=0)
            month = f"{today:%B}" if full else f"{today:%b}"
            d.text((rx, y + 24), f"{month} {today.day}, {today.year}", font=font(26, bold=True), fill=0)
            month_grid(d, rx, y + 70, rw, today, big=True)
        img.paste(c, (box.x, box.y))

    def _now(self, d, x, y, p: WeatherPayload) -> int:
        label, kind = wmo_info(p.code)
        r = 34
        icon(d, kind, x + r + 4, y + r + 4, r)
        tx = x + 2 * r + 18
        d.text((tx, y + 2), deg(p.temp), font=font(46, bold=True), fill=0)
        d.text((tx, y + 54), label, font=font(14), fill=0)
        hl = f"H {deg(p.today.hi)}  L {deg(p.today.lo)}" if p.today else "H —  L —"
        d.text((tx, y + 72), hl, font=font(14, bold=True), fill=0)
        return 2 * r + 20

    def _rows(self, d, x, y, w, days: tuple[DayForecast, ...]) -> int:
        rh = 30
        for k, day in enumerate(days):
            yy = y + k * rh + rh / 2
            d.text((x + 4, yy), f"{day.day:%a}", font=font(15, bold=True), fill=0, anchor="lm")
            icon(d, wmo_info(day.code)[1], x + 70, yy, 12)
            d.text((x + w - 50, yy), deg(day.hi), font=font(15, bold=True), fill=0, anchor="rm")
            d.text((x + w - 4, yy), deg(day.lo), font=font(15), fill=0, anchor="rm")
            if k:
                for xx in range(int(x + 4), int(x + w - 4), 4):
                    d.point((xx, yy - rh / 2), fill=0)
        return len(days) * rh
```

Add to `server/inkboard_server/widgets/__init__.py`, after the `market_trends` import:
```python
from . import calendar_weather  # noqa: F401  (registers)
```

- [ ] **Step 4: Generate the goldens and inspect them**

Run: `uv run pytest tests/test_calendar_weather.py --update-goldens -q`, then Read each
`tests/goldens/calendar_weather_*.png`.

Check them against spec §5.2 and the reference mockup:
- Weather is first.
- The forecast rows are 5 at 1/3 and 2/3, and 7 at full.
- The date line isn't clipped. At 2/3 it reads `Sep D, YYYY`.
- Today is inverted in the grid.
- Nothing extends past the 464 px height.

Fix the layout and regenerate if needed.

- [ ] **Step 5: Run the tests to verify they pass**

Run: `uv run pytest tests/test_calendar_weather.py -v`
Expected: 11 passed

- [ ] **Step 6: Commit**

```bash
git add server/inkboard_server/widgets server/tests/test_calendar_weather.py server/tests/goldens
git commit -m "Server: calendar_weather widget (weather-first) + goldens

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 10: Compositor

**Files:**
- Create: `server/inkboard_server/compositor.py`
- Test: `server/tests/test_compositor.py`
- Goldens: `server/tests/goldens/screen_default.png`, `screen_stale.png`, `screen_nodata.png`,
  `screen_render_error.png`

**Interfaces:**
- Consumes:
  - `Widget`, `WidgetData`, `Box`, `WIDGET_H` and `RenderContext` (Task 5)
  - `NoData` (Task 3)
  - `new_canvas`, `to_1bit` and `WIDTH` (Task 1)
  - `font` (Task 1)
- Produces:
  - `FOOTER_H = 16`
  - `FetchFailure(source: str)`
  - `fetch_widgets(widgets, sources, ctx) -> list[WidgetData | FetchFailure]`
  - `frame_versions(results) -> tuple`
  - `footer_time_text(results, tz) -> str`
  - `compose(widgets, results, ctx) -> Image` ("L")
  - `render_frame(widgets, results, ctx) -> Image` ("1")

- [ ] **Step 1: Write the failing tests**

`server/tests/test_compositor.py`:
```python
from datetime import timedelta

from inkboard_server.compositor import (
    FetchFailure, compose, fetch_widgets, footer_time_text, frame_versions, render_frame,
)
from inkboard_server.series import DEFAULT_SERIES
from inkboard_server.widgets.base import Size
from inkboard_server.widgets.calendar_weather import CalendarWeather
from inkboard_server.widgets.market_trends import MarketTrends

from .conftest import LA


def default_widgets():
    return [MarketTrends(Size.TWO_THIRDS, {"series": DEFAULT_SERIES, "years": 5}),
            CalendarWeather(Size.THIRD, {"lat": 34.1, "lon": -118.2, "units": "imperial"})]


class Exploding(CalendarWeather):
    def render(self, img, box, data, ctx):
        raise RuntimeError("boom")


def test_default_screen(sources, ctx, golden):
    ws = default_widgets()
    golden("screen_default", render_frame(ws, fetch_widgets(ws, sources, ctx), ctx))


def test_footer_time_is_newest_fetch_in_tz(sources, ctx):
    ws = default_widgets()
    assert footer_time_text(fetch_widgets(ws, sources, ctx), LA) == "updated 10:00 AM"
    assert footer_time_text([FetchFailure("fred")], LA) == "updated —"


def test_stale_screen(sources, upstream, clock, ctx, golden):
    ws = default_widgets()
    fresh = fetch_widgets(ws, sources, ctx)
    clock.advance(hours=7)
    upstream.fail = True
    stale = fetch_widgets(ws, sources, ctx)
    assert all(r.stale for r in stale)
    assert frame_versions(stale) != frame_versions(fresh)
    golden("screen_stale", render_frame(ws, stale, ctx))


def test_nodata_column(sources, upstream, ctx, golden):
    upstream.fail = True
    ws = default_widgets()
    results = fetch_widgets(ws, sources, ctx)
    assert results == [FetchFailure("fred"), FetchFailure("weather")]
    assert frame_versions(results) == (("nodata", "fred"), ("nodata", "weather"))
    golden("screen_nodata", render_frame(ws, results, ctx))


def test_render_error_is_isolated(sources, ctx, golden):
    ws = [default_widgets()[0], Exploding(Size.THIRD, {"lat": 34.1, "lon": -118.2, "units": "imperial"})]
    img = compose(ws, fetch_widgets(ws, sources, ctx), ctx)
    assert img.mode == "L" and img.size == (800, 480)
    golden("screen_render_error", render_frame(ws, fetch_widgets(ws, sources, ctx), ctx))


def test_same_inputs_same_frame(sources, ctx):
    ws = default_widgets()
    a = render_frame(ws, fetch_widgets(ws, sources, ctx), ctx)
    b = render_frame(ws, fetch_widgets(ws, sources, ctx), ctx)
    assert a.tobytes() == b.tobytes()
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `uv run pytest tests/test_compositor.py -v`
Expected: FAIL with `ModuleNotFoundError: No module named 'inkboard_server.compositor'`

- [ ] **Step 3: Implement the compositor**

`server/inkboard_server/compositor.py`:
```python
"""Columns, dividers, footer and per-widget error isolation (spec §3.3)."""
from __future__ import annotations

import logging
from dataclasses import dataclass
from zoneinfo import ZoneInfo

from PIL import Image, ImageDraw

from .draw.fonts import font
from .frame import WIDTH, new_canvas, to_1bit
from .sources.base import NoData
from .widgets.base import WIDGET_H, Box, RenderContext, Widget, WidgetData

log = logging.getLogger(__name__)
FOOTER_H = 16


@dataclass(frozen=True)
class FetchFailure:
    source: str


def fetch_widgets(widgets: list[Widget], sources, ctx: RenderContext) -> list[WidgetData | FetchFailure]:
    out: list[WidgetData | FetchFailure] = []
    for w in widgets:
        try:
            out.append(w.fetch(sources, ctx))
        except NoData as e:
            out.append(FetchFailure(e.source))
        except Exception:
            log.exception("fetch failed in widget %s", w.type_name)
            out.append(FetchFailure(w.type_name))
    return out


def frame_versions(results) -> tuple:
    """Everything a render can see from sources; part of the frame-cache key."""
    out = []
    for r in results:
        if isinstance(r, FetchFailure):
            out.append(("nodata", r.source))
        else:
            out.extend(s.version for s in r.sources)
    return tuple(out)


def footer_time_text(results, tz: ZoneInfo) -> str:
    fetched = [s.fetched_at for r in results if isinstance(r, WidgetData) for s in r.sources]
    if not fetched:
        return "updated —"
    t = max(fetched).astimezone(tz)
    return f"updated {t.hour % 12 or 12}:{t.minute:02d} {'AM' if t.hour < 12 else 'PM'}"


def _message(img: Image.Image, box: Box, text: str) -> None:
    d = ImageDraw.Draw(img)
    d.text((box.x + box.w / 2, box.y + box.h / 2), text, font=font(14, bold=True), fill=0, anchor="mm")


def compose(widgets: list[Widget], results, ctx: RenderContext) -> Image.Image:
    img = new_canvas()
    x, dividers = 0, []
    for w, r in zip(widgets, results):
        box = Box(x, 0, w.size.width, WIDGET_H)
        if isinstance(r, FetchFailure):
            _message(img, box, f"No data yet: {r.source}")
        else:
            try:
                w.render(img, box, r, ctx)
            except Exception:
                log.exception("render failed in widget %s", w.type_name)
                img.paste(255, (box.x, box.y, box.x + box.w, box.y + box.h))
                _message(img, box, f"error: {w.type_name}")
        if x:
            dividers.append(x)
        x += box.w
    d = ImageDraw.Draw(img)
    for dx in dividers:  # after rendering: widgets paste over their whole box
        d.line([(dx, 8), (dx, WIDGET_H - 8)], fill=0)
    _footer(d, widgets, results, ctx)
    return img


def _footer(d: ImageDraw.ImageDraw, widgets, results, ctx: RenderContext) -> None:
    d.line([(0, WIDGET_H), (WIDTH, WIDGET_H)], fill=0)
    left = footer_time_text(results, ctx.tz)
    if any(isinstance(r, WidgetData) and r.stale for r in results):
        left += "   ⚠ stale"
    y = WIDGET_H + FOOTER_H // 2
    d.text((8, y), left, font=font(10), fill=0, anchor="lm")
    attrs: list[str] = []
    for w in widgets:
        for a in w.attributions():
            if a not in attrs:
                attrs.append(a)
    d.text((WIDTH - 8, y), " · ".join(attrs), font=font(10), fill=0, anchor="rm")


def render_frame(widgets: list[Widget], results, ctx: RenderContext) -> Image.Image:
    return to_1bit(compose(widgets, results, ctx))
```

- [ ] **Step 4: Generate the goldens and inspect them**

Run: `uv run pytest tests/test_compositor.py --update-goldens -q`, then Read the four
`tests/goldens/screen_*.png`.

- `screen_default` should look like the reference mockup with markers.
- `screen_stale` shows `updated 10:00 AM   ⚠ stale`.
- `screen_nodata` shows the two "No data yet" messages.
- `screen_render_error` shows the markets column plus `error: calendar_weather`.

- [ ] **Step 5: Run the tests to verify they pass**

Run: `uv run pytest tests/test_compositor.py -v`
Expected: 6 passed

- [ ] **Step 6: Commit**

```bash
git add server/inkboard_server/compositor.py server/tests/test_compositor.py server/tests/goldens
git commit -m "Server: compositor (columns, footer, stale marker, error isolation) + goldens

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 11: HTTP API (frame cache, ETag/304, rate limit, health, index)

**Files:**
- Create: `server/inkboard_server/ratelimit.py`, `server/inkboard_server/app.py`,
  `server/inkboard_server/main.py`
- Test: `server/tests/test_ratelimit.py`, `server/tests/test_api.py`
- Golden: `server/tests/goldens/api_footer_stale.png`

**Interfaces:**
- Consumes: everything above
- Produces:
  - `RateLimiter(capacity=30, per_seconds=3600, clock=time.monotonic, max_keys=10000)`,
    with `.check(key) -> float | None` (None = allowed, otherwise the seconds to wait)
  - `app.create_app(sources, *, clock=utcnow, limiter: RateLimiter | None = None, registry=None, client_ip_header: str | None = None, request_budget_s: float = REQUEST_BUDGET_S) -> FastAPI`
  - `app.REQUEST_BUDGET_S = 10.0`: the per-request deadline for cold fetches, which keeps
    the response well under the board's 20 s timeout (spec §6.2)
  - `app.etag_matches(header: str | None, etag: str) -> bool`
  - `main.app` (the production ASGI app)

- [ ] **Step 1: Write the failing rate-limiter tests**

`server/tests/test_ratelimit.py`:
```python
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
```

- [ ] **Step 2: Implement `ratelimit.py` and run its tests**

`server/inkboard_server/ratelimit.py`:
```python
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
```

Run: `uv run pytest tests/test_ratelimit.py -v`
Expected: 3 passed

- [ ] **Step 3: Write the failing API tests**

`server/tests/test_api.py`:
```python
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
```

- [ ] **Step 4: Run the tests to verify they fail**

Run: `uv run pytest tests/test_api.py -v`
Expected: FAIL with `ModuleNotFoundError: No module named 'inkboard_server.app'`

- [ ] **Step 5: Implement `app.py` and `main.py`**

`server/inkboard_server/app.py`:
```python
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
```

`server/inkboard_server/main.py`:
```python
"""Production entry point: uvicorn inkboard_server.main:app"""
import logging
import os
from pathlib import Path

from .app import create_app
from .sources import build_sources

logging.basicConfig(level=os.environ.get("LOG_LEVEL", "INFO"),
                    format="%(asctime)s %(levelname)s %(name)s %(message)s")
_key = os.environ.get("FRED_API_KEY", "")
if not _key:
    logging.getLogger(__name__).warning("FRED_API_KEY is not set: market widgets will show 'No data yet'")

app = create_app(build_sources(Path(os.environ.get("INKBOARD_CACHE_DIR", ".cache")), _key),
                 client_ip_header=os.environ.get("INKBOARD_CLIENT_IP_HEADER") or None)
```

- [ ] **Step 6: Generate the API golden, inspect it, run all tests**

Run: `uv run pytest tests/test_api.py --update-goldens -q`, then Read
`tests/goldens/api_footer_stale.png`. It must show `updated 10:00 AM   ⚠ stale`.

Run: `uv run pytest -v`
Expected: every test passes.

- [ ] **Step 7: Smoke-run the real app locally (live Open-Meteo, no FRED key)**

```bash
uv run uvicorn inkboard_server.main:app --port 8765 --no-access-log &
sleep 3
curl -s -o .cache/smoke-frame.png -w "%{http_code}\n" \
  "http://127.0.0.1:8765/v1/frame.png?w=market_trends:2/3,calendar_weather:1/3&lat=34.05&lon=-118.24&tz=America/Los_Angeles"
curl -s "http://127.0.0.1:8765/v1/frame.png?w=market_trends:1" -o /dev/null -w "%{http_code}\n"
curl -s http://127.0.0.1:8765/healthz
kill %1
```
Expected:
- `200`, then `400` (the thirds don't add up to 3), then health JSON.
- Read `.cache/smoke-frame.png` (gitignored): the markets column shows "No data yet: fred"
  (no key) and the calendar shows live LA weather.

- [ ] **Step 8: Commit**

```bash
git add server/inkboard_server/app.py server/inkboard_server/main.py server/inkboard_server/ratelimit.py server/tests
git commit -m "Server: HTTP API (frame cache, ETag/304, rate limit, health, index)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 12: Container, compose, and docs

**Files:**
- Create: `server/Dockerfile`, `server/.dockerignore`, `server/compose.yaml`,
  `server/.env.example`, `server/README.md`
- Modify: `README.md` (repo root: layout and roadmap)

**Interfaces:**
- Consumes: `inkboard_server.main:app` (Task 11)
- Produces: the image `inkboard-server`, listening on 8000 inside the container and on
  `127.0.0.1:8090` on the host (compose)

- [ ] **Step 1: Write the deployment files**

`server/Dockerfile`:
```dockerfile
FROM python:3.12-slim
COPY --from=ghcr.io/astral-sh/uv:0.11.14 /uv /bin/uv
WORKDIR /app
ENV UV_COMPILE_BYTECODE=1 UV_LINK_MODE=copy
COPY pyproject.toml uv.lock ./
RUN uv sync --frozen --no-dev --no-install-project
COPY inkboard_server ./inkboard_server
RUN uv sync --frozen --no-dev
ENV INKBOARD_CACHE_DIR=/cache
RUN mkdir -p /cache && chown nobody /cache
VOLUME /cache
EXPOSE 8000
USER nobody
CMD ["/app/.venv/bin/uvicorn", "inkboard_server.main:app", "--host", "0.0.0.0", "--port", "8000", \
     "--no-access-log", "--no-proxy-headers"]
```
If the `ghcr.io/astral-sh/uv:0.11.14` tag doesn't exist, use `ghcr.io/astral-sh/uv:0.11`.

`server/.dockerignore`:
```
.venv
.cache
tests
**/__pycache__
```

`server/compose.yaml` (the same tunnel pattern as `~/finance-ai`; see `docs/cloudflare-tunnel.md`):
```yaml
# inkboard render server at inkboard.signalwave.dev. The tunnel's public hostname points at the
# Docker service name http://inkboard:8000, which only resolves on this Compose network. Do not
# also run cloudflared for this tunnel as a host service: two connectors split the traffic.
name: inkboard

services:
  inkboard:
    build: .
    restart: unless-stopped
    environment:
      FRED_API_KEY: ${FRED_API_KEY:-}
      LOG_LEVEL: ${LOG_LEVEL:-INFO}
      # Cloudflare overwrites this header with the real client IP; the port below is
      # loopback-only, so it cannot be spoofed from outside the tunnel.
      INKBOARD_CLIENT_IP_HEADER: CF-Connecting-IP
    ports:
      - "127.0.0.1:8090:8000"   # local checks only
    volumes:
      - cache:/cache
    healthcheck:
      test: ["CMD", "python", "-c", "import urllib.request; urllib.request.urlopen('http://127.0.0.1:8000/healthz', timeout=3)"]
      interval: 30s
      timeout: 5s
      retries: 3
      start_period: 10s

  cloudflared:
    image: cloudflare/cloudflared:latest
    restart: unless-stopped
    command: tunnel --no-autoupdate run
    environment:
      TUNNEL_TOKEN: ${CLOUDFLARE_TUNNEL_TOKEN:?Set CLOUDFLARE_TUNNEL_TOKEN in .env}
    depends_on:
      inkboard:
        condition: service_healthy

volumes:
  cache:
```

The healthcheck runs `python` inside the image. `python:3.12-slim` has it on the PATH.

`server/.env.example`:
```
# Free key: https://fredaccount.stlouisfed.org/apikeys
FRED_API_KEY=
# Zero Trust -> Networks -> Tunnels -> inkboard: the token after --token in the install command.
CLOUDFLARE_TUNNEL_TOKEN=
LOG_LEVEL=INFO
```
Add a line `.env` to `server/.gitignore`.

`server/README.md`:
```markdown
# inkboard server

Renders 800×480 1-bit frames for inkboard e-paper boards. Each board sends its layout in the
query string; the server keeps no per-device state. Design: `../docs/superpowers/specs/2026-09-27-dashboard-design.md`.

## Develop

    uv sync
    uv run pytest                     # all tests, no network
    uv run pytest --update-goldens    # after an intended visual change; review the PNG diffs
    FRED_API_KEY=... uv run uvicorn inkboard_server.main:app --reload --port 8765

Preview in a browser:
http://127.0.0.1:8765/v1/frame.png?w=market_trends:2/3,calendar_weather:1/3&lat=34.05&lon=-118.24&tz=America/Los_Angeles

## Deploy

    cp .env.example .env              # set FRED_API_KEY and CLOUDFLARE_TUNNEL_TOKEN
    docker compose up -d --build
    docker compose logs -f cloudflared   # "Registered tunnel connection"

Public traffic arrives through the Cloudflare Tunnel (`cloudflared` service); the server
itself is only published on 127.0.0.1:8090 for local checks. One-time Cloudflare setup,
cache and bot settings, and checks: `../docs/cloudflare-tunnel.md`. The cache lives in the
`cache` volume.

## Endpoints

| Path | What |
|---|---|
| `/v1/frame.bin?…` | 48,000-byte packed frame (ETag / If-None-Match → 304) |
| `/v1/frame.png?…` | same frame as PNG |
| `/v1/test.bin`, `/v1/test.png` | hardware calibration pattern |
| `/healthz` | source cache status |
| `/` | widget and series reference |
```

In the repo root `README.md`:
- Add these lines to the `## Layout` code block:
  ```
  server/              render server (Python): widgets, data sources, HTTP API
  ```
- Change the roadmap line `- [ ] Dashboard layout engine (widgets on a grid)` to
  `- [x] Dashboard layout engine (server-rendered widgets, see server/)`.
- Change `- [ ] Data sources (weather, calendar, ...)` to
  `- [x] Data sources (FRED markets, Open-Meteo weather)`.

- [ ] **Step 2: Build and run the container (without touching `.env`)**

```bash
docker build -t inkboard-server:smoke .
docker run --rm -d --name inkboard-smoke -p 127.0.0.1:8091:8000 inkboard-server:smoke
sleep 4
curl -s -o /dev/null -w "%{http_code}\n" "http://127.0.0.1:8091/v1/test.png"
curl -s "http://127.0.0.1:8091/healthz"
docker stop inkboard-smoke
CLOUDFLARE_TUNNEL_TOKEN=dummy FRED_API_KEY= docker compose config -q && echo compose-ok
```
Expected: `200`, then health JSON, then `compose-ok`.
- Port 8091 can't collide with a running compose instance (8090) or ERPNext (8080).
- The smoke container runs without a FRED key or tunnel, and never reads or writes
  `server/.env`.
- `compose config -q` only validates the YAML. The dummy token is set in that one
  command's environment and nothing is started.

- [ ] **Step 3: Run the full test suite one last time**

Run: `uv run pytest -q`
Expected: all passed, 0 failed.

- [ ] **Step 4: Commit**

```bash
git add server/Dockerfile server/.dockerignore server/compose.yaml server/.env.example server/README.md server/.gitignore README.md
git commit -m "Server: Dockerfile, compose, deployment and usage docs

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

## Deferred to plan 2 (firmware)

These are all in spec §6 and §7.2–7.3:
- the thin-client firmware,
- LittleFS frame store,
- offline badge,
- config error screen,
- `extras/smoke/` move,
- host tests,
- hardware bring-up checklist.

Plan 2 will be written after this plan is merged and `/v1/test.bin` is reachable from
the board.
