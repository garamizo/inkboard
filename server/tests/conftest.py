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


from datetime import datetime, timedelta, timezone


class FakeClock:
    def __init__(self, t: datetime):
        self.t = t

    def __call__(self) -> datetime:
        return self.t

    def advance(self, **kw) -> None:
        self.t += timedelta(**kw)


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
