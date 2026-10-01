#include <unity.h>

#include "../support/ink_test.h"
#include "render/calendar.h"
#include "render/icons.h"
#include "render/lines.h"

void setUp() {}
void tearDown() {}

void test_wmo_mapping() {
  TEST_ASSERT_EQUAL_STRING("Clear", ink::wmo_info(0).label);
  TEST_ASSERT_TRUE(ink::wmo_info(2).icon == ink::Icon::Part);
  TEST_ASSERT_TRUE(ink::wmo_info(48).icon == ink::Icon::Fog);
  TEST_ASSERT_TRUE(ink::wmo_info(81).icon == ink::Icon::Rain);
  TEST_ASSERT_TRUE(ink::wmo_info(86).icon == ink::Icon::Snow);
  TEST_ASSERT_TRUE(ink::wmo_info(96).icon == ink::Icon::Storm);
  TEST_ASSERT_EQUAL_STRING("\xE2\x80\x94", ink::wmo_info(42).label);  // "—"
  TEST_ASSERT_TRUE(ink::wmo_info(42).icon == ink::Icon::Cloud);
}

void test_month_weeks_sunday_first() {
  int w[6][7];
  TEST_ASSERT_EQUAL_INT(5, ink::month_weeks(2026, 9, w));  // Sep 1, 2026 is a Tuesday
  TEST_ASSERT_EQUAL_INT(0, w[0][1]);
  TEST_ASSERT_EQUAL_INT(1, w[0][2]);
  TEST_ASSERT_EQUAL_INT(27, w[4][0]);
  TEST_ASSERT_EQUAL_INT(30, w[4][3]);
  TEST_ASSERT_EQUAL_INT(0, w[4][4]);
  TEST_ASSERT_EQUAL_INT(6, ink::month_weeks(2026, 8, w));  // Aug 2026 starts on a Saturday
}

void test_marker_positions_and_y_at() {
  double xs[16];
  const int n = ink::marker_xs(10, 200, 1, 4, xs, 16);
  TEST_ASSERT_EQUAL_INT(3, n);                              // 10 + 56 * 1.5 / 4 = 31, then 87, 143; 199 is not < 200 - 4
  TEST_ASSERT_EQUAL_DOUBLE(31.0, xs[0]);
  const ink::Pt pts[] = {{0, 0}, {10, 10}, {20, 0}};
  TEST_ASSERT_EQUAL_DOUBLE(5.0, ink::y_at(pts, 3, 5));
  TEST_ASSERT_EQUAL_DOUBLE(0.0, ink::y_at(pts, 3, -3));
  TEST_ASSERT_EQUAL_DOUBLE(0.0, ink::y_at(pts, 3, 25));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_wmo_mapping);
  RUN_TEST(test_month_weeks_sunday_first);
  RUN_TEST(test_marker_positions_and_y_at);
  return UNITY_END();
}
