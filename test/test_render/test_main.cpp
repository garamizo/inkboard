#include <unity.h>

#include "../support/ink_test.h"
#include "crc32.h"
#include "render/png.h"
#include "render/canvas.h"
#include "prim_cases.h"
#include "render/text.h"
#include "text_cases.h"

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

static void replay(ink::View& v, const PrimOp& op) {
  using ink::Pt;
  const std::string k = op.kind;
  if (k == "line") {
    Pt pts[8];
    for (int i = 0; i < op.n / 2; ++i) pts[i] = Pt{op.v[2 * i], op.v[2 * i + 1]};
    v.line(pts, op.n / 2, op.fill, op.width);
  } else if (k == "points") {
    for (int x = int(op.v[0]); x < int(op.v[1]); x += int(op.v[2])) v.point(x, op.v[3], op.fill);
  } else if (k == "rectangle") {
    v.rectangle(op.v[0], op.v[1], op.v[2], op.v[3], op.fill, op.outline, op.width);
  } else if (k == "ellipse") {
    v.ellipse(op.v[0], op.v[1], op.v[2], op.v[3], op.fill, op.outline, op.width);
  } else if (k == "polygon") {
    Pt pts[8];
    for (int i = 0; i < op.n / 2; ++i) pts[i] = Pt{op.v[2 * i], op.v[2 * i + 1]};
    v.polygon(pts, op.n / 2, op.fill, op.outline, op.width);
  } else if (k == "rounded_rectangle") {
    v.rounded_rectangle(op.v[0], op.v[1], op.v[2], op.v[3], op.v[4], op.fill, op.outline, op.width);
  } else {
    TEST_FAIL_MESSAGE(op.kind);
  }
}

void test_primitives_match_pillow() {
  int failed = 0;
  for (const PrimCase& c : PRIM_CASES) {
    std::vector<uint8_t> bits(static_cast<size_t>((PRIM_W + 7) / 8) * PRIM_H);
    ink::Bitmap bm(bits.data(), PRIM_W, PRIM_H);
    bm.fill(ink::WHITE);
    ink::View v(bm, ink::Box{0, 0, PRIM_W, PRIM_H});
    for (int i = 0; i < c.n_ops; ++i) replay(v, c.ops[i]);
    long diff = ink_test::match_reference((std::string("reference/primitives/") + c.name + ".pbm").c_str(),
                                          bits.data(), PRIM_W, PRIM_H);
    if (diff != 0) {
      printf("primitive %s: %ld pixels differ (see test/reference/primitives/%s.pbm.cpp.png)\n", c.name, diff, c.name);
      ++failed;
    }
  }
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, failed, "primitives differ from Pillow");
}

// A view stands for a Pillow image of its box's size pasted at box.x (each server widget drew
// on its own image), so a case drawn at a column offset must match the same reference.
void test_primitives_match_pillow_at_column_offsets() {
  int failed = 0;
  const int offsets[] = {266, 534};
  const int stride = (ink::FRAME_W + 7) / 8, rstride = (PRIM_W + 7) / 8;
  for (int ox : offsets)
    for (const PrimCase& c : PRIM_CASES) {
      std::vector<uint8_t> frame(static_cast<size_t>(stride) * PRIM_H);
      ink::Bitmap bm(frame.data(), ink::FRAME_W, PRIM_H);
      bm.fill(ink::WHITE);
      ink::View v(bm, ink::Box{ox, 0, PRIM_W, PRIM_H});
      for (int i = 0; i < c.n_ops; ++i) replay(v, c.ops[i]);
      std::vector<uint8_t> region(static_cast<size_t>(rstride) * PRIM_H, 0xFF);
      long outside = 0;
      for (int y = 0; y < PRIM_H; ++y)
        for (int x = 0; x < ink::FRAME_W; ++x) {
          const bool black = bm.is_black(x, y);
          if (x < ox || x >= ox + PRIM_W) outside += black;
          else if (black) region[y * rstride + (x - ox) / 8] &= static_cast<uint8_t>(~(0x80 >> ((x - ox) & 7)));
        }
      long diff = ink_test::match_reference((std::string("reference/primitives/") + c.name + ".pbm").c_str(),
                                            region.data(), PRIM_W, PRIM_H);
      if (diff != 0 || outside != 0) {
        printf("primitive %s at x=%d: %ld pixels differ, %ld outside the box\n", c.name, ox, diff, outside);
        ++failed;
      }
    }
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, failed, "offset views differ from Pillow");
}

void test_view_clips_and_translates() {
  std::vector<uint8_t> bits(static_cast<size_t>(10 / 8 + 1) * 10, 0xFF);
  ink::Bitmap bm(bits.data(), 10, 10);
  ink::View v(bm, ink::Box{2, 3, 4, 4});
  v.rectangle(-5, -5, 50, 50, ink::BLACK);
  for (int y = 0; y < 10; ++y)
    for (int x = 0; x < 10; ++x)
      TEST_ASSERT_EQUAL(x >= 2 && x < 6 && y >= 3 && y < 7, bm.is_black(x, y));
  ink::View inner = v.sub(ink::Box{1, 1, 10, 10});  // clipped to the parent
  TEST_ASSERT_EQUAL_INT(3, inner.w());
  TEST_ASSERT_EQUAL_INT(3, inner.h());
}

void test_every_widget_font_exists() {
  const int regular[] = {10, 11, 12, 13, 14, 15, 18};
  const int bold[] = {8, 11, 12, 13, 14, 15, 17, 18, 20, 22, 26, 36, 46};
  for (int s : regular) TEST_ASSERT_NOT_NULL(ink::find_font(s, false));
  for (int s : bold) TEST_ASSERT_NOT_NULL(ink::find_font(s, true));
  TEST_ASSERT_NULL(ink::find_font(9, false));
  TEST_ASSERT_NOT_NULL(ink::find_glyph(ink::font(10), 0x26A0));  // ⚠
}

void test_text_length_matches_pillow() {
  for (const TextCase& c : TEXT_CASES) {
    TEST_ASSERT_DOUBLE_WITHIN_MESSAGE(1.0 / 64, c.length, ink::text_length(ink::font(c.size, c.bold), c.text), c.text);
  }
}

void test_text_matches_pillow() {
  int failed = 0;
  for (size_t i = 0; i < sizeof(TEXT_CASES) / sizeof(TEXT_CASES[0]); ++i) {
    const TextCase& c = TEXT_CASES[i];
    std::vector<uint8_t> bits(static_cast<size_t>((TEXT_W + 7) / 8) * TEXT_H);
    ink::Bitmap bm(bits.data(), TEXT_W, TEXT_H);
    bm.fill(ink::WHITE);
    ink::View v(bm, ink::Box{0, 0, TEXT_W, TEXT_H});
    ink::draw_text(v, c.x, c.y, c.text, ink::font(c.size, c.bold), ink::BLACK, c.anchor);
    char rel[64];
    snprintf(rel, sizeof rel, "reference/text/case_%02zu.pbm", i);
    long diff = ink_test::match_reference(rel, bits.data(), TEXT_W, TEXT_H);
    if (diff != 0) {
      printf("text case %zu (%s, %s): %ld pixels differ\n", i, c.text, c.anchor, diff);
      ++failed;
    }
  }
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, failed, "text differs from Pillow");
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_crc32_known_value);
  RUN_TEST(test_png_header_and_size);
  RUN_TEST(test_primitives_match_pillow);
  RUN_TEST(test_primitives_match_pillow_at_column_offsets);
  RUN_TEST(test_view_clips_and_translates);
  RUN_TEST(test_every_widget_font_exists);
  RUN_TEST(test_text_length_matches_pillow);
  RUN_TEST(test_text_matches_pillow);
  return UNITY_END();
}
