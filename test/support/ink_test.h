#pragma once
// Shared helpers for the native test suites: file access, goldens, reference images.
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unity.h>

#include <string>
#include <vector>

#include "render/png.h"

#ifndef INK_TEST_DIR
#error "INK_TEST_DIR comes from platformio.ini [env:native]"
#endif

namespace ink_test {

inline std::string path(const char* rel) { return std::string(INK_TEST_DIR) + "/" + rel; }

inline bool read_file(const std::string& p, std::string& out) {
  FILE* f = fopen(p.c_str(), "rb");
  if (!f) return false;
  out.clear();
  char buf[65536];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
  fclose(f);
  return true;
}

inline bool write_file(const std::string& p, const void* data, size_t n) {
  FILE* f = fopen(p.c_str(), "wb");
  if (!f) return false;
  bool ok = fwrite(data, 1, n, f) == n;
  return fclose(f) == 0 && ok;
}

inline bool update_goldens() {
  const char* v = getenv("INKBOARD_UPDATE_GOLDENS");
  return v != nullptr && v[0] == '1';
}

// Bit-for-bit double equality (Unity's DOUBLE asserts allow a relative tolerance).
inline bool same_double(double a, double b) { return memcmp(&a, &b, sizeof a) == 0; }

// Compare with test/goldens/<name>.png (byte-exact: png_encode is deterministic).
inline void golden(const char* name, const uint8_t* bits, int w, int h) {
  mkdir(path("goldens").c_str(), 0755);  // a clean checkout has no goldens yet
  const std::string p = path("goldens/") + name + ".png";
  const std::vector<uint8_t> png = ink::png_encode(bits, w, h);
  if (update_goldens()) {
    TEST_ASSERT_TRUE_MESSAGE(write_file(p, png.data(), png.size()), p.c_str());
    return;
  }
  std::string want;
  if (!read_file(p, want)) {
    TEST_FAIL_MESSAGE(("missing golden " + p + ": run INKBOARD_UPDATE_GOLDENS=1 pio test -e native, then look at it").c_str());
  }
  if (want.size() != png.size() || memcmp(want.data(), png.data(), png.size()) != 0) {
    const std::string actual = path("goldens/") + name + ".actual.png";
    write_file(actual, png.data(), png.size());
    TEST_FAIL_MESSAGE((std::string(name) + " differs from its golden; wrote " + actual).c_str());
  }
}

// Binary PBM (P4, bit 1 = black, as Pillow writes it) -> frame-style bits (bit 1 = white).
inline bool read_pbm(const std::string& p, std::vector<uint8_t>& bits, int& w, int& h) {
  std::string s;
  if (!read_file(p, s) || s.compare(0, 2, "P4") != 0) return false;
  size_t i = 2;
  int vals[2];
  for (int& v : vals) {
    while (i < s.size() && (isspace(static_cast<unsigned char>(s[i])) || s[i] == '#')) {
      if (s[i] == '#') while (i < s.size() && s[i] != '\n') ++i;
      else ++i;
    }
    v = 0;
    while (i < s.size() && isdigit(static_cast<unsigned char>(s[i]))) v = v * 10 + (s[i++] - '0');
  }
  ++i;  // the single whitespace byte before the raster
  w = vals[0];
  h = vals[1];
  const size_t n = static_cast<size_t>((w + 7) / 8) * h;
  if (s.size() < i + n) return false;
  bits.assign(s.begin() + i, s.begin() + i + n);
  for (uint8_t& b : bits) b = static_cast<uint8_t>(~b);
  return true;
}

// Pixels that differ from test/<rel_pbm>, ignoring padding bits; -1 if missing or a size
// mismatch. On a difference writes <rel_pbm>.cpp.png next to it for side-by-side review.
inline long match_reference(const char* rel_pbm, const uint8_t* bits, int w, int h) {
  std::vector<uint8_t> want;
  int ww, hh;
  if (!read_pbm(path(rel_pbm), want, ww, hh) || ww != w || hh != h) return -1;
  const int stride = (w + 7) / 8;
  long diff = 0;
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const uint8_t m = static_cast<uint8_t>(0x80 >> (x & 7));
      if ((want[y * stride + x / 8] & m) != (bits[y * stride + x / 8] & m)) ++diff;
    }
  if (diff) {
    const std::vector<uint8_t> png = ink::png_encode(bits, w, h);
    write_file(path(rel_pbm) + ".cpp.png", png.data(), png.size());
  }
  return diff;
}

}  // namespace ink_test
