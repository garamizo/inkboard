"""Columns, dividers, footer and per-widget error isolation (spec §3.3)."""
from __future__ import annotations

import logging
from dataclasses import dataclass
from zoneinfo import ZoneInfo

from PIL import Image, ImageDraw

from .draw.canvas import drawer
from .draw.fonts import font
from .frame import WIDTH, new_canvas, to_1bit
from .sources.base import NoData
from .widgets.base import WIDGET_H, Box, RenderContext, Widget, WidgetData

log = logging.getLogger(__name__)
FOOTER_H = 16


@dataclass(frozen=True)
class FetchFailure:
    source: str


def fetch_widgets(widgets: list[Widget], sources, ctx: RenderContext) -> list[WidgetData | FetchFailure]:
    out: list[WidgetData | FetchFailure] = []
    for w in widgets:
        try:
            out.append(w.fetch(sources, ctx))
        except NoData as e:
            out.append(FetchFailure(e.source))
        except Exception:
            log.exception("fetch failed in widget %s", w.type_name)
            out.append(FetchFailure(w.type_name))
    return out


def frame_versions(results) -> tuple:
    """Everything a render can see from sources; part of the frame-cache key."""
    out = []
    for r in results:
        if isinstance(r, FetchFailure):
            out.append(("nodata", r.source))
        else:
            out.extend(s.version for s in r.sources)
    return tuple(out)


def footer_time_text(results, tz: ZoneInfo) -> str:
    fetched = [s.fetched_at for r in results if isinstance(r, WidgetData) for s in r.sources]
    if not fetched:
        return "updated —"
    t = max(fetched).astimezone(tz)
    return f"updated {t.hour % 12 or 12}:{t.minute:02d} {'AM' if t.hour < 12 else 'PM'}"


def _message(img: Image.Image, box: Box, text: str) -> None:
    d = drawer(img)
    d.text((box.x + box.w / 2, box.y + box.h / 2), text, font=font(14, bold=True), fill=0, anchor="mm")


def compose(widgets: list[Widget], results, ctx: RenderContext, version: str = "") -> Image.Image:
    img = new_canvas()
    x, dividers = 0, []
    for w, r in zip(widgets, results):
        box = Box(x, 0, w.size.width, WIDGET_H)
        if isinstance(r, FetchFailure):
            _message(img, box, f"No data yet: {r.source}")
        else:
            try:
                w.render(img, box, r, ctx)
            except Exception:
                log.exception("render failed in widget %s", w.type_name)
                img.paste(255, (box.x, box.y, box.x + box.w, box.y + box.h))
                _message(img, box, f"error: {w.type_name}")
        if x:
            dividers.append(x)
        x += box.w
    d = drawer(img)
    for dx in dividers:  # after rendering: widgets paste over their whole box
        d.line([(dx, 8), (dx, WIDGET_H - 8)], fill=0)
    _footer(d, widgets, results, ctx, version)
    return img


def _footer(d: ImageDraw.ImageDraw, widgets, results, ctx: RenderContext, version: str) -> None:
    d.line([(0, WIDGET_H), (WIDTH, WIDGET_H)], fill=0)
    left = footer_time_text(results, ctx.tz)
    if any(isinstance(r, WidgetData) and r.stale for r in results):
        left += "   ⚠ stale"
    if version:
        left += f"   {version}"
    y = WIDGET_H + FOOTER_H // 2
    d.text((8, y), left, font=font(10), fill=0, anchor="lm")
    attrs: list[str] = []
    for w in widgets:
        for a in w.attributions():
            if a not in attrs:
                attrs.append(a)
    d.text((WIDTH - 8, y), " · ".join(attrs), font=font(10), fill=0, anchor="rm")


def render_frame(widgets: list[Widget], results, ctx: RenderContext, version: str = "") -> Image.Image:
    return to_1bit(compose(widgets, results, ctx, version))
