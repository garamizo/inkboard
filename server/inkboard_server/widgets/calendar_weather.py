"""Weather-first calendar: current conditions, forecast rows, date and month grid (spec §5.2)."""
from __future__ import annotations

from dataclasses import dataclass
from datetime import date

from PIL import Image

from ..draw.calendar import month_grid
from ..draw.canvas import drawer
from ..draw.fonts import font
from ..draw.icons import icon, wmo_info
from .base import Box, ParamSpec, RenderContext, Size, Widget, WidgetData, register
from .params import coordinate, fmt_coord, one_of


@dataclass(frozen=True)
class DayForecast:
    day: date
    code: int
    hi: float
    lo: float


@dataclass(frozen=True)
class WeatherPayload:
    temp: float
    code: int
    today: DayForecast | None
    upcoming: tuple[DayForecast, ...]


def build_payload(data: dict, today: date) -> WeatherPayload:
    """Pick days by date, not position: the cache may still hold yesterday's forecast."""
    days = [DayForecast(date.fromisoformat(x["date"]), x["code"], x["hi"], x["lo"]) for x in data["daily"]]
    return WeatherPayload(
        temp=data["current"]["temp"],
        code=data["current"]["code"],
        today=next((x for x in days if x.day == today), None),
        upcoming=tuple(x for x in days if x.day > today),
    )


def deg(v: float) -> str:
    return f"{int(round(v)) + 0}°"


@register
class CalendarWeather(Widget):
    type_name = "calendar_weather"
    supported_sizes = frozenset(Size)
    params = {
        "lat": ParamSpec(coordinate(-90, 90), fmt=fmt_coord),
        "lon": ParamSpec(coordinate(-180, 180), fmt=fmt_coord),
        "units": ParamSpec(one_of("imperial", "metric"), default="imperial"),
    }

    def fetch(self, sources, ctx: RenderContext) -> WidgetData:
        o = self.options
        r = sources.weather(o["lat"], o["lon"], o["units"], ctx.tz.key)
        return WidgetData(build_payload(r.data, ctx.today), [r])

    def attributions(self) -> list[str]:
        return ["Open-Meteo"]

    def render(self, img: Image.Image, box: Box, data: WidgetData, ctx: RenderContext) -> None:
        p: WeatherPayload = data.payload
        c = Image.new("L", (box.w, box.h), 255)
        d = drawer(c)
        pad, today = 14, ctx.today
        if self.size is Size.THIRD:
            iw = box.w - 2 * pad
            y = pad
            y += self._now(d, pad, y, p) + 4
            y += self._rows(d, pad, y, iw, p.upcoming[:5]) + 8
            d.line([(pad, y), (box.w - pad, y)], fill=0)
            y += 8
            d.text((pad, y), f"{today:%a}, {today:%B} {today.day}", font=font(20, bold=True), fill=0)
            month_grid(d, pad, y + 30, iw, today)
        else:
            full = self.size is Size.FULL
            split = int(box.w * (0.36 if full else 0.45))
            lx, lw = pad, split - 2 * pad
            rx, rw = split + pad, box.w - split - 2 * pad
            d.line([(split, pad), (split, box.h - pad)], fill=0)
            y = pad + 6
            y += self._now(d, lx, y, p) + 14
            self._rows(d, lx, y, lw, p.upcoming[:7 if full else 5])
            y = pad + 6
            d.text((rx, y), f"{today:%A}", font=font(18, bold=True), fill=0)
            month = f"{today:%B}" if full else f"{today:%b}"
            d.text((rx, y + 24), f"{month} {today.day}, {today.year}", font=font(26, bold=True), fill=0)
            month_grid(d, rx, y + 70, rw, today, big=True)
        img.paste(c, (box.x, box.y))

    def _now(self, d, x, y, p: WeatherPayload) -> int:
        label, kind = wmo_info(p.code)
        r = 34
        icon(d, kind, x + r + 4, y + r + 4, r)
        tx = x + 2 * r + 18
        d.text((tx, y + 2), deg(p.temp), font=font(46, bold=True), fill=0)
        d.text((tx, y + 54), label, font=font(14), fill=0)
        hl = f"H {deg(p.today.hi)}  L {deg(p.today.lo)}" if p.today else "H —  L —"
        d.text((tx, y + 72), hl, font=font(14, bold=True), fill=0)
        return 2 * r + 20

    def _rows(self, d, x, y, w, days: tuple[DayForecast, ...]) -> int:
        rh = 30
        for k, day in enumerate(days):
            yy = y + k * rh + rh / 2
            d.text((x + 4, yy), f"{day.day:%a}", font=font(15, bold=True), fill=0, anchor="lm")
            icon(d, wmo_info(day.code)[1], x + 70, yy, 12)
            d.text((x + w - 50, yy), deg(day.hi), font=font(15, bold=True), fill=0, anchor="rm")
            d.text((x + w - 4, yy), deg(day.lo), font=font(15), fill=0, anchor="rm")
            if k:
                for xx in range(int(x + 4), int(x + w - 4), 4):
                    d.point((xx, yy - rh / 2), fill=0)
        return len(days) * rh
