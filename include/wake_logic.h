#pragma once
// Wake-cycle state and sleep arithmetic (spec §4). Pure: no Arduino, host-tested in test/test_logic.
#include <stdint.h>

namespace wake {

constexpr uint32_t RTC_MAGIC = 0x1B0A4D03;
constexpr int32_t MIN_SLEEP_S = 300;
constexpr int32_t MAX_SLEEP_S = 21600;

// RTC memory (RTC_NOINIT_ATTR): survives deep sleep and resets, not power loss (magic check).
struct Rtc {
  uint32_t magic;
  bool clock_valid;   // system time was set by SNTP or a Date header since the last power-on/chip reset
  bool panel_dirty;   // the panel may not show shown_crc (cold boot, or a reset mid-refresh)
  uint32_t shown_crc;
  uint8_t fail_count; // consecutive wakes whose network step failed
};

inline void rtc_begin(Rtc& r, bool clock_survived_reset) {
  if (r.magic != RTC_MAGIC) {
    r = Rtc{RTC_MAGIC, false, true, 0, 0};
    return;
  }
  if (!clock_survived_reset) r.clock_valid = false;
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

inline int32_t clamp_sleep(int32_t s) {
  return s < MIN_SLEEP_S ? MIN_SLEEP_S : (s > MAX_SLEEP_S ? MAX_SLEEP_S : s);
}

inline int32_t backoff_seconds(uint8_t fail_count, int32_t retry_after_s) {
  int32_t s = fail_count <= 1 ? 300 : (fail_count == 2 ? 900 : 3600);
  if (retry_after_s > s) s = retry_after_s;
  return clamp_sleep(s);
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
