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
