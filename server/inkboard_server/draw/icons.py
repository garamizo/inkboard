"""Geometric weather icons (no icon font) and the WMO code mapping (spec §4)."""
import math

from PIL import ImageDraw

from .fonts import font


def wmo_info(code: int) -> tuple[str, str]:
    """(label, icon kind) for an Open-Meteo WMO weather code."""
    if code == 0:
        return "Clear", "sun"
    if code == 1:
        return "Mostly clear", "sun"
    if code == 2:
        return "Partly cloudy", "part"
    if code == 3:
        return "Overcast", "cloud"
    if code in (45, 48):
        return "Fog", "fog"
    if 51 <= code <= 67 or 80 <= code <= 82:
        return "Rain", "rain"
    if 71 <= code <= 77 or code in (85, 86):
        return "Snow", "snow"
    if 95 <= code <= 99:
        return "Storms", "storm"
    return "—", "cloud"


def _sun(d, cx, cy, r):
    w = max(2, int(r * 0.12))
    d.ellipse([cx - r * .45, cy - r * .45, cx + r * .45, cy + r * .45], outline=0, width=w)
    for k in range(8):
        a = k * math.pi / 4
        d.line([(cx + math.cos(a) * r * .65, cy + math.sin(a) * r * .65),
                (cx + math.cos(a) * r * .95, cy + math.sin(a) * r * .95)], fill=0, width=max(2, int(r * .1)))


def _cloud(d, cx, cy, r):
    w = max(2, int(r * .1))
    parts = [(cx - r * .45, cy + r * .1, r * .38), (cx, cy - r * .15, r * .5), (cx + r * .45, cy + r * .12, r * .36)]
    for x, y, rr in parts:
        d.ellipse([x - rr, y - rr, x + rr, y + rr], outline=0, width=w)
    for x, y, rr in parts:
        d.ellipse([x - rr + w, y - rr + w, x + rr - w, y + rr - w], fill=255)
    d.rectangle([cx - r * .45, cy + r * .1, cx + r * .45, cy + r * .48 - w], fill=255)
    d.line([(cx - r * .45, cy + r * .48), (cx + r * .45, cy + r * .48)], fill=0, width=w)


def icon(d: ImageDraw.ImageDraw, kind: str, cx: float, cy: float, r: float) -> None:
    if kind == "sun":
        _sun(d, cx, cy, r)
    elif kind == "part":
        _sun(d, cx - r * .3, cy - r * .3, r * .7)
        _cloud(d, cx + r * .1, cy + r * .15, r * .8)
    elif kind == "cloud":
        _cloud(d, cx, cy, r)
    elif kind in ("rain", "snow", "storm"):
        _cloud(d, cx, cy - r * .2, r * .85)
        for k in range(3):
            x = cx - r * .35 + k * r * .35
            if kind == "snow":
                d.text((x, cy + r * .55), "*", font=font(max(8, int(r * .5)), bold=True), fill=0, anchor="mm")
            elif kind == "storm" and k == 1:
                d.line([(x + r * .1, cy + r * .3), (x - r * .1, cy + r * .55), (x + r * .1, cy + r * .55),
                        (x - r * .1, cy + r * .85)], fill=0, width=max(2, int(r * .1)))
            else:
                d.line([(x, cy + r * .35), (x - r * .12, cy + r * .75)], fill=0, width=max(2, int(r * .1)))
    elif kind == "fog":
        for k in range(4):
            y = cy - r * .4 + k * r * .27
            d.line([(cx - r * .7, y), (cx + r * .7, y)], fill=0, width=max(2, int(r * .1)))
    else:
        raise ValueError(f"unknown icon {kind!r}")
