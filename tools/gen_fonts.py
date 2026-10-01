# /// script
# requires-python = ">=3.11"
# dependencies = ["pillow==12.3.0"]
# ///
"""Bitmap fonts for include/render/fonts/ from DejaVu (plan Task 5, spec §3.1).

Everything comes from Pillow with the same settings the server used (fontmode "1", default
layout engine = raqm), so the C++ text matches the server's. Also writes reference images
for test/test_render (text_cases.h + test/reference/text/*.pbm).
Run: uv run tools/gen_fonts.py

Pillow sizes a text's mask from FreeType's outline boxes (rounded outward) but places the
glyphs by their mono bitmaps (rounded to nearest, possibly with blank rows or columns, and
1x1 for an empty glyph such as space). The two differ by a pixel often enough to matter, so
each glyph records its ink box, its outline box and its bitmap origin; measure() reads them
off Pillow's own masks and checks the C++ placement rules (Model) against Pillow.
"""
import math
import struct
from itertools import product
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont, features

ROOT = Path(__file__).resolve().parent.parent
TTF = ROOT / "tools" / "fonts"
OUT = ROOT / "include" / "render" / "fonts"
REF = ROOT / "test" / "reference" / "text"
CASES_H = ROOT / "test" / "test_render" / "text_cases.h"

REGULAR = [10, 11, 12, 13, 14, 15, 18]
BOLD = [8, 11, 12, 13, 14, 15, 17, 18, 20, 22, 26, 36, 46]
CHARS = [chr(c) for c in range(0x20, 0x7F)] + ["°", "×", "·", "—", "⚠"]

TEXT_CASES = [  # (size, bold, text, anchor) - every anchor and the special characters the widgets use
    (46, True, "73°", "la"), (14, False, "Mostly clear", "la"), (14, True, "H 89°  L 67°", "la"),
    (15, True, "Wed", "lm"), (15, False, "62°", "rm"), (13, False, "27", "mm"), (13, True, "27", "mm"),
    (11, False, "1.5×", "rm"), (11, False, "2024", "mt"), (11, False, "now", "rb"),
    (11, False, "as of Aug 1", "ra"), (10, False, "updated 10:00 AM   ⚠ stale   fw 2.0.0", "lm"),
    (10, False, "FRED · S&P DJI · Coinbase", "rm"), (22, True, "Markets", "la"),
    (12, False, "5-yr, × own average, log", "la"), (13, True, "S&P 500 (since 2021)", "lm"),
    (13, False, "$84.6k", "rm"), (17, True, "*", "mm"), (8, True, "*", "mm"), (14, True, "No data yet: fred", "mm"),
    (20, True, "Sun, September 27", "la"), (26, True, "Sep 27, 2026", "la"), (12, False, "H —  L —", "la"),
    (36, True, "inkboard calibration", "mm"), (18, False, "AV Ty To WA", "la"),
]
REF_W, REF_H, REF_X, REF_Y = 520, 90, 260, 45
FRACTIONAL = [  # (size, bold, text, anchor, x, y): widgets place text at float positions
    (13, False, "27", "mm", 260.5, 45.5), (15, True, "Thu", "lm", 264.0, 52.5),
    (11, False, "1.5×", "rm", 259.75, 44.6), (11, False, "2024", "mt", 301.3, 40.0),
    (15, False, "81°", "rm", 246.0, 52.5), (12, True, "S", "mm", 256.92857142857144, 56.0),
]  # No fi/fl/ff: raqm applies DejaVu's ligatures, which the bitmap fonts leave out (no widget text has them).
EDGES = [  # (size, bold, text, anchor, x, y): where the outline box and the bitmap disagree
    (13, False, "~oc", "la", 260.0, 45.0),  # alone, ~ would sit a row higher (outline top > bitmap top)
    (10, False, "~ _", "lm", 260.0, 45.0),  # the space's 1x1 bitmap pushes '_' out of the mask
    (11, False, "jy J_", "la", -1.7, -0.6),  # negative start: Pillow clips the mask's first row and column
    (8, True, "x |y", "mb", 260.25, 45.75), (36, True, "8db~d", "la", 260.25, 20.75),
]


def ttf(bold: bool) -> Path:
    return TTF / ("DejaVuSans-Bold.ttf" if bold else "DejaVuSans.ttf")


def ident(size: int, bold: bool) -> str:
    return f"dejavu_sans{'_bold' if bold else ''}_{size}"


def PIXEL(x: int) -> int:  # _imagingft.c: 26.6 to whole pixels, rounding half up
    return ((x + 32) & -64) >> 6


def f32(v: float) -> float:  # Pillow passes the start offset as a C float
    return struct.unpack("f", struct.pack("f", v))[0]


def c_round(v: float) -> int:
    return math.floor(v + 0.5) if v >= 0 else -math.floor(-v + 0.5)


def length64(f: ImageFont.FreeTypeFont, s: str) -> int:
    return round(f.getlength(s, mode="1") * 64)


def pil_mask(f: ImageFont.FreeTypeFont, s: str, start=(0.0, 0.0), anchor="ls"):
    """Pillow's text mask: (offset, height, inked (x, y) set). The width is left out: it only
    clips on the right, which outline-vs-bitmap rounding never reaches."""
    m, off = f.getmask2(s, "1", anchor=anchor, start=start)
    w, h = m.size
    data = bytes(m)
    return tuple(off), h, {(i % w, i // w) for i, v in enumerate(data) if v}


def ink_box(ink):
    if not ink:
        return None
    xs = [x for x, _ in ink]
    ys = [y for _, y in ink]
    return min(xs), min(ys), max(xs) + 1, max(ys) + 1


class Model:
    """Python twin of draw_text in include/render/text.h, used to check the measured metrics."""

    def __init__(self, f: ImageFont.FreeTypeFont):
        self.f, self.g, self.k = f, {}, {}
        self.ascent, self.descent = f.getmetrics()

    def mask(self, text: str, start=(0.0, 0.0), anchor="ls"):
        pos, prev, pens = 0, 0, []
        for ch in text:
            cp = ord(ch)
            if prev:
                pos += self.k.get((prev, cp), 0)
            pens.append((pos, self.g[cp]))
            pos += self.g[cp]["adv"]
            prev = cp
        xb = xr = yt = yb = yr = 0
        for p, g in pens:
            xb = min(xb, g["box_l"] + PIXEL(p))
            xr = min(xr, g["bmp_l"] + PIXEL(p))
            yt, yb, yr = min(yt, g["box_t"]), max(yb, g["box_b"]), min(yr, g["bmp_t"])
        xa = {"l": 0, "m": PIXEL(pos // 2), "r": PIXEL(pos)}[anchor[0]]
        ya = {"a": self.ascent, "s": 0, "d": -self.descent, "t": -yt, "b": -yb,
              "m": PIXEL(int((self.ascent - self.descent) * 64 / 2))}[anchor[1]]
        fx, fy = f32(start[0]), f32(start[1])
        h = -yt + yb + math.ceil(fy)
        x64 = c_round(f32(f32(-xr + fx) * 64))
        py = PIXEL(c_round(f32(f32(yr - fy) * 64)))
        ink = set()
        for p, g in pens:
            gx, gy = PIXEL(x64 + p) + g["dx"], -py + g["dy"]
            for x, y in g["ink"]:
                if gx + x >= 0 and 0 <= gy + y < h:
                    ink.add((gx + x, gy + y))
        return (-xa + xb, ya + yt), h, ink


def measure(f: ImageFont.FreeTypeFont) -> Model:
    m = Model(f)
    # '_' lies below the baseline, so alone it shows its true depth; 'H' has a positive left
    # bearing, so alone it shows its true left edge. Both then serve as rulers.
    off, _, ink = pil_mask(f, "_")
    assert off[1] == 0
    us_dy = ink_box(ink)[1]
    assert us_dy > 0
    _, _, ink = pil_mask(f, "H")
    hb = ink_box(ink)
    assert hb[0] > 0
    for ch in CHARS:
        box_l, box_t, _, box_b = f.getbbox(ch, mode="1", anchor="ls")
        _, _, ink = pil_mask(f, ch)
        ib = ink_box(ink)
        # ch + '_': the underscore's top row shows ch's bitmap top (start y 0.25 adds a row so
        # the underscore is never clipped); ch + 'H': H's right edge shows ch's bitmap left.
        _, _, cink = pil_mask(f, ch + "_", (0, 0.25))
        last = max(x for x, _ in cink)
        bmp_t = us_dy - min(y for x, y in cink if x == last)
        pos_h = length64(f, ch + "H") - length64(f, "H")
        _, _, hink = pil_mask(f, ch + "H")
        bmp_l = PIXEL(pos_h) + hb[2] - ink_box(hink)[2]
        assert bmp_t <= 0 and bmp_l <= 0, ch
        g = dict(adv=length64(f, ch), box_l=box_l, box_t=box_t, box_b=box_b, bmp_l=bmp_l, bmp_t=bmp_t,
                 dx=0, dy=0, w=0, h=0, ink=set())
        if ib:
            g.update(dx=ib[0] + bmp_l, dy=ib[1] + bmp_t, w=ib[2] - ib[0], h=ib[3] - ib[1],
                     ink={(x - ib[0], y - ib[1]) for x, y in ink})
        m.g[ord(ch)] = g
    for a, b in product(CHARS, CHARS):
        adj = length64(f, a + b) - length64(f, a) - length64(f, b)
        if adj:
            m.k[(ord(a), ord(b))] = adj
    for ch in CHARS:  # each glyph first, after H, before '_', and around a space, at fractional starts
        for s, start in ((ch, (0, 0)), ("H" + ch, (0, 0)), (ch + "_", (0, 0.25)), (ch, (0.5, 0.75)),
                         (" " + ch + "_", (0.3, 0.6)), (ch + "j", (-0.6, -0.7))):
            if any(lig in s for lig in ("fi", "fl", "ff")):
                continue
            assert m.mask(s, start) == pil_mask(f, s, start), (f.size, s, start)
    # draw_text computes the 'm' anchor from whole-pixel metrics (FreeType rounds them for scalable fonts)
    assert m.mask("H", anchor="mm")[0] == f.getbbox("H", "1", anchor="mm")[:2]
    return m


def font_header(size: int, bold: bool) -> str:
    f = ImageFont.truetype(str(ttf(bold)), size)
    m = measure(f)
    name = ident(size, bold)
    ascent, descent = f.getmetrics()
    bits, glyphs = bytearray(), []
    for ch in CHARS:
        g = m.g[ord(ch)]
        assert g["w"] < 256 and g["h"] < 256, (name, ch)
        stride = (g["w"] + 7) // 8
        data = bytearray(stride * g["h"])
        for x, y in g["ink"]:
            data[y * stride + x // 8] |= 0x80 >> (x % 8)
        glyphs.append((ord(ch), g["dx"], g["dy"], g["w"], g["h"], g["adv"], len(bits),
                       g["box_l"], g["box_t"], g["box_b"], g["bmp_l"], g["bmp_t"]))
        bits += data
    kerns = sorted((a, b, adj) for (a, b), adj in m.k.items())
    glyphs.sort()
    hexbytes = ",".join(f"0x{x:02x}" for x in bits) or "0"
    g = ",\n    ".join("{%d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d}" % x for x in glyphs)
    k = ",\n    ".join("{%d, %d, %d}" % x for x in kerns) or "{0, 0, 0}"
    return (
        "#pragma once\n// Generated by tools/gen_fonts.py from DejaVu Sans (tools/fonts/LICENSE); do not edit.\n"
        '#include "render/font.h"\n\nnamespace ink::fonts {\n\n'
        f"inline constexpr uint8_t {name}_bits[] = {{{hexbytes}}};\n"
        f"inline constexpr Glyph {name}_glyphs[] = {{\n    {g}}};\n"
        f"inline constexpr Kern {name}_kern[] = {{\n    {k}}};\n"
        f"inline constexpr Font {name} = {{{size}, {'true' if bold else 'false'}, {ascent}, {descent}, "
        f"{name}_glyphs, {len(glyphs)}, {name}_bits, {name}_kern, {len(kerns) if kerns else 0}}};\n\n"
        "}  // namespace ink::fonts\n")


def main() -> None:
    assert features.check("raqm"), "Pillow without raqm lays text out differently from the server"
    OUT.mkdir(parents=True, exist_ok=True)
    names = []
    for bold, sizes in ((False, REGULAR), (True, BOLD)):
        for size in sizes:
            (OUT / f"{ident(size, bold)}.h").write_text(font_header(size, bold))
            names.append((size, bold))
    inc = "\n".join(f'#include "render/fonts/{ident(s, b)}.h"' for s, b in names)
    lst = ",\n    ".join(f"&{ident(s, b)}" for s, b in names)
    (OUT / "all.h").write_text(
        "#pragma once\n// Generated by tools/gen_fonts.py; do not edit.\n" + inc +
        "\n\nnamespace ink::fonts {\ninline constexpr const Font* ALL[] = {\n    " + lst + "};\n}  // namespace ink::fonts\n")
    REF.mkdir(parents=True, exist_ok=True)
    rows = []
    cases = [(s, b, t, a, REF_X, REF_Y) for s, b, t, a in TEXT_CASES] + FRACTIONAL + EDGES
    for i, (size, bold, text, anchor, x, y) in enumerate(cases):
        img = Image.new("L", (REF_W, REF_H), 255)
        d = ImageDraw.Draw(img)
        d.fontmode = "1"
        d.text((x, y), text, font=ImageFont.truetype(str(ttf(bold)), size), fill=0, anchor=anchor)
        img.point(lambda p: 255 if p > 140 else 0).convert("1").save(REF / f"case_{i:02d}.pbm")
        # Octal escapes for non-ASCII bytes: C++ hex escapes are greedy ("\xb0C" would be one escape).
        lit = "".join(chr(b) if 32 <= b < 127 and chr(b) not in '"\\' else "\\%03o" % b for b in text.encode())
        rows.append('    {%d, %s, "%s", "%s", %r, %r, %r},' % (
            size, "true" if bold else "false", lit, anchor,
            ImageFont.truetype(str(ttf(bold)), size).getlength(text, mode="1"), float(x), float(y)))
    CASES_H.write_text(
        "#pragma once\n// Generated by tools/gen_fonts.py; do not edit.\n"
        "struct TextCase { int size; bool bold; const char* text; const char* anchor; double length, x, y; };\n"
        f"static const int TEXT_W = {REF_W}, TEXT_H = {REF_H}, TEXT_X = {REF_X}, TEXT_Y = {REF_Y};\n"
        "static const TextCase TEXT_CASES[] = {\n" + "\n".join(rows) + "\n};\n")
    print(f"wrote {len(names)} fonts, {len(cases)} text cases")


if __name__ == "__main__":
    main()
