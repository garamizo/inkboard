#pragma once
// CRC-32 (IEEE, as zlib/PNG): frame change detection, cache files, PNG chunks.
#include <stddef.h>
#include <stdint.h>

namespace ink {

inline uint32_t crc32_update(uint32_t crc, const uint8_t* p, size_t n) {
  crc = ~crc;
  for (size_t i = 0; i < n; ++i) {
    crc ^= p[i];
    for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

inline uint32_t crc32(const uint8_t* p, size_t n) { return crc32_update(0, p, n); }

}  // namespace ink
