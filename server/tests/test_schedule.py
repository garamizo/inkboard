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
