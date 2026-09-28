from datetime import date

import pytest
from PIL import Image, ImageDraw

from inkboard_server.draw.calendar import month_grid, month_weeks
from inkboard_server.draw.icons import icon, wmo_info
from inkboard_server.draw.lines import (
    STYLES, LineStyle, draw_marker, legend_sample, line_with_markers, marker_xs, styled_line, y_at,
)


def canvas(w=200, h=100):
    img = Image.new("L", (w, h), 255)
    return img, ImageDraw.Draw(img)


def black(img):
    return sum(img.histogram()[:128])


def test_marker_xs_stagger():
    assert marker_xs(0, 200, 0, 4) == [7.0, 63.0, 119.0, 175.0]
    sets = [set(marker_xs(0, 400, k, 4)) for k in range(4)]
    for i in range(4):
        for j in range(i + 1, 4):
            assert not sets[i] & sets[j]


def test_y_at_interpolates():
    pts = [(0, 10), (10, 20), (20, 0)]
    assert y_at(pts, 5) == 15
    assert y_at(pts, 15) == 10
    assert y_at(pts, -5) == 10 and y_at(pts, 50) == 0


def test_solid_line_width():
    img, d = canvas()
    styled_line(d, [(10, 50), (190, 50)], LineStyle(3, None, "square"))
    rows = {y for y in range(100) if img.getpixel((100, y)) < 128}
    assert len(rows) == 3


def test_dashed_line_has_gaps():
    img, d = canvas()
    styled_line(d, [(0, 50), (100, 50)], LineStyle(2, (7, 4), "circle"))
    row = [img.getpixel((x, 50)) < 128 for x in range(100)]
    assert 50 < sum(row) < 90


@pytest.mark.parametrize("kind", ["square", "circle", "triangle", "diamond"])
def test_markers_draw_near_point(kind):
    img, d = canvas()
    draw_marker(d, 100, 50, kind)
    assert black(img) > 0
    assert black(img.crop((92, 42, 109, 59))) == black(img)


def test_line_with_markers_and_legend_draw():
    img, d = canvas(400, 100)
    line_with_markers(d, [(0, 50), (399, 50)], STYLES[1], 1, 4)
    legend_sample(d, 10, 80, STYLES[3])
    assert black(img) > 100


@pytest.mark.parametrize("code,expected", [
    (0, ("Clear", "sun")), (1, ("Mostly clear", "sun")), (2, ("Partly cloudy", "part")),
    (3, ("Overcast", "cloud")), (45, ("Fog", "fog")), (48, ("Fog", "fog")),
    (51, ("Rain", "rain")), (67, ("Rain", "rain")), (80, ("Rain", "rain")), (82, ("Rain", "rain")),
    (71, ("Snow", "snow")), (77, ("Snow", "snow")), (85, ("Snow", "snow")), (86, ("Snow", "snow")),
    (95, ("Storms", "storm")), (99, ("Storms", "storm")), (4, ("—", "cloud")), (68, ("—", "cloud")),
])
def test_wmo_info(code, expected):
    assert wmo_info(code) == expected


@pytest.mark.parametrize("kind", ["sun", "part", "cloud", "fog", "rain", "snow", "storm"])
def test_icons_stay_in_bounds(kind):
    img, d = canvas(200, 200)
    icon(d, kind, 100, 100, 30)
    assert black(img) > 30
    r = int(30 * 1.1) + 2
    assert black(img.crop((100 - r, 100 - r, 100 + r + 1, 100 + r + 1))) == black(img)


def test_month_weeks_start_sunday():
    assert month_weeks(2026, 2)[0] == [1, 2, 3, 4, 5, 6, 7]         # Feb 2026 starts on Sunday
    aug = month_weeks(2026, 8)                                      # Aug 2026 starts on Saturday
    assert aug[0] == [0, 0, 0, 0, 0, 0, 1] and len(aug) == 6


def test_month_grid_highlights_today():
    img, d = canvas(266, 200)
    h = month_grid(d, 0, 0, 266, date(2026, 9, 27))
    assert 120 < h < 200
    # Sep 27, 2026 is a Sunday in the 5th week row: column 0, row index 5 (after header)
    cw, ch = 266 / 7, 22
    cx, cy = int(cw / 2), int(ch * 5 + ch / 2 + 2)
    assert img.getpixel((cx - 12, cy)) < 128  # inverted cell background is black
