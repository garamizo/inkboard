#pragma once
// 1-bit grayscale PNG with stored (uncompressed) deflate blocks: no zlib, and the bytes
// depend only on the pixels, so goldens compare by file content. Host-side only.
#include <stddef.h>
#include <stdint.h>

#include <vector>

#include "crc32.h"

namespace ink {

namespace png_detail {
inline void put32(std::vector<uint8_t>& v, uint32_t x) {
  for (int s = 24; s >= 0; s -= 8) v.push_back(static_cast<uint8_t>(x >> s));
}
inline void chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data) {
  put32(out, static_cast<uint32_t>(data.size()));
  size_t start = out.size();
  out.insert(out.end(), type, type + 4);
  out.insert(out.end(), data.begin(), data.end());
  put32(out, crc32(out.data() + start, out.size() - start));
}
}  // namespace png_detail

// bits: h rows of (w + 7) / 8 bytes, MSB = leftmost pixel, bit 1 = white (PNG gray 1-bit agrees).
inline std::vector<uint8_t> png_encode(const uint8_t* bits, int w, int h) {
  using namespace png_detail;
  const size_t stride = static_cast<size_t>(w + 7) / 8;
  std::vector<uint8_t> raw;
  raw.reserve((stride + 1) * static_cast<size_t>(h));
  for (int y = 0; y < h; ++y) {
    raw.push_back(0);  // filter: none
    raw.insert(raw.end(), bits + y * stride, bits + (y + 1) * stride);
  }
  std::vector<uint8_t> z{0x78, 0x01};
  size_t off = 0;
  do {
    size_t n = raw.size() - off;
    if (n > 65535) n = 65535;
    const bool last = off + n == raw.size();
    const uint16_t len = static_cast<uint16_t>(n), nlen = static_cast<uint16_t>(~len);
    z.insert(z.end(), {static_cast<uint8_t>(last ? 1 : 0), static_cast<uint8_t>(len), static_cast<uint8_t>(len >> 8),
                       static_cast<uint8_t>(nlen), static_cast<uint8_t>(nlen >> 8)});
    z.insert(z.end(), raw.begin() + off, raw.begin() + off + n);
    off += n;
  } while (off < raw.size());
  uint32_t a = 1, b = 0;
  for (uint8_t c : raw) {
    a = (a + c) % 65521;
    b = (b + a) % 65521;
  }
  put32(z, (b << 16) | a);
  std::vector<uint8_t> out{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  std::vector<uint8_t> ihdr;
  put32(ihdr, static_cast<uint32_t>(w));
  put32(ihdr, static_cast<uint32_t>(h));
  ihdr.insert(ihdr.end(), {1, 0, 0, 0, 0});  // bit depth 1, grayscale, deflate, filter 0, no interlace
  chunk(out, "IHDR", ihdr);
  chunk(out, "IDAT", z);
  chunk(out, "IEND", {});
  return out;
}

}  // namespace ink
