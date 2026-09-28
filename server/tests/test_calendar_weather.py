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
