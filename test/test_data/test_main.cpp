#include <unity.h>

#include <algorithm>

#include "../support/ink_test.h"
#include "civil.h"
#include "format.h"
#include "json_stream.h"
#include "tz.h"
#include "tz_cases.h"
#include "query.h"

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

void test_tz_lookup() {
  TEST_ASSERT_EQUAL_STRING("PST8PDT,M3.2.0,M11.1.0", ink::tz::lookup("America/Los_Angeles"));
  TEST_ASSERT_NOT_NULL(ink::tz::lookup("UTC"));
  TEST_ASSERT_NOT_NULL(ink::tz::lookup("America/Vancouver"));
  TEST_ASSERT_NULL(ink::tz::lookup("America"));
  TEST_ASSERT_NULL(ink::tz::lookup("../../etc/passwd"));
  // tzdata 2026c lists no Ramadan switches for Morocco (footer <+00>0), so it is expressible.
  // An older tzdata leaves it out; the generator keeps any zone whose footer disagrees with zoneinfo out.
  TEST_ASSERT_NOT_NULL(ink::tz::lookup("Africa/Casablanca"));
}

void test_tz_every_table_rule_parses() {
  for (const ink::tz::Entry& e : ink::tz::TABLE) {
    ink::tz::Rule r;
    TEST_ASSERT_TRUE_MESSAGE(ink::tz::parse(e.rule, r), e.name);
  }
}

void test_tz_matches_zoneinfo() {
  int failed = 0;
  for (const TzCase& c : TZ_CASES) {
    ink::tz::Rule r;
    TEST_ASSERT_TRUE(ink::tz::parse(ink::tz::lookup(c.zone), r));
    const ink::tz::Local l = ink::tz::to_local(r, c.epoch);
    const int64_t nxt = ink::tz::next_top_of_hour(r, c.epoch);
    if (l.day != c.day || l.hh != c.hh || l.mm != c.mm || l.offset != c.offset || nxt != c.next_top) {
      if (failed++ < 10)
        printf("%s @%lld: got day %d %02d:%02d off %d next %lld, want day %d %02d:%02d off %d next %lld\n", c.zone,
               (long long)c.epoch, (int)l.day, l.hh, l.mm, (int)l.offset, (long long)nxt, (int)c.day, c.hh, c.mm,
               (int)c.offset, (long long)c.next_top);
    }
  }
  TEST_ASSERT_EQUAL_INT(0, failed);
}

void test_next_refresh_seconds_clamps() {
  ink::tz::Rule r;
  ink::tz::parse("UTC0", r);
  const int64_t ten_oclock = 1790503200;  // 2026-09-27 10:00:00 UTC
  TEST_ASSERT_EQUAL_INT32(3660, ink::tz::next_refresh_seconds(r, ten_oclock));       // 11:01
  TEST_ASSERT_EQUAL_INT32(300, ink::tz::next_refresh_seconds(r, ten_oclock + 3590));  // 10:59:50 -> 11:01 is 70 s, clamped
}

static std::string qerr(const char* q) {
  ink::Layout l;
  char e[192] = "";
  TEST_ASSERT_FALSE_MESSAGE(ink::parse_query(q, l, e, sizeof e), q);
  return e;
}
static ink::Layout qok(const char* q) {
  ink::Layout l;
  char e[192] = "";
  TEST_ASSERT_TRUE_MESSAGE(ink::parse_query(q, l, e, sizeof e), e);
  return l;
}

void test_query_default_layout() {
  ink::Layout l = qok("w=market_trends:2/3,calendar_weather:1/3&lat=34.05&lon=-118.24&tz=America/Los_Angeles&units=imperial");
  TEST_ASSERT_EQUAL_UINT8(2, l.n_columns);
  TEST_ASSERT_TRUE(l.columns[0].type == ink::WidgetType::MarketTrends && l.columns[0].size == ink::Size::TwoThirds);
  TEST_ASSERT_TRUE(l.columns[1].type == ink::WidgetType::CalendarWeather && l.columns[1].size == ink::Size::Third);
  TEST_ASSERT_EQUAL_DOUBLE(34.0, l.lat);      // f"{34.05:.1f}" == "34.0"
  TEST_ASSERT_EQUAL_DOUBLE(-118.2, l.lon);
  TEST_ASSERT_FALSE(l.metric);
  TEST_ASSERT_EQUAL_INT(5, l.years);
  TEST_ASSERT_EQUAL_UINT8(4, l.n_series);
  TEST_ASSERT_EQUAL_STRING("America/Los_Angeles", l.tz);
  TEST_ASSERT_TRUE(l.has_market && l.has_weather);
}

void test_query_market_only_needs_no_location() {
  ink::Layout l = qok("w=market_trends:1&tz=America/Los_Angeles&series=ust10y,usd_broad&years=10");
  TEST_ASSERT_FALSE(l.has_weather);
  TEST_ASSERT_EQUAL_UINT8(2, l.n_series);
  TEST_ASSERT_EQUAL_UINT8(4, l.series[0]);
  TEST_ASSERT_EQUAL_INT(10, l.years);
  TEST_ASSERT_EQUAL_STRING("UTC", qok("w=market_trends:1").tz);
}

void test_query_url_encoding_and_negative_zero() {
  ink::Layout a = qok("w=calendar_weather%3A1&lat=-0.04&lon=1");
  TEST_ASSERT_TRUE(a.columns[0].type == ink::WidgetType::CalendarWeather);
  TEST_ASSERT_FALSE(signbit(a.lat));          // -0.0 -> 0.0, as "+ 0.0" does in params.py
}

void test_query_errors_match_server() {
  TEST_ASSERT_EQUAL_STRING("w: sizes add up to 4/3, need 3/3", qerr("w=market_trends:2/3,market_trends:2/3").c_str());
  TEST_ASSERT_EQUAL_STRING("w: sizes add up to 2/3, need 3/3", qerr("w=market_trends:2/3").c_str());
  TEST_ASSERT_EQUAL_STRING("w: required, e.g. w=market_trends:2/3,calendar_weather:1/3", qerr("lat=1").c_str());
  TEST_ASSERT_EQUAL_STRING("w: unknown widget 'nope'", qerr("w=nope:1").c_str());
  TEST_ASSERT_EQUAL_STRING("w: expected type:size, got 'market_trends'", qerr("w=market_trends").c_str());
  TEST_ASSERT_EQUAL_STRING("w: size must be 1/3, 2/3 or 1, got '1/2'", qerr("w=market_trends:1/2").c_str());
  TEST_ASSERT_EQUAL_STRING("w: at most 3 widgets",
                           qerr("w=market_trends:1/3,market_trends:1/3,market_trends:1/3,market_trends:1/3").c_str());
  TEST_ASSERT_EQUAL_STRING("w: given more than once", qerr("w=market_trends:1&w=market_trends:1").c_str());
  TEST_ASSERT_EQUAL_STRING("foo: unknown parameter", qerr("w=market_trends:1&foo=2").c_str());
  TEST_ASSERT_EQUAL_STRING("lat: not used by any widget in w", qerr("w=market_trends:1&lat=1").c_str());
  TEST_ASSERT_EQUAL_STRING("lat: required by calendar_weather", qerr("w=calendar_weather:1&lon=1").c_str());
  TEST_ASSERT_EQUAL_STRING("lat: must be a number from -90 to 90", qerr("w=calendar_weather:1&lat=91&lon=1").c_str());
  TEST_ASSERT_EQUAL_STRING("lat: must be a number from -90 to 90", qerr("w=calendar_weather:1&lat=nan&lon=1").c_str());
  TEST_ASSERT_EQUAL_STRING("lat: could not convert string to float: 'abc'", qerr("w=calendar_weather:1&lat=abc&lon=1").c_str());
  TEST_ASSERT_EQUAL_STRING("units: must be one of imperial, metric", qerr("w=calendar_weather:1&lat=1&lon=1&units=kelvin").c_str());
  TEST_ASSERT_EQUAL_STRING("years: must be a whole number from 1 to 10", qerr("w=market_trends:1&years=11").c_str());
  TEST_ASSERT_EQUAL_STRING("series: ids must be unique", qerr("w=market_trends:1&series=sp500,sp500").c_str());
  TEST_ASSERT_EQUAL_STRING("series: give 1 to 4 ids", qerr("w=market_trends:1&series=sp500,btc,mortgage30,home_la,ust10y").c_str());
  TEST_ASSERT_EQUAL_STRING("series: unknown id 'foo'; choose from sp500, btc, mortgage30, home_la, ust10y, usd_broad",
                           qerr("w=market_trends:1&series=foo").c_str());
  TEST_ASSERT_EQUAL_STRING("tz: unknown timezone 'Nope/Zone'", qerr("w=market_trends:1&tz=Nope/Zone").c_str());
  TEST_ASSERT_EQUAL_STRING("tz: unknown timezone 'America'", qerr("w=market_trends:1&tz=America").c_str());
  TEST_ASSERT_EQUAL_STRING("malformed query string", qerr("w=market_trends:1&").c_str());
  TEST_ASSERT_EQUAL_STRING("malformed query string", qerr("w=market_trends:1&w=market_trends:1&bad").c_str());
  TEST_ASSERT_EQUAL_STRING("malformed query string", qerr("w=market_trends:1%00junk").c_str());
  TEST_ASSERT_EQUAL_STRING("malformed query string", qerr("w=calendar_weather:1&lat=1&lon=1&units=metric%00x").c_str());
  std::string longq = "w=market_trends:1&" + std::string(1100, 'x');
  TEST_ASSERT_EQUAL_STRING("query longer than 1024 bytes", qerr(longq.c_str()).c_str());
}

void test_series_formats() {
  char b[32];
  ink::format_value(ink::CATALOG[0].fmt, 7743.21, b, sizeof b); TEST_ASSERT_EQUAL_STRING("7,743", b);
  ink::format_value(ink::CATALOG[1].fmt, 84612.0, b, sizeof b); TEST_ASSERT_EQUAL_STRING("$84.6k", b);
  ink::format_value(ink::CATALOG[2].fmt, 7.03, b, sizeof b);    TEST_ASSERT_EQUAL_STRING("7.03%", b);
  ink::format_value(ink::CATALOG[3].fmt, 1050000.0, b, sizeof b); TEST_ASSERT_EQUAL_STRING("$1.05M", b);
  ink::format_value(ink::CATALOG[5].fmt, 121.44, b, sizeof b);  TEST_ASSERT_EQUAL_STRING("121.4", b);
  TEST_ASSERT_EQUAL_INT(3, ink::find_series("home_la", 7));
  TEST_ASSERT_EQUAL_INT(-1, ink::find_series("home", 4));
}

struct Recorder : ink::json::Handler {
  std::string log;
  bool last_truncated = false;
  bool last_closed_array = false;
  void scalar(const ink::json::Parser& p, ink::json::Type t, const char* text) override {
    for (int i = 0; i < p.depth(); ++i) {
      if (p.is_array(i)) log += "[" + std::to_string(p.index(i)) + "]";
      else log += std::string(".") + p.key(i);
    }
    log += "=" + std::to_string(static_cast<int>(t)) + ":" + text + ";";
    last_truncated = p.truncated();
  }
  void end_container(const ink::json::Parser& p) override {
    log += "end@" + std::to_string(p.depth()) + ";";
    last_closed_array = p.closed_array();
  }
};

static std::string parse_all(const std::string& doc, size_t chunk, bool* ok = nullptr) {
  Recorder r;
  ink::json::Parser p(r);
  for (size_t i = 0; i < doc.size(); i += chunk) p.feed(doc.data() + i, std::min(chunk, doc.size() - i));
  const bool done = p.finish();
  if (ok) *ok = done;
  return r.log;
}

void test_json_paths_and_types() {
  bool ok;
  std::string log = parse_all(R"({"a":[1,{"b":"x\"y"},true,null],"c":-2.5e3})", 1000, &ok);
  TEST_ASSERT_TRUE(ok);
  TEST_ASSERT_EQUAL_STRING(".a[0]=1:1;.a[1].b=0:x\"y;end@2;.a[2]=2:true;.a[3]=4:null;end@1;.c=1:-2.5e3;end@0;", log.c_str());
}

void test_json_any_chunk_split_gives_same_events() {
  const std::string doc = R"( {"observations":[{"date":"2026-09-25","value":"6604.72"},{"date":"2026-09-26","value":"."}],
    "u":"\u00b0F \ud83d\ude00", "e":[], "o":{}} )";
  bool ok;
  const std::string whole = parse_all(doc, doc.size(), &ok);
  TEST_ASSERT_TRUE(ok);
  for (size_t chunk = 1; chunk < 9; ++chunk) TEST_ASSERT_EQUAL_STRING(whole.c_str(), parse_all(doc, chunk).c_str());
  TEST_ASSERT_NOT_EQUAL(std::string::npos, whole.find(".u=0:\xC2\xB0" "F \xF0\x9F\x98\x80;"));
}

void test_json_rejects_malformed() {
  const char* bad[] = {"{", "[1,]", "{\"a\" 1}", "{\"a\":1,}", "[1 2]", "\"abc", "tru", "01", "1.", "-", "[1]]",
                       "{\"a\":\"\\x\"}", "[\"\x01\"]", "{1:2}", "[[[[[[[[[1]]]]]]]]]", "\"\\ud83d\""};
  for (const char* b : bad) {
    bool ok = true;
    parse_all(b, 3, &ok);
    TEST_ASSERT_FALSE_MESSAGE(ok, b);
  }
  bool ok = false;
  parse_all("  42  ", 1, &ok);
  TEST_ASSERT_TRUE(ok);
}

void test_json_long_strings_truncate_long_keys_fail() {
  Recorder r;
  ink::json::Parser p(r);
  std::string doc = "{\"m\":\"" + std::string(300, 'z') + "\"}";
  p.feed(doc.data(), doc.size());
  TEST_ASSERT_TRUE(p.finish());
  std::string expected = ".m=0:" + std::string(127, 'z') + ";end@0;";
  TEST_ASSERT_EQUAL_STRING(expected.c_str(), r.log.c_str());
  TEST_ASSERT_TRUE(r.last_truncated);
  bool ok = true;
  parse_all("{\"" + std::string(40, 'k') + "\":1}", 7, &ok);
  TEST_ASSERT_FALSE(ok);
}

void test_json_at_matches_paths() {
  struct H : ink::json::Handler {
    int hits = 0;
    void scalar(const ink::json::Parser& p, ink::json::Type, const char*) override {
      hits += p.at({"observations", nullptr, "date"});
    }
  } h;
  ink::json::Parser p(h);
  const char* doc = R"({"observations":[{"date":"a"},{"value":"b","date":"c"}],"date":"x"})";
  p.feed(doc, strlen(doc));
  TEST_ASSERT_TRUE(p.finish());
  TEST_ASSERT_EQUAL_INT(2, h.hits);
}

void test_json_boundary_key_length() {
  // Exactly 31 chars: accepted
  bool ok = true;
  parse_all("{\"" + std::string(31, 'k') + "\":1}", 100, &ok);
  TEST_ASSERT_TRUE(ok);
  // Exactly 32 chars: rejected
  ok = true;
  parse_all("{\"" + std::string(32, 'k') + "\":1}", 100, &ok);
  TEST_ASSERT_FALSE(ok);
}

void test_json_boundary_string_length() {
  // Exactly 127 chars: truncated() == false
  Recorder r;
  ink::json::Parser p(r);
  std::string doc = "{\"s\":\"" + std::string(127, 'x') + "\"}";
  p.feed(doc.data(), doc.size());
  TEST_ASSERT_TRUE(p.finish());
  TEST_ASSERT_FALSE(r.last_truncated);
  // Exactly 128 chars: truncated() == true
  Recorder r2;
  ink::json::Parser p2(r2);
  std::string doc2 = "{\"s\":\"" + std::string(128, 'x') + "\"}";
  p2.feed(doc2.data(), doc2.size());
  TEST_ASSERT_TRUE(p2.finish());
  TEST_ASSERT_TRUE(r2.last_truncated);
}

void test_json_boundary_number_length() {
  // Exactly 63 chars: accepted
  bool ok = true;
  parse_all(std::string(63, '9'), 100, &ok);
  TEST_ASSERT_TRUE(ok);
  // Exactly 64 chars: rejected
  ok = true;
  parse_all(std::string(64, '9'), 100, &ok);
  TEST_ASSERT_FALSE(ok);
}

void test_json_boundary_nesting_depth() {
  // Exactly 8 deep: accepted
  std::string doc = "";
  for (int i = 0; i < 8; ++i) doc += "[";
  doc += "1";
  for (int i = 0; i < 8; ++i) doc += "]";
  bool ok = true;
  parse_all(doc, 100, &ok);
  TEST_ASSERT_TRUE(ok);
}

void test_json_closed_array_vs_object() {
  struct H : ink::json::Handler {
    std::string log;
    void scalar(const ink::json::Parser&, ink::json::Type, const char*) override {}
    void end_container(const ink::json::Parser& p) override {
      log += p.closed_array() ? "a" : "o";
    }
  } h;
  ink::json::Parser p(h);
  const char* doc = R"([{},[]])";  // outer array, then object, then inner array
  p.feed(doc, strlen(doc));
  p.finish();
  // Record: object closes (o), inner array closes (a), outer array closes (a)
  TEST_ASSERT_EQUAL_STRING("oaa", h.log.c_str());
}

void test_json_rejects_escaped_nul() {
  bool ok = true;
  parse_all("{\"a\":\"x\\u0000y\"}", 10, &ok);
  TEST_ASSERT_FALSE(ok);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_test_dir_is_absolute_and_readable);
  RUN_TEST(test_civil_roundtrip_and_weekday);
  RUN_TEST(test_floor_div_mod);
  RUN_TEST(test_parse_iso_date);
  RUN_TEST(test_first_sunday);
  RUN_TEST(test_format_python_compatible);
  RUN_TEST(test_tz_lookup);
  RUN_TEST(test_tz_every_table_rule_parses);
  RUN_TEST(test_tz_matches_zoneinfo);
  RUN_TEST(test_next_refresh_seconds_clamps);
  RUN_TEST(test_query_default_layout);
  RUN_TEST(test_query_market_only_needs_no_location);
  RUN_TEST(test_query_url_encoding_and_negative_zero);
  RUN_TEST(test_query_errors_match_server);
  RUN_TEST(test_series_formats);
  RUN_TEST(test_json_paths_and_types);
  RUN_TEST(test_json_any_chunk_split_gives_same_events);
  RUN_TEST(test_json_rejects_malformed);
  RUN_TEST(test_json_long_strings_truncate_long_keys_fail);
  RUN_TEST(test_json_at_matches_paths);
  RUN_TEST(test_json_boundary_key_length);
  RUN_TEST(test_json_boundary_string_length);
  RUN_TEST(test_json_boundary_number_length);
  RUN_TEST(test_json_boundary_nesting_depth);
  RUN_TEST(test_json_closed_array_vs_object);
  RUN_TEST(test_json_rejects_escaped_nul);
  return UNITY_END();
}
