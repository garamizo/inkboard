"""Chart line styles for 1-bit output: width + dash pattern + marker shape (spec §5.1)."""
import math
from dataclasses import dataclass

from PIL import ImageDraw


@dataclass(frozen=True)
class LineStyle:
    width: int
    dash: tuple[int, int] | None  # (on, off) px; None = solid
    marker: str                   # square | circle | triangle | diamond


STYLES = (
    LineStyle(3, None, "square"),
    LineStyle(2, (7, 4), "circle"),
    LineStyle(1, None, "triangle"),
    LineStyle(2, (2, 3), "diamond"),
)
MARKER_STEP = 56


def styled_line(d: ImageDraw.ImageDraw, pts, style: LineStyle) -> None:
    if len(pts) < 2:
        return
    if style.dash is None:
        d.line(pts, fill=0, width=style.width, joint="curve" if style.width > 2 else None)
        return
    on, off = style.dash
    acc, drawing = 0.0, True
    for (x0, y0), (x1, y1) in zip(pts, pts[1:]):
        seg = math.hypot(x1 - x0, y1 - y0)
        t = 0.0
        while t < seg:
            limit = on if drawing else off
            step = min(limit - acc, seg - t)
            if drawing:
                a, b = t / seg, (t + step) / seg
                d.line([(x0 + (x1 - x0) * a, y0 + (y1 - y0) * a),
                        (x0 + (x1 - x0) * b, y0 + (y1 - y0) * b)], fill=0, width=style.width)
            t += step
            acc += step
            if acc >= limit:
                acc, drawing = 0.0, not drawing


def draw_marker(d: ImageDraw.ImageDraw, x: float, y: float, kind: str, r: int = 4) -> None:
    if kind == "square":
        d.rectangle([x - r, y - r, x + r, y + r], fill=0)
    elif kind == "circle":
        d.ellipse([x - r - 1, y - r - 1, x + r + 1, y + r + 1], fill=255, outline=0, width=2)
    elif kind == "triangle":
        d.polygon([(x, y - r - 1), (x + r + 1, y + r), (x - r - 1, y + r)], fill=0)
    elif kind == "diamond":
        d.polygon([(x, y - r - 2), (x + r + 2, y), (x, y + r + 2), (x - r - 2, y)], fill=255, outline=0, width=2)
    else:
        raise ValueError(f"unknown marker {kind!r}")


def marker_xs(x0: float, x1: float, k: int, n: int, step: float = MARKER_STEP) -> list[float]:
    """Marker positions for series k of n: every `step` px, offset so series don't stack."""
    xs, x = [], x0 + step * (k + 0.5) / n
    while x < x1 - 4:
        xs.append(x)
        x += step
    return xs


def y_at(pts, x: float) -> float:
    """y of the polyline at x (pts sorted by x); clamps outside the range."""
    if x <= pts[0][0]:
        return pts[0][1]
    for (xa, ya), (xb, yb) in zip(pts, pts[1:]):
        if xa <= x <= xb:
            return ya if xb == xa else ya + (yb - ya) * (x - xa) / (xb - xa)
    return pts[-1][1]


def line_with_markers(d: ImageDraw.ImageDraw, pts, style: LineStyle, k: int, n: int) -> None:
    styled_line(d, pts, style)
    if not pts:
        return
    for x in marker_xs(pts[0][0], pts[-1][0], k, n):
        draw_marker(d, x, y_at(pts, x), style.marker)
    draw_marker(d, pts[-1][0], pts[-1][1], style.marker)


def legend_sample(d: ImageDraw.ImageDraw, x: float, y: float, style: LineStyle, length: int = 24) -> None:
    styled_line(d, [(x, y), (x + length, y)], style)
    draw_marker(d, x + length / 2, y, style.marker)
