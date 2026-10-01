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
#include "sources/fred.h"
#include "widgets/market_trends.h"


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

// Fixture observations, optionally only those on or after `since` (the short-history scenario).
struct ObsCollector : ink::json::Handler {
  std::vector<std::pair<int32_t, double>> obs;
  int32_t date = INT32_MIN;
  std::string value;
  void scalar(const ink::json::Parser& p, ink::json::Type, const char* s) override {
    if (p.at({"observations", nullptr, "date"})) date = ink::parse_iso_date(s);
    if (p.at({"observations", nullptr, "value"})) value = s;
  }
  void end_container(const ink::json::Parser& p) override {
    if (p.at({"observations", nullptr})) {
      if (value != "." && !value.empty()) obs.emplace_back(date, strtod(value.c_str(), nullptr));
      value.clear();
    }
  }
};

static ink::SeriesData g_series[4];
static ink::Summary g_sum[4];

static void load_series(int slot, const char* fred_id, int32_t since = INT32_MIN) {
  std::string doc;
  TEST_ASSERT_TRUE(ink_test::read_file(ink_test::path((std::string("fixtures/fred_") + fred_id + "_full.json").c_str()), doc));
  ObsCollector c;
  ink::json::Parser p(c);
  p.feed(doc.data(), doc.size());
  TEST_ASSERT_TRUE(p.finish());
  ink::SundayResampler r;
  const int32_t g0 = ink::first_sunday_on_or_after(ink::window_start_day(FIXTURE_TODAY, 10));
  r.begin_full(g_series[slot], g0, FIXTURE_TODAY);
  for (auto& o : c.obs)
    if (o.first >= since) r.add(o.first, o.second);
  TEST_ASSERT_TRUE(r.finish(g0));
}

static ink::MarketPayload payload(const int* catalog_idx, int n) {
  ink::MarketPayload p{n, {}, 5, FIXTURE_TODAY};
  for (int i = 0; i < n; ++i) {
    TEST_ASSERT_TRUE(ink::summarize(catalog_idx[i], g_series[i], FIXTURE_TODAY, 5, g_sum[i]));
    p.series[i] = &g_sum[i];
  }
  return p;
}

static void render_market(const char* name, ink::Size size, const ink::MarketPayload& p) {
  const int w = ink::size_width(size);
  std::vector<uint8_t> bits(static_cast<size_t>((w + 7) / 8) * ink::WIDGET_H);
  ink::Bitmap bm(bits.data(), w, ink::WIDGET_H);
  bm.fill(ink::WHITE);
  ink::View v(bm, ink::Box{0, 0, w, ink::WIDGET_H});
  TEST_ASSERT_TRUE(ink::market_payload_ok(p));
  ink::render_market_trends(v, size, p);
  check_reference(name, bits.data(), w, ink::WIDGET_H);
}

void test_market_trends_matches_server() {
  const char* ids[] = {"SP500", "CBBTCUSD", "MORTGAGE30US", "MEDLISPRI31080"};
  for (int i = 0; i < 4; ++i) load_series(i, ids[i]);
  const int idx[] = {0, 1, 2, 3};
  const ink::MarketPayload p = payload(idx, 4);
  render_market("market_trends_third", ink::Size::Third, p);
  render_market("market_trends_two_thirds", ink::Size::TwoThirds, p);
  render_market("market_trends_full", ink::Size::Full, p);
}

void test_market_trends_short_series() {
  load_series(0, "SP500");
  load_series(1, "MEDLISPRI31080", ink::days_from_civil(2024, 1, 1));
  const int idx[] = {0, 3};
  const ink::MarketPayload p = payload(idx, 2);
  TEST_ASSERT_TRUE(g_sum[1].short_history);
  render_market("market_trends_short_series", ink::Size::TwoThirds, p);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_wmo_mapping);
  RUN_TEST(test_month_weeks_sunday_first);
  RUN_TEST(test_marker_positions_and_y_at);
  RUN_TEST(test_weather_payload_picks_rows_by_date);
  RUN_TEST(test_calendar_weather_matches_server);
  RUN_TEST(test_market_trends_matches_server);
  RUN_TEST(test_market_trends_short_series);
  return UNITY_END();
}
