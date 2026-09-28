#include <unity.h>

#include "wake_logic.h"
#include <string.h>

#include "http_time.h"
#include "badge.h"
#include "body_reader.h"
#include "cycle.h"

using namespace wake;

void setUp() {}
void tearDown() {}

static State S(uint8_t fails, bool badge, bool error, bool dirty = false) {
  return State{fails, badge, error, dirty};
}

void test_classify_200_needs_exact_length() {
  TEST_ASSERT_TRUE(classify(200, 48000) == Outcome::Frame200);
  TEST_ASSERT_TRUE(classify(200, 47999) == Outcome::Failure);   // truncated
  TEST_ASSERT_TRUE(classify(200, 5120) == Outcome::Failure);    // captive portal page
  TEST_ASSERT_TRUE(classify(304, 0) == Outcome::NotModified304);
  TEST_ASSERT_TRUE(classify(400, 40) == Outcome::BadRequest400);
  TEST_ASSERT_TRUE(classify(429, 12) == Outcome::Failure);
  TEST_ASSERT_TRUE(classify(530, 0) == Outcome::Failure);
  TEST_ASSERT_TRUE(classify(-1, 0) == Outcome::Failure);        // no connection
}

void test_parse_seconds_rejects_garbage() {
  TEST_ASSERT_EQUAL_INT32(3660, parse_seconds("3660"));
  TEST_ASSERT_EQUAL_INT32(0, parse_seconds("0"));
  TEST_ASSERT_EQUAL_INT32(-1, parse_seconds(""));
  TEST_ASSERT_EQUAL_INT32(-1, parse_seconds(nullptr));
  TEST_ASSERT_EQUAL_INT32(-1, parse_seconds("-5"));
  TEST_ASSERT_EQUAL_INT32(-1, parse_seconds("12abc"));
  TEST_ASSERT_EQUAL_INT32(-1, parse_seconds("99999999999"));    // overflow
  TEST_ASSERT_EQUAL_INT32(-25200, parse_signed_seconds("-25200"));
  TEST_ASSERT_EQUAL_INT32(INT32_MIN, parse_signed_seconds("x"));
}

void test_sleep_after_success() {
  TEST_ASSERT_EQUAL_INT32(3660, sleep_after_success(3660, 3600));
  TEST_ASSERT_EQUAL_INT32(3600, sleep_after_success(-1, 3600));  // header missing
  TEST_ASSERT_EQUAL_INT32(300, sleep_after_success(0, 3600));    // clamped up
  TEST_ASSERT_EQUAL_INT32(21600, sleep_after_success(999999, 3600));
}

void test_backoff() {
  TEST_ASSERT_EQUAL_INT32(300, backoff_seconds(1, -1));
  TEST_ASSERT_EQUAL_INT32(900, backoff_seconds(2, -1));
  TEST_ASSERT_EQUAL_INT32(3600, backoff_seconds(3, -1));
  TEST_ASSERT_EQUAL_INT32(3600, backoff_seconds(200, -1));
  TEST_ASSERT_EQUAL_INT32(1800, backoff_seconds(1, 1800));        // Retry-After wins when longer
  TEST_ASSERT_EQUAL_INT32(900, backoff_seconds(2, 60));
  TEST_ASSERT_EQUAL_INT32(21600, backoff_seconds(1, 999999));
}

void test_200_draws_and_saves_and_clears_flags() {
  Plan p = decide(Outcome::Frame200, S(5, true, true, true), true);
  TEST_ASSERT_TRUE(p.draw == Draw::NewFrame);
  TEST_ASSERT_FALSE(p.next.panel_dirty);
  TEST_ASSERT_TRUE(p.save_frame);
  TEST_ASSERT_FALSE(p.clear_etag);
  TEST_ASSERT_EQUAL_UINT8(0, p.next.fail_count);
  TEST_ASSERT_FALSE(p.next.showing_badge);
  TEST_ASSERT_FALSE(p.next.showing_error);
}

void test_304_leaves_panel_alone() {
  Plan p = decide(Outcome::NotModified304, S(2, false, false), true);
  TEST_ASSERT_TRUE(p.draw == Draw::Nothing);
  TEST_ASSERT_FALSE(p.save_frame);
  TEST_ASSERT_EQUAL_UINT8(0, p.next.fail_count);
}

void test_304_with_badge_restores_stored_frame() {
  Plan p = decide(Outcome::NotModified304, S(0, true, false), true);
  TEST_ASSERT_TRUE(p.draw == Draw::StoredFrame);
  TEST_ASSERT_FALSE(p.next.showing_badge);
}

void test_304_with_badge_but_no_stored_frame_forces_refetch() {
  Plan p = decide(Outcome::NotModified304, S(0, true, false), false);
  TEST_ASSERT_TRUE(p.draw == Draw::Nothing);
  TEST_ASSERT_TRUE(p.clear_etag);            // next wake gets a 200 and a clean frame
  TEST_ASSERT_TRUE(p.next.showing_badge);
}

void test_400_draws_error_and_clears_etag() {
  Plan p = decide(Outcome::BadRequest400, S(1, false, false), true);
  TEST_ASSERT_TRUE(p.draw == Draw::ErrorScreen);
  TEST_ASSERT_TRUE(p.clear_etag);
  TEST_ASSERT_TRUE(p.next.showing_error);
  TEST_ASSERT_EQUAL_UINT8(0, p.next.fail_count);
}

void test_repeated_400_draws_error_once() {
  Plan p = decide(Outcome::BadRequest400, S(0, false, true), true);
  TEST_ASSERT_TRUE(p.draw == Draw::Nothing);
  TEST_ASSERT_TRUE(p.next.showing_error);
}

void test_failures_saturate_and_badge_once() {
  State s = S(0, false, false);
  Draw draws[6];
  for (int i = 0; i < 6; i++) {
    Plan p = decide(Outcome::Failure, s, true);
    draws[i] = p.draw;
    s = p.next;
  }
  TEST_ASSERT_TRUE(draws[0] == Draw::Nothing);
  TEST_ASSERT_TRUE(draws[1] == Draw::Nothing);
  TEST_ASSERT_TRUE(draws[2] == Draw::StoredFrameWithBadge);  // third failure
  TEST_ASSERT_TRUE(draws[3] == Draw::Nothing);
  TEST_ASSERT_TRUE(draws[5] == Draw::Nothing);
  TEST_ASSERT_TRUE(s.showing_badge);
  s.fail_count = 255;
  TEST_ASSERT_EQUAL_UINT8(255, decide(Outcome::Failure, s, true).next.fail_count);
}

void test_failure_without_stored_frame_never_badges() {
  State s = S(2, false, false);
  Plan p = decide(Outcome::Failure, s, false);
  TEST_ASSERT_TRUE(p.draw == Draw::Nothing);
  TEST_ASSERT_FALSE(p.next.showing_badge);
}

void test_failure_keeps_error_screen() {
  Plan p = decide(Outcome::Failure, S(2, false, true), true);
  TEST_ASSERT_TRUE(p.draw == Draw::Nothing);                 // don't cover the config error
  TEST_ASSERT_TRUE(p.next.showing_error);
}

void test_cold_boot_304_restores_clean_frame() {
  Plan p = decide(Outcome::NotModified304, cold_boot_state(), true);
  TEST_ASSERT_TRUE(p.draw == Draw::StoredFrame);
  TEST_ASSERT_FALSE(p.next.panel_dirty);
}

void test_cold_boot_still_badges_after_three_failures() {
  State s = cold_boot_state();
  Draw last = Draw::Nothing;
  for (int i = 0; i < 3; i++) {
    Plan p = decide(Outcome::Failure, s, true);
    last = p.draw;
    s = p.next;
  }
  TEST_ASSERT_TRUE(last == Draw::StoredFrameWithBadge);
  TEST_ASSERT_FALSE(s.panel_dirty);
}

void test_sleep_for() {
  TEST_ASSERT_EQUAL_INT32(3660, sleep_for(Outcome::Frame200, 0, 3660, -1, 3600));
  TEST_ASSERT_EQUAL_INT32(3600, sleep_for(Outcome::NotModified304, 0, -1, -1, 3600));
  TEST_ASSERT_EQUAL_INT32(3600, sleep_for(Outcome::BadRequest400, 0, -1, -1, 3600));
  TEST_ASSERT_EQUAL_INT32(900, sleep_for(Outcome::Failure, 2, -1, -1, 3600));
}

void test_parse_http_date() {
  TEST_ASSERT_TRUE(httptime::parse_http_date("Sun, 27 Sep 2026 19:39:05 GMT") == 1790537945LL);
  TEST_ASSERT_TRUE(httptime::parse_http_date("Thu, 01 Jan 1970 00:00:00 GMT") == 0LL);
  TEST_ASSERT_TRUE(httptime::parse_http_date("Tue, 29 Feb 2028 12:00:00 GMT") == 1835438400LL);
  TEST_ASSERT_TRUE(httptime::parse_http_date("garbage") == -1);
  TEST_ASSERT_TRUE(httptime::parse_http_date("") == -1);
  TEST_ASSERT_TRUE(httptime::parse_http_date(nullptr) == -1);
  TEST_ASSERT_TRUE(httptime::parse_http_date("Sun, 27 Foo 2026 19:39:05 GMT") == -1);
}

void test_format_clock() {
  char out[9];
  httptime::format_clock(1790537945LL, -25200, out);   // 19:39 UTC-7 -> 12:39 PM
  TEST_ASSERT_EQUAL_STRING("12:39 PM", out);
  httptime::format_clock(1790537945LL, 0, out);
  TEST_ASSERT_EQUAL_STRING("7:39 PM", out);
  httptime::format_clock(1790537945LL - 19 * 3600 - 39 * 60 - 5, 0, out);   // midnight
  TEST_ASSERT_EQUAL_STRING("12:00 AM", out);
  httptime::format_clock(1790537945LL, 5 * 3600 + 1800, out);   // +5:30
  TEST_ASSERT_EQUAL_STRING("1:09 AM", out);
}

void test_badge_text_with_time() {
  char out[32];
  httptime::badge_text(out, sizeof out, 1790537945LL, -25200);
  TEST_ASSERT_EQUAL_STRING("offline since 12:39 PM", out);
}

void test_badge_text_without_time() {
  char out[32];
  httptime::badge_text(out, sizeof out, 0, -25200);
  TEST_ASSERT_EQUAL_STRING("offline", out);
}

static uint8_t g_frame[48000];

static void white_frame() { memset(g_frame, 0xFF, sizeof g_frame); }

void test_pixel_bit_layout() {
  white_frame();
  badge::set_px(g_frame, 0, 0, true);
  TEST_ASSERT_EQUAL_HEX8(0x7F, g_frame[0]);          // MSB = leftmost, 0 = black
  badge::set_px(g_frame, 9, 1, true);
  TEST_ASSERT_EQUAL_HEX8(0xBF, g_frame[101]);        // row 1 starts at byte 100
  TEST_ASSERT_TRUE(badge::is_black(g_frame, 9, 1));
  badge::set_px(g_frame, 800, 0, true);              // out of range: ignored
  badge::set_px(g_frame, -1, 5, true);
  TEST_ASSERT_EQUAL_HEX8(0xFF, g_frame[99]);
}

void test_text_width() {
  TEST_ASSERT_EQUAL_INT(11, badge::text_width("ab", 1));   // 2 x 6 px, minus trailing gap
  TEST_ASSERT_EQUAL_INT(22, badge::text_width("ab", 2));
  TEST_ASSERT_EQUAL_INT(0, badge::text_width("", 2));
}

void test_badge_box_bottom_right_and_contained() {
  white_frame();
  badge::Rect r = badge::draw_badge(g_frame, "offline since 12:39 PM");
  TEST_ASSERT_TRUE(r.x + r.w <= 800 && r.y + r.h <= 480 && r.x > 400 && r.y > 440);
  // Border is black on all four sides.
  TEST_ASSERT_TRUE(badge::is_black(g_frame, r.x, r.y));
  TEST_ASSERT_TRUE(badge::is_black(g_frame, r.x + r.w - 1, r.y + r.h - 1));
  TEST_ASSERT_TRUE(badge::is_black(g_frame, r.x + r.w / 2, r.y));
  // Text drew something inside; padding next to the border stays white.
  int inside = 0;
  for (int y = r.y + 2; y < r.y + r.h - 2; y++)
    for (int x = r.x + 2; x < r.x + r.w - 2; x++) inside += badge::is_black(g_frame, x, y);
  TEST_ASSERT_TRUE(inside > 100);
  TEST_ASSERT_FALSE(badge::is_black(g_frame, r.x + 2, r.y + 2));
  // Nothing outside the box changed.
  int outside = 0;
  for (int y = 0; y < 480; y++)
    for (int x = 0; x < 800; x++)
      if (x < r.x || x >= r.x + r.w || y < r.y || y >= r.y + r.h) outside += badge::is_black(g_frame, x, y);
  TEST_ASSERT_EQUAL_INT(0, outside);
}

void test_badge_overwrites_dark_background() {
  memset(g_frame, 0x00, sizeof g_frame);             // all black underneath
  badge::Rect r = badge::draw_badge(g_frame, "offline");
  TEST_ASSERT_FALSE(badge::is_black(g_frame, r.x + 2, r.y + 2));   // box interior cleared
}

void test_glyph_one() {
  white_frame();
  badge::draw_text(g_frame, 10, 10, "1", 1);
  // '1' row 0 = 0x04: only column 2 is set.
  TEST_ASSERT_FALSE(badge::is_black(g_frame, 11, 10));
  TEST_ASSERT_TRUE(badge::is_black(g_frame, 12, 10));
  TEST_ASSERT_FALSE(badge::is_black(g_frame, 13, 10));
  // row 6 = 0x0E: columns 1..3.
  TEST_ASSERT_TRUE(badge::is_black(g_frame, 11, 16));
  TEST_ASSERT_TRUE(badge::is_black(g_frame, 13, 16));
  TEST_ASSERT_FALSE(badge::is_black(g_frame, 14, 16));
}

// A stream whose bytes arrive at simulated times; the reader's idle() advances the clock.
static uint32_t g_now;

struct FakeStream {
  struct Chunk { uint32_t at; int32_t len; };
  Chunk chunks[4];
  int n = 0;
  uint32_t close_at = 0xFFFFFFFFu;  // never, unless set
  int32_t consumed = 0;
  void add(uint32_t at, int32_t len) { chunks[n++] = Chunk{at, len}; }
  int32_t arrived() const {
    int32_t a = 0;
    for (int i = 0; i < n; i++)
      if (chunks[i].at <= g_now) a += chunks[i].len;
    return a;
  }
  int available() { return static_cast<int>(arrived() - consumed); }
  int read(uint8_t* buf, size_t want) {
    int32_t k = static_cast<int32_t>(want) < available() ? static_cast<int32_t>(want) : available();
    for (int32_t i = 0; i < k; i++) buf[i] = static_cast<uint8_t>(consumed + i);
    consumed += k;
    return static_cast<int>(k);
  }
  bool connected() { return g_now < close_at || available() > 0; }
};

static uint8_t g_body[48000];

static int32_t read_body(FakeStream& s, int32_t content_length, uint32_t timeout_ms = 20000) {
  g_now = 0;
  return body::read_exact(s, g_body, 48000, content_length, timeout_ms,
                          [] { return g_now; }, [] { g_now += 5; });
}

void test_reader_declared_length_in_pieces() {
  FakeStream s;
  s.add(0, 20000);
  s.add(300, 28000);
  TEST_ASSERT_EQUAL_INT32(48000, read_body(s, 48000));
  TEST_ASSERT_EQUAL_HEX8(static_cast<uint8_t>(47999), g_body[47999]);
}

void test_reader_rejects_wrong_declared_length() {
  FakeStream s;
  s.add(0, 48001);
  TEST_ASSERT_EQUAL_INT32(-1, read_body(s, 48001));
  TEST_ASSERT_EQUAL_INT32(0, s.consumed);            // didn't even start reading
}

void test_reader_close_delimited_exact() {
  FakeStream s;
  s.add(0, 48000);
  s.close_at = 50;
  TEST_ASSERT_EQUAL_INT32(48000, read_body(s, -1));
}

void test_reader_rejects_delayed_trailing_bytes() {
  FakeStream s;
  s.add(0, 48000);
  s.add(500, 1);                                     // arrives after a pause
  s.close_at = 1000;
  TEST_ASSERT_EQUAL_INT32(-1, read_body(s, -1));
}

void test_reader_rejects_truncated_body() {
  FakeStream a;
  a.add(0, 47000);
  a.close_at = 100;
  TEST_ASSERT_EQUAL_INT32(-1, read_body(a, -1));
  FakeStream b;
  b.add(0, 47000);
  b.close_at = 100;
  TEST_ASSERT_EQUAL_INT32(-1, read_body(b, 48000));
}

void test_reader_times_out_when_body_never_ends() {
  FakeStream s;
  s.add(0, 48000);                                   // close-delimited, but never closes
  TEST_ASSERT_EQUAL_INT32(-1, read_body(s, -1, 2000));
  FakeStream slow;
  slow.add(0, 1000);
  slow.add(30000, 47000);                            // rest arrives after the timeout
  TEST_ASSERT_EQUAL_INT32(-1, read_body(slow, 48000, 20000));
}

void test_reader_declared_length_ignores_open_connection() {
  FakeStream s;
  s.add(0, 48000);                                   // keep-alive: never closes, length says done
  TEST_ASSERT_EQUAL_INT32(48000, read_body(s, 48000));
}

struct FakeOps {
  bool fs = true;
  bool stored = false;
  uint8_t disk[48000];
  char disk_etag[40] = "";
  wake::Fetched next;
  uint8_t body_fill = 0xFF;
  char log[128] = "";
  char etag_at_show[40] = "";
  uint8_t shown[48000];

  void ev(const char* e) { strcat(log, e); strcat(log, " "); }
  bool store_ok() { return fs; }
  void load_etag(char* out, size_t n) { strncpy(out, disk_etag, n - 1); out[n - 1] = '\0'; }
  bool has_frame() { return stored; }
  bool load_frame(uint8_t* b) {
    ev("load");
    if (!stored) return false;
    memcpy(b, disk, sizeof disk);
    return true;
  }
  bool save(const uint8_t* b, const char* e) {
    ev("save");
    memcpy(disk, b, sizeof disk);
    strcpy(disk_etag, e);
    stored = true;
    return true;
  }
  void clear_etag() { ev("clear"); disk_etag[0] = '\0'; }
  wake::Fetched fetch(const char*, uint8_t* buf) {
    ev("fetch");
    if (next.outcome == wake::Outcome::Frame200) memset(buf, body_fill, 48000);
    return next;
  }
  void show(const uint8_t* b) { ev("show"); strcpy(etag_at_show, disk_etag); memcpy(shown, b, sizeof shown); }
  void show_error(const char*) { ev("error"); }
};

static FakeOps g_ops;          // large buffers: keep them off the stack
static wake::Rtc g_rtc;
static uint8_t g_cycle_frame[48000];

static wake::Fetched F(wake::Outcome o, const char* etag = "") {
  wake::Fetched f;
  f.outcome = o;
  f.next_refresh_s = 3660;
  f.date_epoch = 1790537945LL;   // Sun, 27 Sep 2026 19:39:05 GMT
  f.utc_offset_s = -25200;
  strcpy(f.etag, etag);
  return f;
}

static int32_t cycle(wake::Fetched f) {
  g_ops.next = f;
  g_ops.log[0] = '\0';
  return wake::run_cycle(g_ops, g_rtc, g_cycle_frame, 3600);
}

static void fresh_board() {
  g_ops = FakeOps();
  g_rtc = wake::Rtc{};  // magic 0: cold boot
}

void test_cycle_saves_etag_only_after_showing() {
  fresh_board();
  g_ops.stored = true;
  strcpy(g_ops.disk_etag, "\"old\"");
  TEST_ASSERT_EQUAL_INT32(3660, cycle(F(wake::Outcome::Frame200, "\"new\"")));
  TEST_ASSERT_EQUAL_STRING("fetch show save ", g_ops.log);
  TEST_ASSERT_EQUAL_STRING("\"old\"", g_ops.etag_at_show);   // a power cut during show keeps the old tag
  TEST_ASSERT_EQUAL_STRING("\"new\"", g_ops.disk_etag);
}

void test_cycle_cold_boot_304_restores_stored_frame() {
  fresh_board();
  g_ops.stored = true;
  memset(g_ops.disk, 0xAA, sizeof g_ops.disk);
  strcpy(g_ops.disk_etag, "\"e\"");
  cycle(F(wake::Outcome::NotModified304));
  TEST_ASSERT_EQUAL_STRING("fetch load show ", g_ops.log);
  TEST_ASSERT_EQUAL_HEX8(0xAA, g_ops.shown[123]);
  cycle(F(wake::Outcome::NotModified304));                     // now known: leave it alone
  TEST_ASSERT_EQUAL_STRING("fetch ", g_ops.log);
}

void test_cycle_badge_on_third_failure_over_stored_frame() {
  fresh_board();
  cycle(F(wake::Outcome::Frame200, "\"a\""));                  // white frame shown and stored
  TEST_ASSERT_EQUAL_INT32(300, cycle(F(wake::Outcome::Failure)));
  TEST_ASSERT_EQUAL_STRING("fetch ", g_ops.log);
  TEST_ASSERT_EQUAL_INT32(900, cycle(F(wake::Outcome::Failure)));
  TEST_ASSERT_EQUAL_INT32(3600, cycle(F(wake::Outcome::Failure)));
  TEST_ASSERT_EQUAL_STRING("fetch load show ", g_ops.log);
  TEST_ASSERT_TRUE(badge::is_black(g_ops.shown, 795, 477));  // badge border, bottom-right
  TEST_ASSERT_FALSE(badge::is_black(g_ops.shown, 10, 10));   // rest of the stored frame
  TEST_ASSERT_EQUAL_HEX8(0xFF, g_ops.disk[47999]);            // flash copy stays clean
  cycle(F(wake::Outcome::NotModified304));                     // back online: clean frame again
  TEST_ASSERT_EQUAL_STRING("fetch load show ", g_ops.log);
  TEST_ASSERT_FALSE(badge::is_black(g_ops.shown, 795, 477));
}

void test_cycle_power_loss_with_badge_then_304_clears_it() {
  fresh_board();
  cycle(F(wake::Outcome::Frame200, "\"a\""));
  for (int i = 0; i < 3; i++) cycle(F(wake::Outcome::Failure));   // badge on the panel
  g_rtc = wake::Rtc{};                                              // power loss
  cycle(F(wake::Outcome::NotModified304));
  TEST_ASSERT_EQUAL_STRING("fetch load show ", g_ops.log);
  TEST_ASSERT_FALSE(badge::is_black(g_ops.shown, 795, 477));
}

void test_cycle_config_error_drawn_once() {
  fresh_board();
  cycle(F(wake::Outcome::Frame200, "\"a\""));
  TEST_ASSERT_EQUAL_INT32(3600, cycle(F(wake::Outcome::BadRequest400)));
  TEST_ASSERT_EQUAL_STRING("fetch clear error ", g_ops.log);
  cycle(F(wake::Outcome::BadRequest400));
  TEST_ASSERT_EQUAL_STRING("fetch clear ", g_ops.log);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_classify_200_needs_exact_length);
  RUN_TEST(test_parse_seconds_rejects_garbage);
  RUN_TEST(test_sleep_after_success);
  RUN_TEST(test_backoff);
  RUN_TEST(test_200_draws_and_saves_and_clears_flags);
  RUN_TEST(test_304_leaves_panel_alone);
  RUN_TEST(test_304_with_badge_restores_stored_frame);
  RUN_TEST(test_304_with_badge_but_no_stored_frame_forces_refetch);
  RUN_TEST(test_400_draws_error_and_clears_etag);
  RUN_TEST(test_repeated_400_draws_error_once);
  RUN_TEST(test_failures_saturate_and_badge_once);
  RUN_TEST(test_failure_without_stored_frame_never_badges);
  RUN_TEST(test_failure_keeps_error_screen);
  RUN_TEST(test_cold_boot_304_restores_clean_frame);
  RUN_TEST(test_cold_boot_still_badges_after_three_failures);
  RUN_TEST(test_sleep_for);
  RUN_TEST(test_parse_http_date);
  RUN_TEST(test_format_clock);
  RUN_TEST(test_badge_text_with_time);
  RUN_TEST(test_badge_text_without_time);
  RUN_TEST(test_pixel_bit_layout);
  RUN_TEST(test_text_width);
  RUN_TEST(test_badge_box_bottom_right_and_contained);
  RUN_TEST(test_badge_overwrites_dark_background);
  RUN_TEST(test_glyph_one);
  RUN_TEST(test_reader_declared_length_in_pieces);
  RUN_TEST(test_reader_rejects_wrong_declared_length);
  RUN_TEST(test_reader_close_delimited_exact);
  RUN_TEST(test_reader_rejects_delayed_trailing_bytes);
  RUN_TEST(test_reader_rejects_truncated_body);
  RUN_TEST(test_reader_times_out_when_body_never_ends);
  RUN_TEST(test_reader_declared_length_ignores_open_connection);
  RUN_TEST(test_cycle_saves_etag_only_after_showing);
  RUN_TEST(test_cycle_cold_boot_304_restores_stored_frame);
  RUN_TEST(test_cycle_badge_on_third_failure_over_stored_frame);
  RUN_TEST(test_cycle_power_loss_with_badge_then_304_clears_it);
  RUN_TEST(test_cycle_config_error_drawn_once);
  return UNITY_END();
}
