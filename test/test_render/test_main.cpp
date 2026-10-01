#include <unity.h>

#include "../support/ink_test.h"
#include "crc32.h"
#include "render/png.h"

void setUp() {}
void tearDown() {}

void test_crc32_known_value() {
  const char* s = "123456789";
  TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, ink::crc32(reinterpret_cast<const uint8_t*>(s), 9));
}

void test_png_header_and_size() {
  uint8_t bits[2 * 3] = {0xFF, 0xC0, 0x00, 0x00, 0xAA, 0x40};  // 10×3
  std::vector<uint8_t> png = ink::png_encode(bits, 10, 3);
  const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  TEST_ASSERT_EQUAL_MEMORY(sig, png.data(), 8);
  TEST_ASSERT_EQUAL_MEMORY("IHDR", png.data() + 12, 4);
  // 8 sig + IHDR(25) + IDAT(12 + 2 zlib hdr + 5 block hdr + 3 rows × 3 bytes + 4 adler) + IEND(12)
  TEST_ASSERT_EQUAL_UINT32(8 + 25 + 12 + 2 + 5 + 9 + 4 + 12, png.size());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_crc32_known_value);
  RUN_TEST(test_png_header_and_size);
  return UNITY_END();
}
