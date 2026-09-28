"""Weekly resampling and normalization for the market chart (spec §5.1 data pipeline)."""
from __future__ import annotations

from dataclasses import dataclass
from datetime import date, timedelta
from typing import Sequence

TICKS = (0.25, 0.5, 1, 1.5, 2, 3)


@dataclass(frozen=True)
class SeriesSummary:
    id: str
    points: tuple[tuple[date, float], ...]  # normalized: value / window mean
    last: float                             # latest raw value
    ratio: float                            # last / window mean
    yoy: float | None                       # last vs the weekly point 52 weeks earlier
    last_date: date                         # date of the latest observation used
    window_start: date
    short_history: bool


def weekly_grid(today: date, years: int) -> list[date]:
    """Sundays in the last `years` years, plus today if today is not a Sunday."""
    start = today - timedelta(days=round(365.25 * years))
    d = start + timedelta(days=(6 - start.weekday()) % 7)
    grid = []
    while d <= today:
        grid.append(d)
        d += timedelta(days=7)
    if not grid or grid[-1] != today:
        grid.append(today)
    return grid


def resample(obs: list[tuple[date, float]], grid: list[date]) -> list[tuple[date, float]]:
    """Last observation on or before each grid date; grid dates before the first obs are dropped."""
    out, i, last = [], 0, None
    for g in grid:
        while i < len(obs) and obs[i][0] <= g:
            last = obs[i][1]
            i += 1
        if last is not None:
            out.append((g, last))
    return out


def summarize(series_id: str, obs: list[tuple[date, float]], today: date, years: int) -> SeriesSummary:
    obs = sorted((d, v) for d, v in obs if d <= today and v > 0)
    grid = weekly_grid(today, years)
    pts = resample(obs, grid)
    if not pts:
        raise ValueError(f"{series_id}: no observations in the window")
    mean = sum(v for _, v in pts) / len(pts)
    last = pts[-1][1]
    year_ago = today - timedelta(weeks=52)
    prior = [v for d, v in pts if d <= year_ago]
    return SeriesSummary(
        id=series_id,
        points=tuple((d, v / mean) for d, v in pts),
        last=last,
        ratio=last / mean,
        yoy=(last / prior[-1] - 1) if prior else None,
        last_date=obs[-1][0],
        window_start=pts[0][0],
        short_history=pts[0][0] > grid[0] + timedelta(days=31),
    )


def y_range(series: Sequence[SeriesSummary]) -> tuple[float, float]:
    values = [v for s in series for _, v in s.points]
    return min(values) * 0.92, max(values) * 1.08


def log_ticks(lo: float, hi: float) -> list[float]:
    return [g for g in TICKS if lo <= g <= hi]
