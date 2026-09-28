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
