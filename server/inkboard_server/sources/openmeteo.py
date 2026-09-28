"""Open-Meteo forecast (https://open-meteo.com/en/docs), free tier, non-commercial."""
from typing import Callable

import httpx

URL = "https://api.open-meteo.com/v1/forecast"
FORECAST_DAYS = 8


def _round(v: float) -> float:
    return float(f"{v:.1f}") + 0.0  # + 0.0 turns -0.0 into 0.0


def weather_params(lat: float, lon: float, units: str, tz: str) -> dict:
    """Cache params: ~10 km cells, and tz because daily boundaries depend on it."""
    return {"lat": _round(lat), "lon": _round(lon), "units": units, "tz": tz}


def parse_forecast(payload: dict) -> dict:
    cur, d = payload["current"], payload["daily"]
    daily = [
        {"date": t, "code": int(c), "hi": float(h), "lo": float(lo)}
        for t, c, h, lo in zip(d["time"], d["weather_code"], d["temperature_2m_max"], d["temperature_2m_min"])
        if c is not None and h is not None and lo is not None
    ]
    return {"current": {"temp": float(cur["temperature_2m"]), "code": int(cur["weather_code"])},
            "daily": daily}


def make_fetch(client: httpx.Client) -> Callable[[dict], dict]:
    def fetch(params: dict) -> dict:
        r = client.get(URL, params={
            "latitude": params["lat"], "longitude": params["lon"],
            "current": "temperature_2m,weather_code",
            "daily": "weather_code,temperature_2m_max,temperature_2m_min",
            "temperature_unit": "fahrenheit" if params["units"] == "imperial" else "celsius",
            "timezone": params["tz"], "forecast_days": FORECAST_DAYS,
        }, timeout=10)
        r.raise_for_status()
        return parse_forecast(r.json())

    return fetch
