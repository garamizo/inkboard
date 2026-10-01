#include <unity.h>

#include "../support/ink_test.h"
#include "render/calendar.h"
#include "render/icons.h"
#include "render/lines.h"
#include <string>
#include <vector>

#include "../fixtures/fixtures.h"
#include "json_stream.h"
#include "widgets/calendar_weather.h"


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

// Reference = the server's own render of the same data (tools/ref_render.py). Primitives and
// text match Pillow exactly (Tasks 4-5), so any difference here comes from the port itself.
static void check_reference(const char* name, const uint8_t* bits, int w, int h) {
  char rel[96];
  snprintf(rel, sizeof rel, "reference/widgets/%s.pbm", name);
  const long diff = ink_test::match_reference(rel, bits, w, h);
  printf("%s: %ld pixels differ from the server render\n", name, diff);
  TEST_ASSERT_TRUE_MESSAGE(diff >= 0, "missing reference: run tools/ref_render.py");
  TEST_ASSERT_TRUE_MESSAGE(diff <= w * h / 200, name);  // > 0.5 %: a layout bug, not rounding
  ink_test::golden(name, bits, w, h);
}

static ink::Weather fixture_weather() {
  std::string doc;
  TEST_ASSERT_TRUE(ink_test::read_file(ink_test::path("fixtures/weather_la.json"), doc));
  ink::OpenMeteoParser h;
  ink::json::Parser p(h);
  p.feed(doc.data(), doc.size());
  ink::Weather w;
  TEST_ASSERT_TRUE(p.finish() && h.result(w));
  return w;
}

void test_weather_payload_picks_rows_by_date() {
  ink::Weather w = fixture_weather();
  ink::WeatherPayload p = ink::build_weather_payload(w, FIXTURE_TODAY);
  TEST_ASSERT_TRUE(p.has_today);
  TEST_ASSERT_EQUAL_INT32(FIXTURE_TODAY, p.today.day);
  TEST_ASSERT_EQUAL_UINT8(7, p.n_upcoming);
  p = ink::build_weather_payload(w, FIXTURE_TODAY + 1);      // the cache still holds yesterday's forecast
  TEST_ASSERT_EQUAL_INT32(FIXTURE_TODAY + 1, p.today.day);
  TEST_ASSERT_EQUAL_INT32(FIXTURE_TODAY + 2, p.upcoming[0].day);
  p = ink::build_weather_payload(w, FIXTURE_TODAY + 30);
  TEST_ASSERT_FALSE(p.has_today);
  TEST_ASSERT_EQUAL_UINT8(0, p.n_upcoming);
}

void test_calendar_weather_matches_server() {
  const ink::Size sizes[] = {ink::Size::Third, ink::Size::TwoThirds, ink::Size::Full};
  const char* names[] = {"calendar_weather_third", "calendar_weather_two_thirds", "calendar_weather_full"};
  const ink::WeatherPayload p = ink::build_weather_payload(fixture_weather(), FIXTURE_TODAY);
  for (int i = 0; i < 3; ++i) {
    const int w = ink::size_width(sizes[i]);
    std::vector<uint8_t> bits(static_cast<size_t>((w + 7) / 8) * ink::WIDGET_H);
    ink::Bitmap bm(bits.data(), w, ink::WIDGET_H);
    bm.fill(ink::WHITE);
    ink::View v(bm, ink::Box{0, 0, w, ink::WIDGET_H});
    ink::render_calendar_weather(v, sizes[i], p, FIXTURE_TODAY);
    check_reference(names[i], bits.data(), w, ink::WIDGET_H);
  }
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_wmo_mapping);
  RUN_TEST(test_month_weeks_sunday_first);
  RUN_TEST(test_marker_positions_and_y_at);
  RUN_TEST(test_weather_payload_picks_rows_by_date);
  RUN_TEST(test_calendar_weather_matches_server);
  return UNITY_END();
}
