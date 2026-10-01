#pragma once
// Text measure and draw with Pillow's anchors (spec §3.2): "la" "lm" "rm" "mm" "ra" "rb" "mt".
// draw_text ports ImageDraw.text + FreeTypeFont.getmask2 (Pillow 12.3.0 _imagingft.c:
// bounding_box_and_anchors, font_render) for one horizontal line in fontmode "1".
#include <math.h>
#include <stdint.h>

#include "render/canvas.h"
#include "render/font.h"

namespace ink {

// Next code point of a UTF-8 string (advances s); invalid bytes decode as U+FFFD.
inline uint32_t next_cp(const char*& s) {
  const uint8_t c = static_cast<uint8_t>(*s++);
  if (c < 0x80) return c;
  int n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : -1;
  if (n < 0) return 0xFFFD;
  uint32_t cp = c & (0x3F >> n);
  while (n-- > 0) {
    const uint8_t d = static_cast<uint8_t>(*s);
    if ((d & 0xC0) != 0x80) return 0xFFFD;
    cp = (cp << 6) | (d & 0x3F);
    ++s;
  }
  return cp;
}

// Pen positions in 1/64 px: advance plus pair kerning, as measured from Pillow's raqm layout.
inline double text_length(const Font& f, const char* utf8) {
  int64_t pen = 0;
  uint32_t prev = 0;
  for (const char* s = utf8; *s;) {
    const uint32_t cp = next_cp(s);
    if (prev) pen += kern64(f, prev, cp);
    if (const Glyph* g = find_glyph(f, cp)) pen += g->adv64;
    prev = cp;
  }
  return pen / 64.0;
}

// _imagingft.c PIXEL(): 26.6 to whole pixels, half up (floor division, also for negatives).
inline int32_t ft_pixel(int64_t v) {
  const int64_t t = v + 32;
  return static_cast<int32_t>(t >= 0 ? t / 64 : -((-t + 63) / 64));
}

// Walks the glyphs with their pen positions (1/64 px), in Pillow's layout order.
template <class Fn>
inline int64_t each_glyph(const Font& f, const char* utf8, Fn fn) {
  int64_t pen = 0;
  uint32_t prev = 0;
  for (const char* s = utf8; *s;) {
    const uint32_t cp = next_cp(s);
    if (prev) pen += kern64(f, prev, cp);
    if (const Glyph* g = find_glyph(f, cp)) {
      fn(*g, pen);
      pen += g->adv64;
    }
    prev = cp;
  }
  return pen;
}

inline void draw_text(View& v, double x, double y, const char* utf8, const Font& f, int ink, const char* anchor = "la") {
  // Pass 1 (bounding_box_and_anchors and font_render's first loop): the mask's outline box
  // (xb, yt, yb) and the bitmaps' top-left (xr, yr), all relative to the pen origin.
  int32_t xb = 0, xr = 0, yt = 0, yb = 0, yr = 0;
  const int64_t pos = each_glyph(f, utf8, [&](const Glyph& g, int64_t pen) {
    const int32_t px = ft_pixel(pen);
    if (g.box_l + px < xb) xb = g.box_l + px;
    if (g.bmp_l + px < xr) xr = g.bmp_l + px;
    if (g.box_t < yt) yt = g.box_t;
    if (g.box_b > yb) yb = g.box_b;
    if (g.bmp_t < yr) yr = g.bmp_t;
  });
  int32_t xa = 0, ya = 0;  // anchor point vs the pen origin (Pillow's y up)
  switch (anchor[0]) {
    case 'm': xa = ft_pixel(pos / 2); break;
    case 'r': xa = ft_pixel(pos); break;
    default: break;
  }
  switch (anchor[1]) {
    case 'a': ya = f.ascent; break;
    case 'd': ya = -f.descent; break;
    // FreeType keeps scalable fonts' ascender/descender in whole pixels (checked by gen_fonts.py).
    case 'm': ya = ft_pixel((static_cast<int64_t>(f.ascent) * 64 - static_cast<int64_t>(f.descent) * 64) / 2); break;
    case 't': ya = -yt; break;
    case 'b': ya = -yb; break;
    default: break;
  }
  // ImageDraw.text: the integer part places the mask, the fraction goes to the renderer as a C
  // float "start" that shifts the pen (and grows the mask by ceil(start)).
  double ix, iy;
  const float fx = static_cast<float>(modf(x, &ix)), fy = static_cast<float>(modf(y, &iy));
  const int mx = static_cast<int>(ix) - xa + xb, my = static_cast<int>(iy) + ya + yt;
  const int mask_h = -yt + yb + static_cast<int>(ceil(fy));
  const int64_t x64 = static_cast<int64_t>(round((static_cast<float>(-xr) + fx) * 64.0f));
  const int32_t py = ft_pixel(static_cast<int64_t>(round((static_cast<float>(yr) - fy) * 64.0f)));
  // Pass 2 (font_render's second loop): bitmaps are clipped to the mask. Only the left, top and
  // bottom edges can clip: bitmaps round inside the outline box, so they never pass the right edge.
  each_glyph(f, utf8, [&](const Glyph& g, int64_t pen) {
    const int gx = ft_pixel(x64 + pen) + g.dx, gy = -py + g.dy;
    const int stride = (g.w + 7) / 8;
    const uint8_t* rows = f.bits + g.offset;
    for (int r = 0; r < g.h; ++r) {
      const int ly = gy + r;
      if (ly < 0 || ly >= mask_h) continue;
      for (int c = 0; c < g.w; ++c) {
        if (gx + c < 0 || !(rows[r * stride + c / 8] & (0x80 >> (c % 8)))) continue;
        v.px(mx + gx + c, my + ly, ink);
      }
    }
  });
}

}  // namespace ink
