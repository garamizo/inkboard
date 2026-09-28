"""Market trends: several series on one log chart, each divided by its own window mean."""
from __future__ import annotations

import math
from dataclasses import dataclass
from datetime import date

from PIL import Image

from ..draw.canvas import drawer
from ..draw.fonts import font
from ..draw.lines import STYLES, legend_sample, line_with_markers
from ..market_data import SeriesSummary, log_ticks, summarize, y_range
from ..series import CATALOG, DEFAULT_SERIES
from .base import Box, ParamSpec, RenderContext, Size, Widget, WidgetData, register
from .params import id_list, int_in_range


@dataclass(frozen=True)
class MarketPayload:
    series: tuple[SeriesSummary, ...]
    years: int
    today: date


@register
class MarketTrends(Widget):
    type_name = "market_trends"
    supported_sizes = frozenset(Size)
    params = {
        "series": ParamSpec(id_list(CATALOG, 4), default=DEFAULT_SERIES, fmt=",".join),
        "years": ParamSpec(int_in_range(1, 10), default=5),
    }

    def fetch(self, sources, ctx: RenderContext) -> WidgetData:
        results, summaries = [], []
        for sid in self.options["series"]:
            r = sources.fred_series(CATALOG[sid].fred_id)
            obs = [(date.fromisoformat(d), v) for d, v in r.data]
            summaries.append(summarize(sid, obs, ctx.today, self.options["years"]))
            results.append(r)
        return WidgetData(MarketPayload(tuple(summaries), self.options["years"], ctx.today), results)

    def attributions(self) -> list[str]:
        out: list[str] = []
        for sid in self.options["series"]:
            for a in CATALOG[sid].attribution:
                if a not in out:
                    out.append(a)
        return out

    def render(self, img: Image.Image, box: Box, data: WidgetData, ctx: RenderContext) -> None:
        p: MarketPayload = data.payload
        c = Image.new("L", (box.w, box.h), 255)
        d = drawer(c)
        narrow = self.size is Size.THIRD
        pad = 12
        d.text((pad, pad), "Markets", font=font(18 if narrow else 22, bold=True), fill=0)
        sub = f"{p.years}-yr · ×avg · log" if narrow else f"{p.years}-yr, × own average, log"
        d.text((pad, pad + (23 if narrow else 28)), sub, font=font(12), fill=0)
        asof = min(s.last_date for s in p.series)
        d.text((box.w - pad, pad + 4), f"as of {asof:%b} {asof.day}", font=font(11), fill=0, anchor="ra")

        row_h = 20 if narrow else 22
        table_h = len(p.series) * row_h + 36
        top, bottom = pad + 50, box.h - pad - 18 - table_h
        self._chart(d, p, pad + 30, top, box.w - pad - 6, bottom, narrow)
        self._table(d, p, pad, bottom + 40, box.w - pad, row_h, narrow)
        img.paste(c, (box.x, box.y))

    def _chart(self, d, p: MarketPayload, left, top, right, bottom, narrow) -> None:
        lo, hi = y_range(p.series)
        llo, lhi = math.log(lo), math.log(hi)
        start = min(s.points[0][0] for s in p.series)
        span = max((p.today - start).days, 1)

        def X(dt: date) -> float:
            return left + (dt - start).days / span * (right - left)

        def Y(v: float) -> float:
            return bottom - (math.log(v) - llo) / (lhi - llo) * (bottom - top)

        axis = font(11)
        for g in log_ticks(lo, hi):
            y = Y(g)
            if g == 1:
                d.line([(left, y), (right, y)], fill=0, width=2)
            else:
                for x in range(int(left), int(right), 6):
                    d.point((x, y), fill=0)
            d.text((left - 4, y), f"{g:g}×", font=axis, fill=0, anchor="rm")
        d.line([(left, bottom), (right, bottom)], fill=0)
        for yr in range(start.year + 1, p.today.year + 1):
            x = X(date(yr, 1, 1))
            d.line([(x, bottom), (x, bottom + 4)], fill=0)
            d.text((x, bottom + 6), f"'{yr % 100:02d}" if narrow else str(yr), font=axis, fill=0, anchor="mt")
        n = len(p.series)
        for k, s in enumerate(p.series):
            line_with_markers(d, [(X(dt), Y(v)) for dt, v in s.points], STYLES[k], k, n)

    def _table(self, d, p: MarketPayload, x0, ty, x1, row_h, narrow) -> None:
        value_font, name_font, head_font = font(12 if narrow else 13), font(12 if narrow else 13, bold=True), font(11)
        cols = [x1 - 60, x1] if narrow else [x1 - 150, x1 - 70, x1]
        heads = ["now", "×avg"] if narrow else ["now", "×avg", "1 yr"]
        for x, h in zip(cols, heads):
            d.text((x, ty - 4), h, font=head_font, fill=0, anchor="rb")
        d.line([(x0, ty), (x1, ty)], fill=0)
        for k, s in enumerate(p.series):
            sd = CATALOG[s.id]
            y = ty + 12 + k * row_h
            legend_sample(d, x0, y, STYLES[k])
            name = sd.short if narrow else sd.label
            if s.short_history and not narrow:
                name += f" (since {s.window_start.year})"
            d.text((x0 + 32, y), name, font=name_font, fill=0, anchor="lm")
            values = [sd.fmt(s.last), f"{s.ratio:.2f}×"]
            if not narrow:
                values.append("—" if s.yoy is None else f"{s.yoy * 100:+.0f}%")
            for x, v in zip(cols, values):
                d.text((x, y), v, font=value_font, fill=0, anchor="rm")
