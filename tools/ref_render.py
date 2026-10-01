"""Reference renders of every golden scenario by the Python server, from test/fixtures
(plan Task 13). test/test_widgets compares the C++ output with these.

Needs the server package (removed in Task 20; the PBMs stay committed). To rerun after that,
check out tag server-render-final into a scratch worktree.
Run from the repo root:  (cd server && uv run python ../tools/ref_render.py)
"""
import json
import re
import sys
from dataclasses import replace
from datetime import date, datetime, timedelta, timezone
from pathlib import Path
from zoneinfo import ZoneInfo

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "server"))

from PIL import Image  # noqa: E402

from inkboard_server.compositor import FetchFailure, render_frame  # noqa: E402
from inkboard_server.frame import THRESHOLD, calibration_pattern  # noqa: E402
from inkboard_server.market_data import summarize  # noqa: E402
from inkboard_server.series import CATALOG, DEFAULT_SERIES  # noqa: E402
from inkboard_server.sources import openmeteo  # noqa: E402
from inkboard_server.sources.base import SourceResult  # noqa: E402
from inkboard_server.widgets.base import WIDGET_H, Box, RenderContext, Size, WidgetData  # noqa: E402
from inkboard_server.widgets.calendar_weather import CalendarWeather, build_payload  # noqa: E402
from inkboard_server.widgets.market_trends import MarketPayload, MarketTrends  # noqa: E402

FIX = ROOT / "test" / "fixtures"
OUT = ROOT / "test" / "reference" / "widgets"
LA = ZoneInfo("America/Los_Angeles")
EPOCH = date(1970, 1, 1)
SIZES = [(Size.THIRD, "third"), (Size.TWO_THIRDS, "two_thirds"), (Size.FULL, "full")]
WEATHER_OPTS = {"lat": 34.1, "lon": -118.2, "units": "imperial"}
VERSION = "fw 2.0.0"


def fixture(name: str) -> int:
    return int(re.search(rf"#define {name} (\d+)", (FIX / "fixtures.h").read_text()).group(1))


TODAY = EPOCH + timedelta(days=fixture("FIXTURE_TODAY"))
NOW = datetime.fromtimestamp(fixture("FIXTURE_NOW"), timezone.utc)
CTX = RenderContext(TODAY, LA)


def save(name: str, img: Image.Image) -> None:
    img.save(OUT / f"{name}.pbm")


def widget_image(w, data) -> Image.Image:
    img = Image.new("L", (w.size.width, WIDGET_H), 255)
    w.render(img, Box(0, 0, w.size.width, WIDGET_H), data, CTX)
    return img.point(lambda p: 255 if p > THRESHOLD else 0).convert("1")


def weather_data(stale=False) -> WidgetData:
    raw = openmeteo.parse_forecast(json.loads((FIX / "weather_la.json").read_text()))
    return WidgetData(build_payload(raw, TODAY), [SourceResult("weather", raw, NOW, stale)])


def fred_obs(fred_id: str, since: date | None = None) -> list[tuple[date, float]]:
    obs = json.loads((FIX / f"fred_{fred_id}_full.json").read_text())["observations"]
    out = [(date.fromisoformat(o["date"]), float(o["value"])) for o in obs if o["value"] not in (".", "")]
    return [x for x in out if since is None or x[0] >= since]


def market_data(series, years=5, stale=False, override=None) -> WidgetData:
    sums, srcs = [], []
    for sid in series:
        obs = (override or {}).get(sid) or fred_obs(CATALOG[sid].fred_id)
        sums.append(summarize(sid, obs, TODAY, years))
        srcs.append(SourceResult(f"fred:{sid}", obs, NOW, stale))
    return WidgetData(MarketPayload(tuple(sums), years, TODAY), srcs)


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    for size, name in SIZES:
        save(f"calendar_weather_{name}", widget_image(CalendarWeather(size, WEATHER_OPTS), weather_data()))
        save(f"market_trends_{name}", widget_image(MarketTrends(size, {"series": DEFAULT_SERIES, "years": 5}),
                                                  market_data(DEFAULT_SERIES)))
    short = market_data(("sp500", "home_la"), override={"home_la": fred_obs("MEDLISPRI31080", date(2024, 1, 1))})
    save("market_trends_short_series",
         widget_image(MarketTrends(Size.TWO_THIRDS, {"series": ("sp500", "home_la"), "years": 5}), short))
    ws = [MarketTrends(Size.TWO_THIRDS, {"series": DEFAULT_SERIES, "years": 5}),
          CalendarWeather(Size.THIRD, WEATHER_OPTS)]
    save("screen_default", render_frame(ws, [market_data(DEFAULT_SERIES), weather_data()], CTX, VERSION))
    save("screen_stale", render_frame(ws, [market_data(DEFAULT_SERIES, stale=True), weather_data(stale=True)],
                                      CTX, VERSION))
    save("screen_nodata", render_frame(ws, [FetchFailure("fred"), FetchFailure("weather")], CTX, VERSION))

    class Exploding(CalendarWeather):
        def render(self, img, box, data, ctx):
            raise RuntimeError("boom")

    save("screen_render_error", render_frame([ws[0], Exploding(Size.THIRD, WEATHER_OPTS)],
                                             [market_data(DEFAULT_SERIES), weather_data()], CTX, VERSION))
    save("calibration", calibration_pattern())
    print("wrote", sorted(p.name for p in OUT.glob("*.pbm")))


if __name__ == "__main__":
    main()
