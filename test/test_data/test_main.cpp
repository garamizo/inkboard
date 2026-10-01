#include <unity.h>

#include "../support/ink_test.h"
#include "civil.h"
#include "format.h"

using namespace ink;

void setUp() {}
void tearDown() {}

void test_test_dir_is_absolute_and_readable() {
  std::string s;
  TEST_ASSERT_EQUAL_CHAR('/', INK_TEST_DIR[0]);
  TEST_ASSERT_TRUE(ink_test::read_file(ink_test::path("support/ink_test.h"), s));
  TEST_ASSERT_NOT_EQUAL(std::string::npos, s.find("INK_TEST_DIR"));
}

void test_civil_roundtrip_and_weekday() {
  TEST_ASSERT_EQUAL_INT32(0, days_from_civil(1970, 1, 1));
  TEST_ASSERT_EQUAL_INT32(20723, days_from_civil(2026, 9, 27));
  Ymd d = civil_from_days(20723);
  TEST_ASSERT_EQUAL_INT(2026, d.y);
  TEST_ASSERT_EQUAL_INT(9, d.m);
  TEST_ASSERT_EQUAL_INT(27, d.d);
  TEST_ASSERT_EQUAL_INT(3, weekday(0));         // 1970-01-01 was a Thursday
  TEST_ASSERT_EQUAL_INT(6, weekday(20723));     // 2026-09-27 is a Sunday
  for (int32_t day = -800; day < 30000; day += 37) {
    Ymd x = civil_from_days(day);
    TEST_ASSERT_EQUAL_INT32(day, days_from_civil(x.y, x.m, x.d));
  }
}

void test_floor_div_mod() {
  TEST_ASSERT_EQUAL_INT64(-1, floor_div(-1, 7));
  TEST_ASSERT_EQUAL_INT64(6, floor_mod(-1, 7));
  TEST_ASSERT_EQUAL_INT64(2, floor_div(14, 7));
  TEST_ASSERT_EQUAL_INT64(0, floor_mod(14, 7));
}

void test_parse_iso_date() {
  TEST_ASSERT_EQUAL_INT32(20723, parse_iso_date("2026-09-27"));
  TEST_ASSERT_EQUAL_INT32(INT32_MIN, parse_iso_date("2026-9-27"));
  TEST_ASSERT_EQUAL_INT32(INT32_MIN, parse_iso_date("2026-02-30"));
  TEST_ASSERT_EQUAL_INT32(INT32_MIN, parse_iso_date("2026-09-27x"));
  TEST_ASSERT_EQUAL_INT32(INT32_MIN, parse_iso_date(""));
  TEST_ASSERT_EQUAL_INT32(days_from_civil(2024, 2, 29), parse_iso_date("2024-02-29"));
}

void test_first_sunday() {
  TEST_ASSERT_EQUAL_INT32(20723, first_sunday_on_or_after(20723));      // a Sunday stays
  TEST_ASSERT_EQUAL_INT32(20723, first_sunday_on_or_after(20717));      // the Monday before
  TEST_ASSERT_EQUAL_INT32(20730, first_sunday_on_or_after(20724));
}

void test_format_python_compatible() {
  char b[32];
  fmt_deg(b, sizeof b, -0.4);  TEST_ASSERT_EQUAL_STRING("0\xC2\xB0", b);
  fmt_deg(b, sizeof b, 72.5);  TEST_ASSERT_EQUAL_STRING("72\xC2\xB0", b);   // half to even
  fmt_deg(b, sizeof b, 73.5);  TEST_ASSERT_EQUAL_STRING("74\xC2\xB0", b);
  fmt_deg(b, sizeof b, -3.6);  TEST_ASSERT_EQUAL_STRING("-4\xC2\xB0", b);
  fmt_thousands0(b, sizeof b, 7743.2);    TEST_ASSERT_EQUAL_STRING("7,743", b);
  fmt_thousands0(b, sizeof b, 1234567.5); TEST_ASSERT_EQUAL_STRING("1,234,568", b);
  fmt_thousands0(b, sizeof b, 999.4);     TEST_ASSERT_EQUAL_STRING("999", b);
  fmt_thousands0(b, sizeof b, -1234.0);   TEST_ASSERT_EQUAL_STRING("-1,234", b);
  fmt_clock(b, sizeof b, 0, 5);   TEST_ASSERT_EQUAL_STRING("12:05 AM", b);
  fmt_clock(b, sizeof b, 10, 0);  TEST_ASSERT_EQUAL_STRING("10:00 AM", b);
  fmt_clock(b, sizeof b, 12, 30); TEST_ASSERT_EQUAL_STRING("12:30 PM", b);
  fmt_clock(b, sizeof b, 23, 59); TEST_ASSERT_EQUAL_STRING("11:59 PM", b);
  TEST_ASSERT_EQUAL_STRING("Sun", DAY_ABBR[6]);
  TEST_ASSERT_EQUAL_STRING("Sunday", DAY_NAME[6]);
  TEST_ASSERT_EQUAL_STRING("Sep", MONTH_ABBR[9]);
  TEST_ASSERT_EQUAL_STRING("September", MONTH_NAME[9]);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_test_dir_is_absolute_and_readable);
  RUN_TEST(test_civil_roundtrip_and_weekday);
  RUN_TEST(test_floor_div_mod);
  RUN_TEST(test_parse_iso_date);
  RUN_TEST(test_first_sunday);
  RUN_TEST(test_format_python_compatible);
  return UNITY_END();
}
