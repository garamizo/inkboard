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
