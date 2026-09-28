#pragma once
// Offline badge drawn straight into the 1-bit frame (spec §6.3): 5x7 font, no GFX.
// Frame layout: 800x480, rows top to bottom, MSB = leftmost pixel, bit 1 = white.
#include <stdint.h>
#include <string.h>

namespace badge {

constexpr int W = 800, H = 480, STRIDE = W / 8;

inline bool is_black(const uint8_t* frame, int x, int y) {
  return (frame[y * STRIDE + x / 8] & (0x80 >> (x & 7))) == 0;
}

inline void set_px(uint8_t* frame, int x, int y, bool black) {
  if (x < 0 || x >= W || y < 0 || y >= H) return;
  uint8_t mask = static_cast<uint8_t>(0x80 >> (x & 7));
  uint8_t& b = frame[y * STRIDE + x / 8];
  b = black ? static_cast<uint8_t>(b & ~mask) : static_cast<uint8_t>(b | mask);
}

// 7 rows of 5 bits (bit 4 = leftmost column). Only the characters the badge uses.
inline const uint8_t* glyph(char c) {
  static const uint8_t kBlank[7] = {0};
  static const struct { char c; uint8_t rows[7]; } kFont[] = {
      {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}}, {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
      {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}}, {'3', {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}},
      {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}}, {'5', {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}},
      {'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}}, {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
      {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}}, {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},
      {':', {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00}}, {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
      {'M', {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}}, {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
      {'c', {0x00, 0x00, 0x0E, 0x10, 0x10, 0x11, 0x0E}}, {'e', {0x00, 0x00, 0x0E, 0x11, 0x1F, 0x10, 0x0E}},
      {'f', {0x06, 0x09, 0x08, 0x1C, 0x08, 0x08, 0x08}}, {'i', {0x04, 0x00, 0x0C, 0x04, 0x04, 0x04, 0x0E}},
      {'l', {0x0C, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}}, {'n', {0x00, 0x00, 0x16, 0x19, 0x11, 0x11, 0x11}},
      {'o', {0x00, 0x00, 0x0E, 0x11, 0x11, 0x11, 0x0E}}, {'s', {0x00, 0x00, 0x0E, 0x10, 0x0E, 0x01, 0x1E}},
  };
  for (const auto& g : kFont)
    if (g.c == c) return g.rows;
  return kBlank;  // space and anything unknown
}

inline int text_width(const char* s, int scale) {
  int n = static_cast<int>(strlen(s));
  return n == 0 ? 0 : (n * 6 - 1) * scale;
}

inline void draw_text(uint8_t* frame, int x, int y, const char* s, int scale) {
  for (; *s; ++s, x += 6 * scale) {
    const uint8_t* rows = glyph(*s);
    for (int r = 0; r < 7; r++)
      for (int c = 0; c < 5; c++)
        if (rows[r] & (0x10 >> c))
          for (int dy = 0; dy < scale; dy++)
            for (int dx = 0; dx < scale; dx++) set_px(frame, x + c * scale + dx, y + r * scale + dy, true);
  }
}

struct Rect {
  int x, y, w, h;
};

inline Rect draw_badge(uint8_t* frame, const char* text, int scale = 2) {
  const int pad = 4 * scale;
  Rect r{0, 0, text_width(text, scale) + 2 * pad, 7 * scale + 2 * pad};
  r.x = W - r.w - 4;
  r.y = H - r.h - 2;
  for (int y = r.y; y < r.y + r.h; y++)
    for (int x = r.x; x < r.x + r.w; x++) {
      bool border = x < r.x + 2 || x >= r.x + r.w - 2 || y < r.y + 2 || y >= r.y + r.h - 2;
      set_px(frame, x, y, border);
    }
  draw_text(frame, r.x + pad, r.y + pad, text, scale);
  return r;
}

}  // namespace badge
