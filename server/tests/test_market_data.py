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
