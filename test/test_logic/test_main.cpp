#include <unity.h>

#include <map>
#include <string>
#include <vector>

#include "../fixtures/fixtures.h"
#include "../support/ink_test.h"
#include "cycle.h"
#include "http_time.h"
#include "wake_logic.h"

using namespace wake;

static Work g_work;  // shared by the cycle tests: a fresh boot before each test

void setUp() { g_work = Work{}; }
void tearDown() {}

static const char* kQuery =
    "w=market_trends:2/3,calendar_weather:1/3&lat=34.1&lon=-118.2&tz=America/Los_Angeles&units=imperial";

// Hardware stand-in: serves the recorded fixtures and records what the cycle did.
struct FakeOps {
  bool fs_ok = true, wifi_ok = true, sntp_ok = true, fred_bad_key = false, save_ok = true;
  int64_t clock = FIXTURE_NOW, sntp_time = FIXTURE_NOW;
  std::map<std::string, std::vector<uint8_t>> files;
  std::vector<std::string> gets;  // "host series-or-weather", never the path (it has the key)
  int shows = 0, wifi_ups = 0;
  uint32_t t = 0;

  bool store_begin() { return fs_ok; }
  bool load(const char* name, uint8_t* buf, size_t cap, size_t& len) {
    auto it = files.find(name);
    if (it == files.end() || it->second.size() > cap) return false;
    memcpy(buf, it->second.data(), it->second.size());
    len = it->second.size();
    return true;
  }
  bool save(const char* name, const uint8_t* d, size_t n) {
    if (!save_ok) return false;
    files[name].assign(d, d + n);
    return true;
  }
  void prune(const char* const* keep, int n) {
    for (auto it = files.begin(); it != files.end();) {
      bool k = false;
      for (int i = 0; i < n; ++i) k = k || it->first == keep[i];
      it = k ? std::next(it) : files.erase(it);
    }
  }
  bool wifi_up(uint32_t) {
    ++wifi_ups;
    t += 2000;
    return wifi_ok;
  }
  void wifi_off() {}
  bool sntp(uint32_t) {
    t += 100;
    if (sntp_ok) clock = sntp_time;
    return sntp_ok;
  }
  void set_clock(int64_t e) { clock = e; }
  int64_t now() { return clock; }
  uint32_t ms() { return t; }
  ink::FetchResult get(const char* host, const char* path, ink::json::Handler& h, uint32_t) {
    t += 500;
    ink::FetchResult r;
    r.date_epoch = sntp_time;
    std::string file;
    if (strcmp(host, ink::OPENMETEO_HOST) == 0) {
      gets.push_back("weather");
      file = "fixtures/weather_la.json";
    } else {
      const char* id = strstr(path, "series_id=") + 10;
      const std::string sid(id, strcspn(id, "&"));
      gets.push_back("fred " + sid);
      if (fred_bad_key) {
        file = "fixtures/fred_error_bad_key.json";
        r.http_status = 400;
      } else {
        const char* start = strstr(path, "observation_start=") + 18;
        const bool tail = ink::parse_iso_date(std::string(start, 10).c_str()) == FIXTURE_TAIL_S0 + 1;
        file = "fixtures/fred_" + sid + (tail ? "_tail.json" : "_full.json");
      }
    }
    if (r.http_status < 0) r.http_status = 200;
    std::string doc;
    TEST_ASSERT_TRUE(ink_test::read_file(ink_test::path(file.c_str()), doc));
    ink::json::Parser p(h);
    p.feed(doc.data(), doc.size());
    r.complete = p.finish();
    return r;
  }
  void close_connections() {}
  void show(const uint8_t*) { ++shows; }
  const char* fred_api_key() { return "TESTKEY"; }
  void log(const char*) {}
};

static Rtc cold() {
  Rtc r{};
  rtc_begin(r, false);
  return r;
}

static int32_t cycle(FakeOps& ops, Rtc& rtc, const char* q = kQuery) {
  return run_cycle(ops, rtc, g_work, q, "fw test", false, 3600);
}

void test_parse_seconds_and_backoff() {
  TEST_ASSERT_EQUAL_INT32(3660, parse_seconds("3660"));
  TEST_ASSERT_EQUAL_INT32(-1, parse_seconds("12abc"));
  TEST_ASSERT_EQUAL_INT32(300, backoff_seconds(1, -1));
  TEST_ASSERT_EQUAL_INT32(900, backoff_seconds(2, -1));
  TEST_ASSERT_EQUAL_INT32(3600, backoff_seconds(9, -1));
  TEST_ASSERT_EQUAL_INT32(1800, backoff_seconds(1, 1800));
  TEST_ASSERT_EQUAL_INT64(1790537945, httptime::parse_http_date("Sun, 27 Sep 2026 19:39:05 GMT"));
  TEST_ASSERT_EQUAL_INT64(-1, httptime::parse_http_date("garbage"));
}

void test_rtc_begin() {
  Rtc r{};
  rtc_begin(r, true);
  TEST_ASSERT_EQUAL_UINT32(RTC_MAGIC, r.magic);
  TEST_ASSERT_TRUE(r.panel_dirty);
  TEST_ASSERT_FALSE(r.clock_valid);
  r.clock_valid = true;
  rtc_begin(r, true);
  TEST_ASSERT_TRUE(r.clock_valid);
  rtc_begin(r, false);
  TEST_ASSERT_FALSE(r.clock_valid);
}

void test_idle_step() {
  TEST_ASSERT_TRUE(idle_step(false, 0) == Idle::RunNow);
  TEST_ASSERT_TRUE(idle_step(true, 5000) == Idle::Wait);
  TEST_ASSERT_TRUE(idle_step(false, 5000) == Idle::DeepSleep);
  TEST_ASSERT_EQUAL_INT32(1, remaining_sleep_s(400));
  TEST_ASSERT_EQUAL_INT32(5, remaining_sleep_s(4001));
}

void test_cold_boot_fetches_renders_shows() {
  FakeOps ops;
  ops.clock = 0;  // garbage before SNTP
  Rtc rtc = cold();
  const int32_t s = cycle(ops, rtc);
  TEST_ASSERT_EQUAL_INT(1, ops.wifi_ups);
  TEST_ASSERT_EQUAL_size_t(5, ops.gets.size());   // weather + 4 FRED full fetches
  TEST_ASSERT_EQUAL_STRING("weather", ops.gets[0].c_str());
  TEST_ASSERT_EQUAL_INT(1, ops.shows);
  TEST_ASSERT_TRUE(rtc.clock_valid);
  TEST_ASSERT_FALSE(rtc.panel_dirty);
  TEST_ASSERT_EQUAL_size_t(5, ops.files.size());  // weather.bin + fred_<ID>.bin x4
  TEST_ASSERT_EQUAL_INT32(3660, s);               // 10:00 -> 11:01
}

void test_nothing_due_skips_wifi_and_unchanged_frame_is_not_shown() {
  FakeOps ops;
  Rtc rtc = cold();
  cycle(ops, rtc);
  ops.clock += 600;  // 10:10, before any TTL; e.g. an early wake while on USB
  ops.sntp_time = ops.clock;
  const int32_t s = cycle(ops, rtc);
  TEST_ASSERT_EQUAL_INT(1, ops.wifi_ups);
  TEST_ASSERT_EQUAL_INT(1, ops.shows);
  TEST_ASSERT_EQUAL_INT32(3060, s);
}

void test_hourly_wake_refreshes_weather_only() {
  FakeOps ops;
  Rtc rtc = cold();
  cycle(ops, rtc);
  ops.gets.clear();
  ops.clock += 3660;
  ops.sntp_time = ops.clock;
  cycle(ops, rtc);
  TEST_ASSERT_EQUAL_size_t(1, ops.gets.size());
  TEST_ASSERT_EQUAL_STRING("weather", ops.gets[0].c_str());
  TEST_ASSERT_EQUAL_INT(2, ops.shows);  // footer moved to 11:01 AM
}

void test_offline_keeps_frame_until_stale() {
  FakeOps ops;
  Rtc rtc = cold();
  cycle(ops, rtc);
  ops.wifi_ok = false;
  ops.clock += 3600;  // offline 1 h: same frame
  TEST_ASSERT_EQUAL_INT32(300, cycle(ops, rtc));
  TEST_ASSERT_EQUAL_INT(1, ops.shows);
  TEST_ASSERT_EQUAL_UINT8(1, rtc.fail_count);
  ops.clock += 3700;  // 2 h 01 min since the last weather: past TTL + 90 min -> "stale" footer
  TEST_ASSERT_EQUAL_INT32(900, cycle(ops, rtc));
  TEST_ASSERT_EQUAL_INT(2, ops.shows);
  ops.wifi_ok = true;
  ops.sntp_time = ops.clock;
  cycle(ops, rtc);
  TEST_ASSERT_EQUAL_UINT8(0, rtc.fail_count);
}

void test_cold_boot_offline_leaves_panel_alone() {
  FakeOps ops;
  ops.wifi_ok = false;
  Rtc rtc = cold();
  TEST_ASSERT_EQUAL_INT32(300, cycle(ops, rtc));
  TEST_ASSERT_EQUAL_INT(0, ops.shows);
  TEST_ASSERT_TRUE(rtc.panel_dirty);
}

void test_sntp_fails_date_header_sets_clock() {
  FakeOps ops;
  ops.sntp_ok = false;
  ops.clock = 0;
  Rtc rtc = cold();
  cycle(ops, rtc);
  TEST_ASSERT_TRUE(rtc.clock_valid);
  TEST_ASSERT_EQUAL_INT64(FIXTURE_NOW, ops.clock);
  TEST_ASSERT_EQUAL_INT(1, ops.shows);
}

void test_fred_auth_rejected_stops_after_first() {
  FakeOps ops;
  ops.fred_bad_key = true;
  Rtc rtc = cold();
  const int32_t s = cycle(ops, rtc);
  TEST_ASSERT_EQUAL_size_t(2, ops.gets.size());  // weather + one FRED request
  TEST_ASSERT_EQUAL_INT(1, ops.shows);
  TEST_ASSERT_EQUAL_INT32(3660, s);               // weather worked: not a network failure
  for (int i = 0; i < 4; ++i) TEST_ASSERT_TRUE(g_work.model.series[i].status.auth_rejected);
}

void test_bad_query_shows_error_once() {
  FakeOps ops;
  Rtc rtc = cold();
  TEST_ASSERT_EQUAL_INT32(3600, cycle(ops, rtc, "w=bad"));
  TEST_ASSERT_EQUAL_INT32(3600, cycle(ops, rtc, "w=bad"));
  TEST_ASSERT_EQUAL_INT(1, ops.shows);
  TEST_ASSERT_EQUAL_INT(0, ops.wifi_ups);
}

void test_cycle_uses_one_now_after_network() {
  FakeOps ops;
  Rtc rtc = cold();
  rtc.clock_valid = true;
  ops.clock = FIXTURE_NOW;                          // the RTC says 10:00
  ops.sntp_time = FIXTURE_NOW + 14 * 3600 + 5;      // SNTP says 00:00:05 the next day
  cycle(ops, rtc);
  TEST_ASSERT_EQUAL_INT64(ops.sntp_time, g_work.model.now);
}

void test_unconfigured_series_files_removed() {
  FakeOps ops;
  ops.files["fred_DGS10.bin"] = {1, 2, 3};
  ops.files["frame.bin"] = {1};
  ops.files["etag.txt"] = {1};
  Rtc rtc = cold();
  cycle(ops, rtc);
  TEST_ASSERT_EQUAL_size_t(0, ops.files.count("fred_DGS10.bin"));
  TEST_ASSERT_EQUAL_size_t(0, ops.files.count("frame.bin"));
  TEST_ASSERT_EQUAL_size_t(0, ops.files.count("etag.txt"));
  TEST_ASSERT_EQUAL_size_t(1, ops.files.count("fred_SP500.bin"));
}

void test_save_failure_still_renders() {
  FakeOps ops;
  ops.save_ok = false;
  Rtc rtc = cold();
  TEST_ASSERT_EQUAL_INT32(3660, cycle(ops, rtc));
  TEST_ASSERT_EQUAL_INT(1, ops.shows);
}

void test_no_filesystem_runs_from_ram() {
  FakeOps ops;
  ops.fs_ok = false;
  Rtc rtc = cold();
  cycle(ops, rtc);
  TEST_ASSERT_EQUAL_INT(1, ops.shows);
  TEST_ASSERT_EQUAL_size_t(0, ops.files.size());
  ops.wifi_ok = false;                // still awake (USB): the next cycle keeps the RAM data
  ops.clock += 3600;
  cycle(ops, rtc);
  TEST_ASSERT_TRUE(g_work.model.weather.valid);
  TEST_ASSERT_EQUAL_INT(1, ops.shows);  // same frame, not "No data yet"
}

void test_dirty_panel_redraws_same_frame() {
  FakeOps ops;
  Rtc rtc = cold();
  cycle(ops, rtc);
  rtc.panel_dirty = true;  // a reset interrupted the last refresh
  ops.clock += 60;
  cycle(ops, rtc);
  TEST_ASSERT_EQUAL_INT(2, ops.shows);
}

void test_cached_data_survives_reboot() {
  FakeOps ops;
  Rtc rtc = cold();
  cycle(ops, rtc);
  g_work = Work{};                    // RAM lost in deep sleep; files remain
  ops.gets.clear();
  ops.clock += 600;
  cycle(ops, rtc);
  TEST_ASSERT_EQUAL_size_t(0, ops.gets.size());   // everything fresh from flash
  TEST_ASSERT_EQUAL_INT(1, ops.shows);            // identical frame
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_parse_seconds_and_backoff);
  RUN_TEST(test_rtc_begin);
  RUN_TEST(test_idle_step);
  RUN_TEST(test_cold_boot_fetches_renders_shows);
  RUN_TEST(test_nothing_due_skips_wifi_and_unchanged_frame_is_not_shown);
  RUN_TEST(test_hourly_wake_refreshes_weather_only);
  RUN_TEST(test_offline_keeps_frame_until_stale);
  RUN_TEST(test_cold_boot_offline_leaves_panel_alone);
  RUN_TEST(test_sntp_fails_date_header_sets_clock);
  RUN_TEST(test_fred_auth_rejected_stops_after_first);
  RUN_TEST(test_bad_query_shows_error_once);
  RUN_TEST(test_cycle_uses_one_now_after_network);
  RUN_TEST(test_unconfigured_series_files_removed);
  RUN_TEST(test_save_failure_still_renders);
  RUN_TEST(test_no_filesystem_runs_from_ram);
  RUN_TEST(test_dirty_panel_redraws_same_frame);
  RUN_TEST(test_cached_data_survives_reboot);
  return UNITY_END();
}
