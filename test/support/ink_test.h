#pragma once
// Shared helpers for the native test suites: file access, goldens, reference images.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unity.h>

#include <string>
#include <vector>

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

}  // namespace ink_test
