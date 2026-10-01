# /// script
# requires-python = ">=3.11"
# dependencies = ["httpx"]
# ///
"""Record raw API responses for the native tests (plan Task 9, spec §3.4).

FRED key: $FRED_API_KEY, else read from include/secrets.h. The key is never printed, and
HTTP errors are reported without their URL (it contains the key).
Also writes test/fixtures/summaries.h: expected market summaries computed with a copy of the
server's market_data.summarize(), so the C++ pipeline is checked against the Python one.
Run: uv run tools/record_fixtures.py
"""
import os
import re
from datetime import date, datetime, time, timedelta
from pathlib import Path
from zoneinfo import ZoneInfo

import httpx

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "test" / "fixtures"
FRED_URL = "https://api.stlouisfed.org/fred/series/observations"
FRED_IDS = ["SP500", "CBBTCUSD", "MORTGAGE30US", "MEDLISPRI31080", "DGS10", "DTWEXBGS"]
LA = ZoneInfo("America/Los_Angeles")
EPOCH = date(1970, 1, 1)


def fred_key() -> str:
    if key := os.environ.get("FRED_API_KEY", "").strip():
        return key
    m = re.search(r'#define\s+FRED_API_KEY\s+"([^"]+)"', (ROOT / "include" / "secrets.h").read_text())
    if not m or m.group(1) == "your-fred-api-key":
        raise SystemExit("set FRED_API_KEY or put it in include/secrets.h")
    return m.group(1)


def get(client: httpx.Client, url: str, params: dict, expect: int = 200) -> bytes:
    r = client.get(url, params=params)
    if r.status_code != expect:
        raise SystemExit(f"{params.get('series_id', url)}: HTTP {r.status_code}")  # no URL: it has the key
    return r.content


def window_start(today: date, years: int) -> date:
    return today - timedelta(days=round(365.25 * years))


# --- copy of server/inkboard_server/market_data.py (summarize and helpers) ---
def weekly_grid(today: date, years: int) -> list[date]:
    start = window_start(today, years)
    d = start + timedelta(days=(6 - start.weekday()) % 7)
    grid = []
    while d <= today:
        grid.append(d)
        d += timedelta(days=7)
    if not grid or grid[-1] != today:
        grid.append(today)
    return grid


def summarize(obs: list[tuple[date, float]], today: date, years: int) -> dict:
    obs = sorted((d, v) for d, v in obs if d <= today and v > 0)
    grid = weekly_grid(today, years)
    pts, i, last = [], 0, None
    for g in grid:
        while i < len(obs) and obs[i][0] <= g:
            last = obs[i][1]
            i += 1
        if last is not None:
            pts.append((g, last))
    mean = sum(v for _, v in pts) / len(pts)
    lastv = pts[-1][1]
    prior = [v for d, v in pts if d <= today - timedelta(weeks=52)]
    return {"n": len(pts), "last": lastv, "ratio": lastv / mean, "yoy": (lastv / prior[-1] - 1) if prior else 0.0,
            "has_yoy": bool(prior), "last_date": (obs[-1][0] - EPOCH).days, "window_start": (pts[0][0] - EPOCH).days,
            "short": pts[0][0] > grid[0] + timedelta(days=31), "first_norm": pts[0][1] / mean,
            "last_norm": lastv / mean}
# --- end of copy ---


def main() -> None:
    import json
    key = fred_key()
    today = datetime.now(LA).date()
    d = today - timedelta(days=120)
    s0 = d - timedelta(days=(d.weekday() + 1) % 7)  # the Sunday on or before today - 120
    full_start = window_start(today, 10) - timedelta(days=62)
    OUT.mkdir(parents=True, exist_ok=True)
    rows = []
    with httpx.Client(timeout=60) as c:
        for sid in FRED_IDS:
            base = {"series_id": sid, "api_key": key, "file_type": "json"}
            full = get(c, FRED_URL, base | {"observation_start": full_start.isoformat()})
            (OUT / f"fred_{sid}_full.json").write_bytes(full)
            (OUT / f"fred_{sid}_tail.json").write_bytes(
                get(c, FRED_URL, base | {"observation_start": (s0 + timedelta(days=1)).isoformat()}))
            obs = [(date.fromisoformat(o["date"]), float(o["value"])) for o in json.loads(full)["observations"]
                   if o["value"] not in (".", "")]
            for years in (1, 5, 10):
                s = summarize(obs, today, years)
                rows.append('    {"%s", %d, %d, %r, %r, %r, %s, %d, %d, %s, %r, %r},' % (
                    sid, years, s["n"], s["last"], s["ratio"], s["yoy"], "true" if s["has_yoy"] else "false",
                    s["last_date"], s["window_start"], "true" if s["short"] else "false", s["first_norm"], s["last_norm"]))
        (OUT / "fred_error_bad_key.json").write_bytes(
            get(c, FRED_URL, {"series_id": "SP500", "api_key": "0" * 32, "file_type": "json"}, expect=400))
        (OUT / "weather_la.json").write_bytes(get(c, "https://api.open-meteo.com/v1/forecast", {
            "latitude": 34.1, "longitude": -118.2, "current": "temperature_2m,weather_code",
            "daily": "weather_code,temperature_2m_max,temperature_2m_min", "temperature_unit": "fahrenheit",
            "timezone": "America/Los_Angeles", "forecast_days": 8}))
    now = datetime.combine(today, time(10, 0), LA)
    (OUT / "fixtures.h").write_text(
        "#pragma once\n// Generated by tools/record_fixtures.py; do not edit.\n"
        f"#define FIXTURE_TODAY {(today - EPOCH).days}  // {today}\n"
        f"#define FIXTURE_NOW {int(now.timestamp())}LL  // {now.isoformat()}\n"
        f"#define FIXTURE_TAIL_S0 {(s0 - EPOCH).days}  // {s0}\n"
        f"#define FIXTURE_FULL_START {(full_start - EPOCH).days}  // {full_start}\n")
    (OUT / "summaries.h").write_text(
        "#pragma once\n// Generated by tools/record_fixtures.py (Python summarize()); do not edit.\n#include <stdint.h>\n"
        "struct SummaryExpect { const char* fred_id; int years; int n; double last, ratio, yoy; bool has_yoy;\n"
        "                       int32_t last_date, window_start; bool short_history; double first_norm, last_norm; };\n"
        "static const SummaryExpect SUMMARIES[] = {\n" + "\n".join(rows) + "\n};\n")
    print(f"recorded fixtures for {today}")


if __name__ == "__main__":
    main()
