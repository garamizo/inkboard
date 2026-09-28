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
