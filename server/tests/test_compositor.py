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
