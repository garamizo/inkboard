"""FRED series observations (https://fred.stlouisfed.org/docs/api/fred/)."""
from datetime import date
from typing import Callable

import httpx

FRED_URL = "https://api.stlouisfed.org/fred/series/observations"
HISTORY_YEARS = 11  # covers the 10-year max window; start changes only once a year


def parse_observations(payload: dict) -> list[list]:
    out = []
    for o in payload.get("observations", []):
        value = o.get("value", ".")
        if value in (".", ""):
            continue  # FRED marks missing days (holidays) with "."
        out.append([o["date"], float(value)])
    if not out:
        raise ValueError("FRED returned no observations")
    return out


def make_fetch(client: httpx.Client, api_key: str, clock) -> Callable[[dict], list]:
    def fetch(params: dict) -> list:
        if not api_key:
            raise RuntimeError("FRED_API_KEY is not set")
        start = date(clock().year - HISTORY_YEARS, 1, 1).isoformat()
        sid = params["series_id"]
        try:
            r = client.get(FRED_URL, params={"series_id": sid, "api_key": api_key,
                                              "file_type": "json", "observation_start": start}, timeout=10)
            r.raise_for_status()
        except httpx.HTTPStatusError as exc:
            # httpx errors carry the full URL, which contains api_key: never let them reach logs.
            raise RuntimeError(f"FRED {sid}: HTTP {exc.response.status_code}") from None
        except httpx.HTTPError as exc:
            raise RuntimeError(f"FRED {sid}: {type(exc).__name__}") from None
        return parse_observations(r.json())

    return fetch
