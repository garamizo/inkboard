#include <unity.h>

#include "wake_logic.h"

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
  return UNITY_END();
}
