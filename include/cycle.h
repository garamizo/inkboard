#pragma once
// One wake cycle (spec §6.2-6.3), generic over the hardware (`Ops`) so the ordering and
// recovery sequences are host-tested. src/main.cpp supplies the real Ops.
#include <stddef.h>
#include <stdint.h>

#include "badge.h"
#include "http_time.h"
#include "wake_logic.h"

namespace wake {

constexpr uint32_t RTC_MAGIC = 0x1B0A4D02;

// Lives in RTC memory: survives deep sleep, lost on power loss (magic mismatch).
struct Rtc {
  uint32_t magic;
  State state;
  int64_t last_ok_epoch;
  int32_t utc_offset_s;
};

struct Fetched {
  Outcome outcome = Outcome::Failure;
  int http_status = -1;
  int32_t next_refresh_s = -1;       // X-Next-Refresh-Seconds, -1 if missing
  int32_t utc_offset_s = INT32_MIN;  // X-UTC-Offset-Seconds, INT32_MIN if missing
  int64_t date_epoch = -1;           // Date, -1 if missing
  int32_t retry_after_s = -1;        // Retry-After, -1 if missing
  char etag[40] = "";                // new ETag (200 only)
  char message[257] = "";            // body of a 400
};

template <class Ops>
int32_t run_cycle(Ops& ops, Rtc& rtc, uint8_t* frame, int32_t fallback_s) {
  if (rtc.magic != RTC_MAGIC) rtc = Rtc{RTC_MAGIC, cold_boot_state(), 0, 0};
  const bool fs = ops.store_ok();
  char etag[40] = "";
  if (fs) ops.load_etag(etag, sizeof etag);

  const Fetched r = ops.fetch(etag, frame);
  const bool stored = fs && ops.has_frame();
  const Plan p = decide(r.outcome, rtc.state, stored);
  if (r.outcome == Outcome::Frame200 || r.outcome == Outcome::NotModified304) {
    if (r.date_epoch > 0) rtc.last_ok_epoch = r.date_epoch;
    if (r.utc_offset_s != INT32_MIN) rtc.utc_offset_s = r.utc_offset_s;
  }
  if (p.clear_etag && fs) ops.clear_etag();

  switch (p.draw) {
    case Draw::NewFrame:
      ops.show(frame);
      // Only after the panel shows it: a tag saved earlier would, after a power cut, make
      // the next wake's 304 vouch for a panel that never got this frame.
      if (p.save_frame && fs) ops.save(frame, r.etag);
      break;
    case Draw::StoredFrame:
      if (ops.load_frame(frame)) ops.show(frame);
      break;
    case Draw::StoredFrameWithBadge:
      if (ops.load_frame(frame)) {
        char text[32];
        httptime::badge_text(text, sizeof text, rtc.last_ok_epoch, rtc.utc_offset_s);
        badge::draw_badge(frame, text);  // the copy in flash stays clean
        ops.show(frame);
      }
      break;
    case Draw::ErrorScreen:
      ops.show_error(r.message);
      break;
    case Draw::Nothing:
      break;
  }
  rtc.state = p.next;
  return sleep_for(r.outcome, rtc.state.fail_count, r.next_refresh_s, r.retry_after_s, fallback_s);
}

}  // namespace wake
