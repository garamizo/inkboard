#pragma once
// Bitmap font format written by tools/gen_fonts.py (spec §3.1). Data lives in flash
// (constexpr arrays); C++17 inline variables keep one copy across translation units.
#include <stdint.h>
#include <stdlib.h>

namespace ink {

// Offsets are whole pixels relative to the pen on the baseline, y down.
struct Glyph {
  uint16_t cp;
  int16_t dx, dy;  // top-left of the ink box
  uint8_t w, h;    // ink box size; 0×0 for blank glyphs (space)
  int32_t adv64;   // advance in 1/64 px (Pillow getlength() * 64)
  uint32_t offset; // first byte in Font::bits; rows of (w + 7) / 8 bytes, MSB left, bit 1 = ink
  // Pillow sizes the text mask from FreeType's outline boxes (rounded outward) but places glyphs
  // by their mono bitmaps (rounded to nearest, 1×1 even for a space); both shift the whole line,
  // so both are kept. Clamped to include the pen point, as Pillow's min/max start from 0.
  int8_t box_l, box_t, box_b;  // outline box: min(0, left), min(0, top), max(0, bottom)
  int8_t bmp_l, bmp_t;         // bitmap origin: min(0, left), min(0, top)
};
struct Kern {
  uint16_t left, right;
  int16_t adj64;
};
struct Font {
  uint8_t size;
  bool bold;
  int16_t ascent, descent;  // ImageFont.getmetrics()
  const Glyph* glyphs;      // sorted by cp
  uint16_t n_glyphs;
  const uint8_t* bits;
  const Kern* kern;         // sorted by (left, right)
  uint16_t n_kern;
};

}  // namespace ink

#include "render/fonts/all.h"

namespace ink {

inline const Font* find_font(int size, bool bold) {
  for (const Font* f : fonts::ALL)
    if (f->size == size && f->bold == bold) return f;
  return nullptr;
}

// Every size the widgets use is generated (test_every_widget_font_exists); a miss is a bug.
inline const Font& font(int size, bool bold = false) {
  const Font* f = find_font(size, bold);
  if (f == nullptr) abort();
  return *f;
}

inline const Glyph* find_glyph(const Font& f, uint32_t cp) {
  int lo = 0, hi = f.n_glyphs;
  while (lo < hi) {
    const int mid = (lo + hi) / 2;
    if (f.glyphs[mid].cp == cp) return &f.glyphs[mid];
    if (f.glyphs[mid].cp < cp) lo = mid + 1;
    else hi = mid;
  }
  return nullptr;
}

inline int32_t kern64(const Font& f, uint32_t left, uint32_t right) {
  int lo = 0, hi = f.n_kern;
  while (lo < hi) {
    const int mid = (lo + hi) / 2;
    const Kern& k = f.kern[mid];
    if (k.left == left && k.right == right) return k.adj64;
    if (k.left < left || (k.left == left && k.right < right)) lo = mid + 1;
    else hi = mid;
  }
  return 0;
}

}  // namespace ink
