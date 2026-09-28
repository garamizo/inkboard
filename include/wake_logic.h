#pragma once
// Wake-cycle decisions (spec §6.2-6.3). Pure: no Arduino, host-tested in test/test_logic.
#include <stdint.h>

namespace wake {

constexpr int32_t FRAME_BYTES = 48000;
constexpr int32_t MIN_SLEEP_S = 300;
constexpr int32_t MAX_SLEEP_S = 21600;
constexpr uint8_t BADGE_AFTER_FAILURES = 3;

enum class Outcome : uint8_t { Frame200, NotModified304, BadRequest400, Failure };
enum class Draw : uint8_t { Nothing, NewFrame, StoredFrame, StoredFrameWithBadge, ErrorScreen };

// Survives deep sleep (RTC memory).
struct State {
  uint8_t fail_count;
  bool showing_badge;
  bool showing_error;
  bool panel_dirty;  // the panel's content is unknown (cold boot): restore on the next success
};

// After power loss the e-paper still shows whatever it last had (maybe the badge).
inline State cold_boot_state() { return State{0, false, false, true}; }

struct Plan {
  Draw draw;
  bool save_frame;  // write the new frame + ETag to flash
  bool clear_etag;  // forget the ETag so the next wake gets a full 200
  State next;
};

// http_status < 0 means no HTTP response at all. A 200 counts only with a whole frame:
// captive portals and truncated downloads also answer 200.
inline Outcome classify(int http_status, int32_t body_bytes) {
  if (http_status == 200) return body_bytes == FRAME_BYTES ? Outcome::Frame200 : Outcome::Failure;
  if (http_status == 304) return Outcome::NotModified304;
  if (http_status == 400) return Outcome::BadRequest400;
  return Outcome::Failure;
}

// Non-negative decimal integer header, or -1 (missing, garbage, negative, overflow).
inline int32_t parse_seconds(const char* s) {
  if (s == nullptr || *s == '\0') return -1;
  int64_t v = 0;
  for (; *s; ++s) {
    if (*s < '0' || *s > '9') return -1;
    v = v * 10 + (*s - '0');
    if (v > INT32_MAX) return -1;
  }
  return static_cast<int32_t>(v);
}

// Optionally signed integer (UTC offsets), or INT32_MIN if invalid.
inline int32_t parse_signed_seconds(const char* s) {
  if (s == nullptr) return INT32_MIN;
  bool neg = (*s == '-');
  int32_t v = parse_seconds(neg ? s + 1 : s);
  if (v < 0) return INT32_MIN;
  return neg ? -v : v;
}

inline int32_t clamp_sleep(int32_t s) {
  return s < MIN_SLEEP_S ? MIN_SLEEP_S : (s > MAX_SLEEP_S ? MAX_SLEEP_S : s);
}

inline int32_t sleep_after_success(int32_t header_s, int32_t fallback_s) {
  return clamp_sleep(header_s >= 0 ? header_s : fallback_s);
}

inline int32_t backoff_seconds(uint8_t fail_count, int32_t retry_after_s) {
  int32_t s = fail_count <= 1 ? 300 : (fail_count == 2 ? 900 : 3600);
  if (retry_after_s > s) s = retry_after_s;
  return clamp_sleep(s);
}

inline Plan decide(Outcome outcome, State s, bool frame_stored) {
  Plan p{Draw::Nothing, false, false, s};
  switch (outcome) {
    case Outcome::Frame200:
      p.draw = Draw::NewFrame;
      p.save_frame = true;
      p.next = State{0, false, false, false};
      break;
    case Outcome::NotModified304:
      p.next.fail_count = 0;
      if (s.showing_badge || s.showing_error || s.panel_dirty) {
        if (frame_stored) {
          p.draw = Draw::StoredFrame;  // put the clean frame back
          p.next.showing_badge = p.next.showing_error = p.next.panel_dirty = false;
        } else {
          p.clear_etag = true;         // can't restore locally: force a 200 next wake
        }
      }
      break;
    case Outcome::BadRequest400:
      p.clear_etag = true;
      p.next = State{0, false, true, false};
      if (!s.showing_error) p.draw = Draw::ErrorScreen;
      break;
    case Outcome::Failure:
      if (p.next.fail_count < 255) p.next.fail_count++;
      if (p.next.fail_count >= BADGE_AFTER_FAILURES && !s.showing_badge && !s.showing_error && frame_stored) {
        p.draw = Draw::StoredFrameWithBadge;
        p.next.showing_badge = true;
        p.next.panel_dirty = false;
      }
      break;
  }
  return p;
}

// fail_count_after: the count after decide() for this wake.
inline int32_t sleep_for(Outcome outcome, uint8_t fail_count_after, int32_t next_refresh_s,
                         int32_t retry_after_s, int32_t fallback_s) {
  switch (outcome) {
    case Outcome::Frame200:
    case Outcome::NotModified304:
      return sleep_after_success(next_refresh_s, fallback_s);
    case Outcome::BadRequest400:
      return clamp_sleep(fallback_s);
    case Outcome::Failure:
    default:
      return backoff_seconds(fail_count_after, retry_after_s);
  }
}

// Between cycles: with a computer on USB the board stays awake (its USB port then stays up
// for flashing and logs); otherwise it deep-sleeps whatever time is left.
enum class Idle : uint8_t { Wait, RunNow, DeepSleep };

inline Idle idle_step(bool computer_attached, int32_t ms_left) {
  if (ms_left <= 0) return Idle::RunNow;
  return computer_attached ? Idle::Wait : Idle::DeepSleep;
}

inline int32_t remaining_sleep_s(int32_t ms_left) { return ms_left <= 1000 ? 1 : (ms_left + 999) / 1000; }

}  // namespace wake
