# Dashboard Firmware Implementation Plan (plan 2 of 2)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the smoke-test firmware with the thin client from spec §6. Each wake it
connects to Wi-Fi, fetches `/v1/frame.bin` with `If-None-Match`, draws a changed frame with
one full refresh, keeps the last good frame in flash, and shows an offline badge or a
config-error screen when needed. Then it deep-sleeps for as long as the server says.

**Architecture:**
- **Pure logic, host-testable:** these are header-only files in `include/` with no
  Arduino dependency, tested with Unity under `pio test -e native`.
  - `wake_logic.h`: the outcome classification, the sleep/backoff maths and the decision table.
  - `http_time.h`: parse the `Date` header and format the badge text.
  - `badge.h`: a 5×7 font that draws the badge into the 1-bit frame.
  - `body_reader.h`: reads a body of exactly 48,000 bytes, checked against
    `Content-Length` or the connection closing.
  - `cycle.h`: the whole wake cycle as a template over the hardware operations, so its
    ordering and recovery sequences are tested on the host.
- **Hardware glue**, each part in its own file in `src/`: `frame_store` (LittleFS),
  `panel` (GxEPD2), `fetch` (Wi-Fi + HTTP/HTTPS). `main.cpp` wires them into a single
  wake cycle.
- The smoke test moves to `extras/smoke/`.

**Tech Stack:** C++17, Arduino-ESP32 3.3 (pioarduino), PlatformIO, GxEPD2 1.6.9, LittleFS,
HTTPClient + NetworkClientSecure, Unity (native tests).

**Spec:** `docs/superpowers/specs/2026-09-27-dashboard-design.md` §2.3–2.4 (frame format,
headers) and §6–7.3 (firmware, tests, bring-up). Plan 1 (the server) is done; its API is
the contract here.

## Global Constraints

- Work in the `feat/dashboard` worktree (`/home/garamizo/inkboard-dashboard`). Firmware
  commands run from the worktree root.
- **Pins:** use the ones in `include/pins.h` only.
  - PWR on GPIO1 goes HIGH before `display.init` and LOW after `hibernate()`, and it is held
    LOW through deep sleep.
  - CS, DC and RST are configured as outputs before GxEPD2 init.
  - Init is `init(115200, true, 2, false)`, as in `extras/minimal/minimal.cpp`.
- **Frame:** exactly 48,000 bytes, rows top to bottom, MSB = leftmost pixel, bit 1 = white.
  It is written with `writeImage(buf, 0, 0, 800, 480, FRAME_INVERT)` followed by
  `refresh(false)` (always a full refresh).
- **Headers read:**
  - `ETag`, stored verbatim (the length must be under 40) and sent back as `If-None-Match`.
  - `X-Next-Refresh-Seconds`.
  - `X-UTC-Offset-Seconds`.
  - `Date`.
  - `Retry-After`.
- **Sleep:** always clamped to 300–21,600 s. After a success it is `X-Next-Refresh-Seconds`,
  or `FALLBACK_SLEEP_S` (3600) when that header is missing. Failures back off 300, then
  900, then 3600 s (and `Retry-After` wins if it's longer). A 400 sleeps `FALLBACK_SLEEP_S`.
- **Badge:** it appears after 3 failures in a row, only if a stored frame exists, and it is
  drawn once. It overlays the stored frame and is shown with one full refresh.
- **Flash:** LittleFS on the `spiffs` partition (128 KB) holds exactly `/frame.bin` and
  `/etag.txt`. Writes go to a `.tmp` file and are then renamed.
- **Secrets:** Wi-Fi credentials go in `include/secrets.h` (gitignored). Never commit it.
- **Every commit message** ends with a blank line and then
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Review Focus

1. **A 200 response whose body isn't the frame.** For example a captive-portal login page,
   or a truncated download over flaky Wi-Fi. It must count as a failure and never be drawn.
   Pinned in Task 2 (`test_classify_200_needs_exact_length`).
2. **A missing, garbled or negative `X-Next-Refresh-Seconds`.** The board must fall back to
   an hour, not sleep 0 s or years. Pinned in Task 2 (`test_parse_seconds_rejects_garbage`,
   `test_sleep_after_success`).
3. **Several days of outage.** The failure count must saturate instead of wrapping around,
   and the badge must be drawn exactly once, not every wake. Pinned in Task 2
   (`test_failures_saturate_and_badge_once`).
4. **The same bad config on consecutive wakes.** The error screen is drawn once and then
   left alone. Pinned in Task 2 (`test_repeated_400_draws_error_once`).
5. **Offline right after a power-up**, with no `Date` seen yet. The badge reads "offline",
   with no invented time. Pinned in Task 3 (`test_badge_text_without_time`).

## File Structure

```
platformio.ini                 + [env:smoke], [env:native]
justfile                       flash default = dashboard; comment lists envs
include/
  config.h                     SERVER_URL, FRAME_QUERY, FALLBACK_SLEEP_S, FRAME_INVERT (tracked)
  secrets.h.example            WIFI_SSID / WIFI_PASSWORD template (tracked)
  ca_certs.h                   generated PEM bundle (tracked)
  wake_logic.h                 classify, parse_seconds, sleep/backoff, decide (pure)
  http_time.h                  parse_http_date, format_clock, badge_text (pure)
  badge.h                      5x7 font + draw_badge into 1-bit frame (pure)
  body_reader.h                exact-length HTTP body reader over any stream (pure)
  cycle.h                      run_cycle<Ops>: one wake, save-after-show, cold-boot recovery (pure)
src/
  main.cpp                     BoardOps (hardware) -> wake::run_cycle, then deep sleep
  frame_store.h/.cpp           LittleFS: load/save frame + etag
  panel.h/.cpp                 GxEPD2: show frame, show error screen, power sequencing
  fetch.h/.cpp                 Wi-Fi connect/off, GET frame.bin with headers
extras/smoke/main.cpp          the former src/main.cpp (hardware smoke test)
test/test_logic/test_main.cpp  Unity tests for the three pure headers
tools/gen_ca_certs.sh          regenerates include/ca_certs.h
docs/dashboard-bringup.md      hardware checklist (spec §7.3)
```

---

### Task 1: Move the smoke test out of `src/`

**Files:**
- Move: `src/main.cpp` → `extras/smoke/main.cpp`
- Modify: `platformio.ini`, `README.md`, `docs/smoke-test.md`, `justfile`

**Interfaces:** Produces the `pio run -e smoke` environment. `src/` is then empty until
Task 5.

- [ ] **Step 1: Move the file and add the env**

```bash
mkdir -p extras/smoke && git mv src/main.cpp extras/smoke/main.cpp
```

Append to `platformio.ini`, after `[env:minimal-kit-pins]`:
```ini

; Hardware smoke test (extras/smoke): pin diagnostics + refresh soak loop.
; Flash with: pio run -e smoke -t upload -t monitor   (or: just flash smoke)
[env:smoke]
extends = env:supermini-c6
build_src_filter = -<*> +<../extras/smoke/>
```

- [ ] **Step 2: Point the docs at the new env**

In `README.md`, change the quick-start line
`pio run -t upload -t monitor     # flashes the hardware smoke test` to
`pio run -e smoke -t upload -t monitor   # flashes the hardware smoke test (just flash smoke)`.

In the Layout block, change `src/main.cpp         current firmware: hardware smoke test + refresh soak test`
to these two lines:
```
src/                 dashboard firmware (thin client, see docs/dashboard-bringup.md)
extras/smoke/        hardware smoke test + refresh soak test (pio run -e smoke)
```

In `docs/smoke-test.md`, replace every `pio run -t upload` that flashes the smoke test with
`pio run -e smoke -t upload`. Find them with `grep -n "pio run" docs/smoke-test.md`.

In `justfile`, change the `flash` comment to:
`# Flash firmware and open the serial monitor; ENV: supermini-c6 (dashboard, default), smoke, minimal.`

- [ ] **Step 3: Verify the builds**

Run: `pio run -e smoke 2>&1 | tail -3 && pio run -e minimal 2>&1 | tail -3`
Expected: both end with `[SUCCESS]`. (`pio run -e supermini-c6` would fail now, because
`src/` is empty; Task 5 fixes that.)

- [ ] **Step 4: Commit**

```bash
git add -A platformio.ini extras/smoke src README.md docs/smoke-test.md justfile
git commit -m "Firmware: move the smoke test to extras/smoke ([env:smoke])

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 2: Wake logic (host-tested)

**Files:**
- Modify: `platformio.ini` (add `[env:native]`)
- Create: `include/wake_logic.h`, `test/test_logic/test_main.cpp`

**Interfaces:**
- Produces (in namespace `wake`):
  - `FRAME_BYTES = 48000`, `MIN_SLEEP_S = 300`, `MAX_SLEEP_S = 21600`,
    `BADGE_AFTER_FAILURES = 3`
  - `enum class Outcome { Frame200, NotModified304, BadRequest400, Failure }`
  - `enum class Draw { Nothing, NewFrame, StoredFrame, StoredFrameWithBadge, ErrorScreen }`
  - `struct State { uint8_t fail_count; bool showing_badge; bool showing_error; bool panel_dirty; }`
  - `State cold_boot_state()`, which marks the panel content as unknown
  - `struct Plan { Draw draw; bool save_frame; bool clear_etag; State next; }`
  - `Outcome classify(int http_status, int32_t body_bytes)`
  - `int32_t parse_seconds(const char* s)`, which returns -1 if the value is missing or
    invalid
  - `int32_t parse_signed_seconds(const char* s)`, which returns `INT32_MIN` if the value is
    missing or invalid
  - `int32_t clamp_sleep(int32_t)`
  - `int32_t sleep_after_success(int32_t header_s, int32_t fallback_s)`
  - `int32_t backoff_seconds(uint8_t fail_count, int32_t retry_after_s)`
  - `Plan decide(Outcome, State, bool frame_stored)`
  - `int32_t sleep_for(Outcome, uint8_t fail_count_after, int32_t next_refresh_s, int32_t retry_after_s, int32_t fallback_s)`

- [ ] **Step 1: Add the native test environment**

Append to `platformio.ini`:
```ini

; Host-side unit tests for the pure logic in include/ (no hardware):  pio test -e native
[env:native]
platform = native
test_framework = unity
build_flags = -std=gnu++17 -Wall -Wextra
```

- [ ] **Step 2: Write the failing tests**

`test/test_logic/test_main.cpp`:
```cpp
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
```

- [ ] **Step 3: Run the tests to verify they fail**

Run: `pio test -e native 2>&1 | tail -5`
Expected: a compile error, `wake_logic.h: No such file or directory`. The first run also
downloads the `native` platform.

- [ ] **Step 4: Implement `include/wake_logic.h`**

```cpp
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

}  // namespace wake
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `pio test -e native 2>&1 | tail -5`
Expected: `16 Tests 0 Failures 0 Ignored` and `PASSED`.

- [ ] **Step 6: Commit**

```bash
git add platformio.ini include/wake_logic.h test/test_logic/test_main.cpp
git commit -m "Firmware: wake-cycle decision logic + native Unity tests

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 3: HTTP date and badge text (host-tested)

**Files:**
- Create: `include/http_time.h`
- Modify: `test/test_logic/test_main.cpp`

**Interfaces:**
- Produces (in namespace `httptime`):
  - `int64_t days_from_civil(int y, unsigned m, unsigned d)`
  - `int64_t parse_http_date(const char* s)`: parses an IMF-fixdate into unix seconds, or
    returns -1
  - `void format_clock(int64_t epoch, int32_t utc_offset_s, char out[9])`, giving e.g.
    `"7:39 PM"`
  - `void badge_text(char* out, size_t n, int64_t last_ok_epoch, int32_t utc_offset_s)`,
    giving `"offline since 7:39 PM"`, or `"offline"` when `last_ok_epoch <= 0`

- [ ] **Step 1: Write the failing tests**

Add to `test/test_logic/test_main.cpp`, after `#include "wake_logic.h"`:
```cpp
#include <string.h>

#include "http_time.h"
```
Then add these tests before `main`:
```cpp
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
```
And register them in `main`, before `return UNITY_END();`:
```cpp
  RUN_TEST(test_parse_http_date);
  RUN_TEST(test_format_clock);
  RUN_TEST(test_badge_text_with_time);
  RUN_TEST(test_badge_text_without_time);
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `pio test -e native 2>&1 | tail -5`
Expected: a compile error, `http_time.h: No such file or directory`.

- [ ] **Step 3: Implement `include/http_time.h`**

```cpp
#pragma once
// HTTP Date parsing and the offline badge's clock text. Pure; host-tested.
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace httptime {

// Days since 1970-01-01 for a proleptic Gregorian date (Howard Hinnant's algorithm).
inline int64_t days_from_civil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

// IMF-fixdate, e.g. "Sun, 27 Sep 2026 19:39:05 GMT" -> unix seconds; -1 if invalid.
inline int64_t parse_http_date(const char* s) {
  if (s == nullptr) return -1;
  int d, y, hh, mm, ss;
  char mon[4] = {0};
  if (sscanf(s, "%*3s, %d %3s %d %d:%d:%d GMT", &d, mon, &y, &hh, &mm, &ss) != 6) return -1;
  static const char* kMonths = "JanFebMarAprMayJunJulAugSepOctNovDec";
  const char* at = strstr(kMonths, mon);
  if (at == nullptr || strlen(mon) != 3 || (at - kMonths) % 3 != 0) return -1;
  unsigned m = static_cast<unsigned>((at - kMonths) / 3 + 1);
  if (d < 1 || d > 31 || hh < 0 || hh > 23 || mm < 0 || mm > 59 || ss < 0 || ss > 60) return -1;
  return days_from_civil(y, m, static_cast<unsigned>(d)) * 86400 + hh * 3600 + mm * 60 + ss;
}

// "7:39 PM" in local time (fixed UTC offset; the board has no tz database).
inline void format_clock(int64_t epoch, int32_t utc_offset_s, char out[9]) {
  int64_t local = epoch + utc_offset_s;
  int64_t sec_of_day = ((local % 86400) + 86400) % 86400;
  int h = static_cast<int>(sec_of_day / 3600);
  int m = static_cast<int>((sec_of_day % 3600) / 60);
  snprintf(out, 9, "%d:%02d %s", h % 12 == 0 ? 12 : h % 12, m, h < 12 ? "AM" : "PM");
}

inline void badge_text(char* out, size_t n, int64_t last_ok_epoch, int32_t utc_offset_s) {
  if (last_ok_epoch <= 0) {
    snprintf(out, n, "offline");
    return;
  }
  char clock[9];
  format_clock(last_ok_epoch, utc_offset_s, clock);
  snprintf(out, n, "offline since %s", clock);
}

}  // namespace httptime
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `pio test -e native 2>&1 | tail -5`
Expected: `20 Tests 0 Failures 0 Ignored`.

The expected epochs were computed with Python:
`calendar.timegm((2026,9,27,19,39,5))` = 1790537945 and
`calendar.timegm((2028,2,29,12,0,0))` = 1835438400. If an assertion on those values fails,
recompute them with that command before changing the code.

- [ ] **Step 5: Commit**

```bash
git add include/http_time.h test/test_logic/test_main.cpp
git commit -m "Firmware: HTTP Date parsing and offline badge text

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 4: Badge drawing into the 1-bit frame (host-tested)

**Files:**
- Create: `include/badge.h`
- Modify: `test/test_logic/test_main.cpp`

**Interfaces:**
- Produces (in namespace `badge`):
  - `W = 800`, `H = 480`, `STRIDE = 100`
  - `bool is_black(const uint8_t* frame, int x, int y)`
  - `void set_px(uint8_t* frame, int x, int y, bool black)`
  - `int text_width(const char* s, int scale)`
  - `struct Rect { int x, y, w, h; }`
  - `Rect draw_badge(uint8_t* frame, const char* text, int scale = 2)`: a black-bordered
    white box with black text, placed in the bottom-right corner

- [ ] **Step 1: Write the failing tests**

Add `#include "badge.h"` after `#include "http_time.h"`, and add these tests before `main`:
```cpp
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
```
Register them in `main`:
```cpp
  RUN_TEST(test_pixel_bit_layout);
  RUN_TEST(test_text_width);
  RUN_TEST(test_badge_box_bottom_right_and_contained);
  RUN_TEST(test_badge_overwrites_dark_background);
  RUN_TEST(test_glyph_one);
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `pio test -e native 2>&1 | tail -5`
Expected: a compile error, `badge.h: No such file or directory`.

- [ ] **Step 3: Implement `include/badge.h`**

```cpp
#pragma once
// Offline badge drawn straight into the 1-bit frame (spec §6.3): 5x7 font, no GFX.
// Frame layout: 800x480, rows top to bottom, MSB = leftmost pixel, bit 1 = white.
#include <stdint.h>
#include <string.h>

namespace badge {

constexpr int W = 800, H = 480, STRIDE = W / 8;

inline bool is_black(const uint8_t* frame, int x, int y) {
  return (frame[y * STRIDE + x / 8] & (0x80 >> (x & 7))) == 0;
}

inline void set_px(uint8_t* frame, int x, int y, bool black) {
  if (x < 0 || x >= W || y < 0 || y >= H) return;
  uint8_t mask = static_cast<uint8_t>(0x80 >> (x & 7));
  uint8_t& b = frame[y * STRIDE + x / 8];
  b = black ? static_cast<uint8_t>(b & ~mask) : static_cast<uint8_t>(b | mask);
}

// 7 rows of 5 bits (bit 4 = leftmost column). Only the characters the badge uses.
inline const uint8_t* glyph(char c) {
  static const uint8_t kBlank[7] = {0};
  static const struct { char c; uint8_t rows[7]; } kFont[] = {
      {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}}, {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
      {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}}, {'3', {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}},
      {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}}, {'5', {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}},
      {'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}}, {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
      {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}}, {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},
      {':', {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00}}, {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
      {'M', {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}}, {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
      {'c', {0x00, 0x00, 0x0E, 0x10, 0x10, 0x11, 0x0E}}, {'e', {0x00, 0x00, 0x0E, 0x11, 0x1F, 0x10, 0x0E}},
      {'f', {0x06, 0x09, 0x08, 0x1C, 0x08, 0x08, 0x08}}, {'i', {0x04, 0x00, 0x0C, 0x04, 0x04, 0x04, 0x0E}},
      {'l', {0x0C, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}}, {'n', {0x00, 0x00, 0x16, 0x19, 0x11, 0x11, 0x11}},
      {'o', {0x00, 0x00, 0x0E, 0x11, 0x11, 0x11, 0x0E}}, {'s', {0x00, 0x00, 0x0E, 0x10, 0x0E, 0x01, 0x1E}},
  };
  for (const auto& g : kFont)
    if (g.c == c) return g.rows;
  return kBlank;  // space and anything unknown
}

inline int text_width(const char* s, int scale) {
  int n = static_cast<int>(strlen(s));
  return n == 0 ? 0 : (n * 6 - 1) * scale;
}

inline void draw_text(uint8_t* frame, int x, int y, const char* s, int scale) {
  for (; *s; ++s, x += 6 * scale) {
    const uint8_t* rows = glyph(*s);
    for (int r = 0; r < 7; r++)
      for (int c = 0; c < 5; c++)
        if (rows[r] & (0x10 >> c))
          for (int dy = 0; dy < scale; dy++)
            for (int dx = 0; dx < scale; dx++) set_px(frame, x + c * scale + dx, y + r * scale + dy, true);
  }
}

struct Rect {
  int x, y, w, h;
};

inline Rect draw_badge(uint8_t* frame, const char* text, int scale = 2) {
  const int pad = 4 * scale;
  Rect r{0, 0, text_width(text, scale) + 2 * pad, 7 * scale + 2 * pad};
  r.x = W - r.w - 4;
  r.y = H - r.h - 2;
  for (int y = r.y; y < r.y + r.h; y++)
    for (int x = r.x; x < r.x + r.w; x++) {
      bool border = x < r.x + 2 || x >= r.x + r.w - 2 || y < r.y + 2 || y >= r.y + r.h - 2;
      set_px(frame, x, y, border);
    }
  draw_text(frame, r.x + pad, r.y + pad, text, scale);
  return r;
}

}  // namespace badge
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `pio test -e native 2>&1 | tail -5`
Expected: `25 Tests 0 Failures 0 Ignored`.

- [ ] **Step 5: Commit**

```bash
git add include/badge.h test/test_logic/test_main.cpp
git commit -m "Firmware: offline badge drawn into the 1-bit frame (5x7 font)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 5: Bounded HTTP body reader (host-tested)

**Files:**
- Create: `include/body_reader.h`
- Modify: `test/test_logic/test_main.cpp`

**Why (plan-review finding):** stopping at 48,000 bytes and checking only whether
another byte is *immediately* available would accept an oversized body whose extra bytes
arrive a little later. The reader has to establish that the body really ended.

**Interfaces:**
- Produces:
  `template <class Stream, class Now, class Idle> int32_t body::read_exact(Stream& s, uint8_t* buf, int32_t expect, int32_t content_length, uint32_t timeout_ms, Now now_ms, Idle idle)`
  - It returns `expect` only for a complete body of exactly `expect` bytes, and -1 otherwise.
  - `content_length` comes from the header, or is -1 when the header is absent (the body
    then ends when the server closes the connection).
  - `Stream` needs `int available()`, `int read(uint8_t*, size_t)` and `bool connected()`.
    Arduino's `NetworkClient` has these.

- [ ] **Step 1: Write the failing tests**

Add `#include "body_reader.h"` after `#include "badge.h"`, and these before `main`:
```cpp
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
```
Register them in `main`:
```cpp
  RUN_TEST(test_reader_declared_length_in_pieces);
  RUN_TEST(test_reader_rejects_wrong_declared_length);
  RUN_TEST(test_reader_close_delimited_exact);
  RUN_TEST(test_reader_rejects_delayed_trailing_bytes);
  RUN_TEST(test_reader_rejects_truncated_body);
  RUN_TEST(test_reader_times_out_when_body_never_ends);
  RUN_TEST(test_reader_declared_length_ignores_open_connection);
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `pio test -e native 2>&1 | tail -5`
Expected: a compile error, `body_reader.h: No such file or directory`.

- [ ] **Step 3: Implement `include/body_reader.h`**

```cpp
#pragma once
// Reads an HTTP body that must be exactly `expect` bytes (spec §2.3: 48,000). Generic
// over the stream and clock so it runs on the host in tests.
#include <stddef.h>
#include <stdint.h>

namespace body {

// Returns `expect` for a complete body of exactly that size, else -1.
// content_length: the header's value, or -1 when absent (body ends at connection close).
template <class Stream, class Now, class Idle>
int32_t read_exact(Stream& s, uint8_t* buf, int32_t expect, int32_t content_length, uint32_t timeout_ms,
                   Now now_ms, Idle idle) {
  if (content_length >= 0 && content_length != expect) return -1;
  const uint32_t start = now_ms();
  int32_t got = 0;
  while (got < expect) {
    if (now_ms() - start > timeout_ms) return -1;
    int avail = s.available();
    if (avail > 0) {
      int32_t want = expect - got < avail ? expect - got : avail;
      int n = s.read(buf + got, static_cast<size_t>(want));
      if (n <= 0) return -1;
      got += n;
    } else if (!s.connected()) {
      return -1;  // closed early: truncated
    } else {
      idle();
    }
  }
  if (content_length == expect) return got;  // the declared length says the body is done
  // No declared length: the body ends only when the server closes, and nothing may follow.
  while (now_ms() - start <= timeout_ms) {
    if (s.available() > 0) return -1;
    if (!s.connected()) return got;
    idle();
  }
  return -1;
}

}  // namespace body
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `pio test -e native 2>&1 | tail -5`
Expected: `32 Tests 0 Failures 0 Ignored`.

- [ ] **Step 5: Commit**

```bash
git add include/body_reader.h test/test_logic/test_main.cpp
git commit -m "Firmware: bounded HTTP body reader (exact length, delayed/extra/truncated bytes)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 6: The wake cycle, generic over hardware (host-tested)

**Files:**
- Create: `include/cycle.h`
- Modify: `test/test_logic/test_main.cpp`

**Why (plan-review findings):**
- The order of operations matters. The frame and ETag are saved only **after** the panel
  has shown the frame; saved first, a power cut in between would leave the next wake's
  304 answering for a panel that never updated.
- After a cold boot the panel's contents are unknown, and the first 304 must restore the
  clean stored frame.

Putting the cycle in a template over an `Ops` type lets the host tests check these
sequences directly.

**Interfaces:**
- Consumes: `wake_logic.h` (with `panel_dirty`), `http_time.h`, `badge.h`.
- Produces (in namespace `wake`):
  - `RTC_MAGIC`
  - `struct Rtc { uint32_t magic; State state; int64_t last_ok_epoch; int32_t utc_offset_s; }`
  - `struct Fetched { Outcome outcome; int http_status; int32_t next_refresh_s; int32_t utc_offset_s; int64_t date_epoch; int32_t retry_after_s; char etag[40]; char message[257]; }`
  - `template <class Ops> int32_t run_cycle(Ops& ops, Rtc& rtc, uint8_t* frame, int32_t fallback_s)`,
    which returns the seconds to sleep
  - `Ops` must provide:
    - `bool store_ok()`
    - `void load_etag(char*, size_t)`
    - `bool has_frame()`
    - `bool load_frame(uint8_t*)`
    - `bool save(const uint8_t*, const char*)`
    - `void clear_etag()`
    - `Fetched fetch(const char* etag, uint8_t* buf)`
    - `void show(const uint8_t*)`
    - `void show_error(const char*)`

- [ ] **Step 1: Write the failing tests**

Add `#include "cycle.h"` after `#include "body_reader.h"`, and these before `main`:
```cpp
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
```
Register them in `main`:
```cpp
  RUN_TEST(test_cycle_saves_etag_only_after_showing);
  RUN_TEST(test_cycle_cold_boot_304_restores_stored_frame);
  RUN_TEST(test_cycle_badge_on_third_failure_over_stored_frame);
  RUN_TEST(test_cycle_power_loss_with_badge_then_304_clears_it);
  RUN_TEST(test_cycle_config_error_drawn_once);
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `pio test -e native 2>&1 | tail -5`
Expected: a compile error, `cycle.h: No such file or directory`.

- [ ] **Step 3: Implement `include/cycle.h`**

```cpp
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
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `pio test -e native 2>&1 | tail -5`
Expected: `37 Tests 0 Failures 0 Ignored`.

- [ ] **Step 5: Commit**

```bash
git add include/cycle.h test/test_logic/test_main.cpp
git commit -m "Firmware: wake cycle over an Ops interface (save ETag after show, cold-boot recovery)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 7: The thin-client firmware

**Files:**
- Create: `include/config.h`, `include/secrets.h.example`, `tools/gen_ca_certs.sh`,
  `include/ca_certs.h` (generated), `src/frame_store.h`, `src/frame_store.cpp`,
  `src/panel.h`, `src/panel.cpp`, `src/fetch.h`, `src/fetch.cpp`, `src/main.cpp`
- Modify: `platformio.ini` (`board_build.filesystem = littlefs`), `README.md` (roadmap)

**Interfaces:**
- Consumes: `wake_logic.h`, `http_time.h`, `badge.h`, `body_reader.h` and `cycle.h` (Tasks 2–6), plus
  `include/pins.h`.
- Produces:
  - `frame_store`:
    - `bool store_begin()`
    - `bool store_has_frame()`
    - `bool store_load_frame(uint8_t* buf)`
    - `bool store_load_etag(char* out, size_t n)`
    - `bool store_save(const uint8_t* buf, const char* etag)`
    - `void store_clear_etag()`
  - `panel`:
    - `void panel_show(const uint8_t* frame)`
    - `void panel_show_error(const char* message, const char* query)`
  - `fetch`:
    - `bool wifi_connect(uint32_t timeout_ms)`
    - `void wifi_off()`
    - `wake::Fetched fetch_frame(const char* etag, uint8_t* buf)`
  - `main.cpp`: a `BoardOps` wrapper that hands everything to `wake::run_cycle` (Task 6).

This task has no host tests. The code is thin glue around hardware, and its logic lives
in the headers tested in Tasks 2–6. The gate is a clean `pio run`; behaviour is checked on
the board in Task 6.

- [ ] **Step 1: Config, secrets template, CA bundle**

`include/config.h`:
```cpp
#pragma once
// Board configuration (tracked). Wi-Fi credentials live in secrets.h (gitignored).

// https:// uses TLS with the CA bundle in ca_certs.h. http:// works for a dev server on
// the LAN, e.g. "http://192.168.1.20:8765" with `uvicorn ... --host 0.0.0.0 --port 8765`.
#define SERVER_URL "https://inkboard.signalwave.dev"

// This board's layout and options (spec §2.2). Preview it in a browser at
// SERVER_URL/v1/frame.png?<FRAME_QUERY>.
#define FRAME_QUERY \
  "w=market_trends:2/3,calendar_weather:1/3&lat=34.05&lon=-118.24&tz=America/Los_Angeles&units=imperial"

// For bring-up (docs/dashboard-bringup.md step 1): 1 fetches /v1/test.bin instead of the dashboard.
#define USE_CALIBRATION_PATTERN 0

// Sleep when the server gives no X-Next-Refresh-Seconds, and after a config error.
#define FALLBACK_SLEEP_S 3600

// Server frames use bit 1 = white, the same as GxEPD2's own buffer, so no inversion is
// expected. Confirmed with the calibration pattern in docs/dashboard-bringup.md.
#define FRAME_INVERT false
```

`include/secrets.h.example`:
```cpp
#pragma once
// Copy to include/secrets.h (gitignored) and fill in.
#define WIFI_SSID "your-network"
#define WIFI_PASSWORD "your-password"
```

`tools/gen_ca_certs.sh`:
```bash
#!/usr/bin/env bash
# Regenerate include/ca_certs.h from the system trust store (spec §6.1): the roots that
# Cloudflare's edge certificates chain to (Let's Encrypt, Google Trust Services, SSL.com).
set -euo pipefail
cd "$(dirname "$0")/.."
certs=(ISRG_Root_X1 ISRG_Root_X2 GTS_Root_R1 GTS_Root_R4 SSL.com_Root_Certification_Authority_ECC)
{
  echo "#pragma once"
  echo "// Generated by tools/gen_ca_certs.sh; do not edit."
  echo "// Root CAs for Cloudflare's edge certificates: ${certs[*]}"
  echo "static const char CA_BUNDLE_PEM[] ="
  for c in "${certs[@]}"; do
    echo "    // $c"
    sed 's/.*/    "&\\n"/' "/etc/ssl/certs/$c.pem"
  done
  echo "    ;"
} > include/ca_certs.h
echo "wrote include/ca_certs.h (${#certs[@]} roots)"
```

Run: `chmod +x tools/gen_ca_certs.sh && tools/gen_ca_certs.sh && grep -c "BEGIN CERTIFICATE" include/ca_certs.h`
Expected: `wrote include/ca_certs.h (5 roots)` and then `5`.

Then create your own `include/secrets.h` from the example with the real network (it is
already gitignored). The build in Step 6 needs it.

- [ ] **Step 2: Frame store (LittleFS)**

`src/frame_store.h`:
```cpp
#pragma once
#include <stddef.h>
#include <stdint.h>

// Last good frame + its ETag on LittleFS (spec §6.1). Survives deep sleep and power loss.
bool store_begin();                                 // mounts, formatting on first use
bool store_has_frame();
bool store_load_frame(uint8_t* buf);                // FRAME_BYTES into buf
bool store_load_etag(char* out, size_t n);          // "" if none
bool store_save(const uint8_t* buf, const char* etag);
void store_clear_etag();
```

`src/frame_store.cpp`:
```cpp
#include "frame_store.h"

#include <LittleFS.h>
#include <string.h>

#include "wake_logic.h"

static const char* kFrame = "/frame.bin";
static const char* kEtag = "/etag.txt";

// Write to <path>.tmp, then rename over <path>: a power cut never leaves a torn file.
static bool write_atomic(const char* path, const uint8_t* data, size_t len) {
  String tmp = String(path) + ".tmp";
  File f = LittleFS.open(tmp, "w");
  if (!f) return false;
  size_t n = f.write(data, len);
  f.close();
  if (n != len) {
    LittleFS.remove(tmp);
    return false;
  }
  if (!LittleFS.rename(tmp, path)) {
    LittleFS.remove(path);
    if (!LittleFS.rename(tmp, path)) return false;
  }
  return true;
}

bool store_begin() { return LittleFS.begin(true); }

bool store_has_frame() {
  File f = LittleFS.open(kFrame, "r");
  bool ok = f && f.size() == static_cast<size_t>(wake::FRAME_BYTES);
  if (f) f.close();
  return ok;
}

bool store_load_frame(uint8_t* buf) {
  File f = LittleFS.open(kFrame, "r");
  if (!f) return false;
  size_t n = f.read(buf, wake::FRAME_BYTES);
  f.close();
  return n == static_cast<size_t>(wake::FRAME_BYTES);
}

bool store_load_etag(char* out, size_t n) {
  out[0] = '\0';
  File f = LittleFS.open(kEtag, "r");
  if (!f) return false;
  size_t got = f.readBytes(out, n - 1);
  f.close();
  out[got] = '\0';
  return got > 0;
}

bool store_save(const uint8_t* buf, const char* etag) {
  // Frame first: a stored ETag must never describe a frame that isn't there.
  if (!write_atomic(kFrame, buf, wake::FRAME_BYTES)) return false;
  return write_atomic(kEtag, reinterpret_cast<const uint8_t*>(etag), strlen(etag));
}

void store_clear_etag() { LittleFS.remove(kEtag); }
```

- [ ] **Step 3: Panel (GxEPD2)**

`src/panel.h`:
```cpp
#pragma once
#include <stdint.h>

// Each call powers the HAT, draws with one full refresh, hibernates, and powers it off.
void panel_show(const uint8_t* frame);                          // 48,000-byte server frame
void panel_show_error(const char* message, const char* query);  // config error screen
```

`src/panel.cpp`:
```cpp
#include "panel.h"

#include <Arduino.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <GxEPD2_BW.h>
#include <SPI.h>

#include "config.h"
#include "pins.h"

// A 1/8-height page buffer (6 KB) is enough for the paged error screen; server frames go
// straight to the controller with writeImage and never touch this buffer.
static GxEPD2_BW<GxEPD2_750_T7, GxEPD2_750_T7::HEIGHT / 8> display(
    GxEPD2_750_T7(PIN_EPD_CS, PIN_EPD_DC, PIN_EPD_RST, PIN_EPD_BUSY));

static void power_on() {
  pinMode(PIN_EPD_PWR, OUTPUT);
  digitalWrite(PIN_EPD_PWR, HIGH);
  delay(50);
  // GxEPD2 drives CS/DC/RST before configuring them; set them up first.
  for (int pin : {PIN_EPD_CS, PIN_EPD_DC, PIN_EPD_RST}) {
    pinMode(pin, OUTPUT);
    digitalWrite(pin, HIGH);
  }
  SPI.begin(PIN_EPD_CLK, -1, PIN_EPD_DIN, -1);
  display.init(115200, true, 2, false);  // initial=true: the controller lost its RAM
}

static void power_off() {
  display.hibernate();
  digitalWrite(PIN_EPD_PWR, LOW);
}

void panel_show(const uint8_t* frame) {
  power_on();
  display.writeImage(frame, 0, 0, GxEPD2_750_T7::WIDTH, GxEPD2_750_T7::HEIGHT, FRAME_INVERT, false, false);
  display.refresh(false);
  power_off();
}

void panel_show_error(const char* message, const char* query) {
  power_on();
  display.setRotation(0);
  display.setFullWindow();
  display.setTextColor(GxEPD_BLACK);
  display.setTextWrap(true);
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setFont(&FreeSansBold12pt7b);
    display.setCursor(24, 60);
    display.print("inkboard config error");
    display.setFont(&FreeSans9pt7b);
    display.setCursor(24, 110);
    display.print(message);
    display.setCursor(24, 380);
    display.print("FRAME_QUERY in include/config.h:");
    display.setCursor(24, 410);
    display.print(query);
  } while (display.nextPage());
  power_off();
}
```

- [ ] **Step 4: Fetch (Wi-Fi + HTTP)**

`src/fetch.h`:
```cpp
#pragma once
#include <stdint.h>

#include "cycle.h"

bool wifi_connect(uint32_t timeout_ms);
void wifi_off();
// GET SERVER_URL/v1/frame.bin?FRAME_QUERY (or the calibration pattern); the body goes into buf.
wake::Fetched fetch_frame(const char* etag, uint8_t* buf);
```

`src/fetch.cpp`:
```cpp
#include "fetch.h"

#include <HTTPClient.h>
#include <NetworkClient.h>
#include <NetworkClientSecure.h>
#include <WiFi.h>
#include <string.h>

#include "body_reader.h"
#include "ca_certs.h"
#include "config.h"
#include "http_time.h"
#include "secrets.h"

bool wifi_connect(uint32_t timeout_ms) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > timeout_ms) return false;
    delay(100);
  }
  return true;
}

void wifi_off() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

wake::Fetched fetch_frame(const char* etag, uint8_t* buf) {
  wake::Fetched r;
  String url = String(SERVER_URL) + (USE_CALIBRATION_PATTERN ? "/v1/test.bin" : "/v1/frame.bin?" FRAME_QUERY);
  bool tls = url.startsWith("https://");
  NetworkClientSecure secure;
  NetworkClient plain;
  if (tls) secure.setCACert(CA_BUNDLE_PEM);

  HTTPClient http;
  http.useHTTP10(true);  // no chunked encoding: a plain body of Content-Length bytes, or until close
  http.setConnectTimeout(10000);
  http.setTimeout(20000);
  http.setUserAgent("inkboard/1.0");
  if (!http.begin(tls ? static_cast<NetworkClient&>(secure) : plain, url)) return r;
  static const char* keys[] = {"ETag", "X-Next-Refresh-Seconds", "X-UTC-Offset-Seconds", "Date", "Retry-After"};
  http.collectHeaders(keys, 5);
  if (etag[0] != '\0') http.addHeader("If-None-Match", etag);

  r.http_status = http.GET();
  int32_t body = 0;
  if (r.http_status == 200) {
    body = body::read_exact(*http.getStreamPtr(), buf, wake::FRAME_BYTES, http.getSize(), 20000,
                            [] { return static_cast<uint32_t>(millis()); }, [] { delay(5); });
    String tag = http.header("ETag");
    if (tag.length() < sizeof(r.etag)) strncpy(r.etag, tag.c_str(), sizeof(r.etag) - 1);
  } else if (r.http_status == 400) {
    String text = http.getString();
    strncpy(r.message, text.c_str(), sizeof(r.message) - 1);
  }
  r.next_refresh_s = wake::parse_seconds(http.header("X-Next-Refresh-Seconds").c_str());
  r.utc_offset_s = wake::parse_signed_seconds(http.header("X-UTC-Offset-Seconds").c_str());
  r.date_epoch = httptime::parse_http_date(http.header("Date").c_str());
  r.retry_after_s = wake::parse_seconds(http.header("Retry-After").c_str());
  http.end();
  // A 200 without an ETag is still drawn; its empty tag means the next wake sends no If-None-Match.
  r.outcome = wake::classify(r.http_status, body);
  return r;
}
```

- [ ] **Step 5: The wake cycle**

`src/main.cpp`:
```cpp
// inkboard thin client (spec §6): wake, fetch the frame, draw it if it changed, sleep.
// The cycle and all decisions live in include/cycle.h and wake_logic.h (host-tested);
// this file only supplies the hardware operations.
#include <Arduino.h>
#include <driver/gpio.h>
#include <esp_sleep.h>
#include <soc/soc_caps.h>

#include "config.h"
#include "cycle.h"
#include "fetch.h"
#include "frame_store.h"
#include "panel.h"
#include "pins.h"

RTC_DATA_ATTR static wake::Rtc g_rtc;  // survives deep sleep; magic mismatch = cold boot
static uint8_t g_frame[wake::FRAME_BYTES];

struct BoardOps {
  bool fs;
  bool store_ok() { return fs; }
  void load_etag(char* out, size_t n) { store_load_etag(out, n); }
  bool has_frame() { return store_has_frame(); }
  bool load_frame(uint8_t* buf) { return store_load_frame(buf); }
  bool save(const uint8_t* buf, const char* etag) {
    bool ok = store_save(buf, etag);
    if (!ok) Serial.println("store_save failed");
    return ok;
  }
  void clear_etag() { store_clear_etag(); }
  wake::Fetched fetch(const char* etag, uint8_t* buf) {
    wake::Fetched r;
    if (wifi_connect(15000)) r = fetch_frame(etag, buf);
    wifi_off();  // before drawing: the refresh takes seconds
    Serial.printf("GET -> %d (sent etag: %s)\n", r.http_status, etag[0] ? etag : "none");
    return r;
  }
  void show(const uint8_t* frame) { panel_show(frame); }
  void show_error(const char* message) { panel_show_error(message, FRAME_QUERY); }
};

static void deep_sleep(int32_t seconds) {
  Serial.printf("sleeping %ld s\n", static_cast<long>(seconds));
  Serial.flush();
  // Keep the HAT unpowered while asleep: hold PWR LOW (GPIO1 is an LP pad on the C6).
  pinMode(PIN_EPD_PWR, OUTPUT);
  digitalWrite(PIN_EPD_PWR, LOW);
  gpio_hold_en(static_cast<gpio_num_t>(PIN_EPD_PWR));
#if !SOC_GPIO_SUPPORT_HOLD_SINGLE_IO_IN_DSLP
  gpio_deep_sleep_hold_en();
#endif
  esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(seconds) * 1000000ULL);
  esp_deep_sleep_start();
}

void setup() {
  Serial.begin(115200);
  gpio_hold_dis(static_cast<gpio_num_t>(PIN_EPD_PWR));
  pinMode(PIN_LED_STATUS, OUTPUT);  // heartbeat
  digitalWrite(PIN_LED_STATUS, LOW);
  delay(30);
  digitalWrite(PIN_LED_STATUS, HIGH);

  BoardOps ops{store_begin()};
  deep_sleep(wake::run_cycle(ops, g_rtc, g_frame, FALLBACK_SLEEP_S));
}

void loop() {}  // never reached: setup() ends in deep sleep
```

- [ ] **Step 6: Build**

Add to `[env:supermini-c6]` in `platformio.ini`, after `board_build.partitions = min_spiffs.csv`:
```ini
board_build.filesystem = littlefs
```

Run: `pio run 2>&1 | tail -8`
Expected: `[SUCCESS]`, with RAM usage printed (it should be under 40%: the 48 KB frame plus
the 6 KB page buffer).

Run: `pio test -e native 2>&1 | tail -3`
Expected: `37 Tests 0 Failures 0 Ignored` (nothing regressed).

In `README.md`, change the roadmap line `- [ ] Deep-sleep update cycle + battery voltage` to
`- [x] Deep-sleep update cycle (battery voltage still to do)`.

- [ ] **Step 7: Commit**

```bash
git add include/config.h include/secrets.h.example include/ca_certs.h tools/gen_ca_certs.sh \
        src platformio.ini README.md
git status --short include/secrets.h   # must print nothing (gitignored)
git commit -m "Firmware: thin client (fetch frame, LittleFS store, offline badge, error screen, deep sleep)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 8: Hardware bring-up (with your human partner at the board)

**Files:**
- Create: `docs/dashboard-bringup.md`

This task needs the board on USB and a reachable server. The server can be the deployed
`inkboard.signalwave.dev`, or the dev server on the LAN (see `config.h`). The agent writes
the doc and walks through it with your human partner; each step's result is recorded in
the doc's "Results" table.

- [ ] **Step 1: Write `docs/dashboard-bringup.md`**

````markdown
# Dashboard bring-up

Checks the thin-client firmware on real hardware (spec §7.3). Do them in order; record
results in the table at the end.

**Before you start:**
- `include/secrets.h` has your Wi-Fi credentials (copy it from `include/secrets.h.example`).
- The server is reachable:
  - either `just check` shows the public URL working,
  - or, for the LAN, run `cd server && uv run uvicorn inkboard_server.main:app --host 0.0.0.0 --port 8765`
    and set `SERVER_URL` to `"http://<this machine's LAN IP>:8765"` in `include/config.h`.
- Flash with `just flash`. The serial monitor shows `GET -> <status>` and `sleeping <s> s`
  on each wake.
- Deep sleep drops USB serial. To see the next wake, reopen `just monitor` after the sleep,
  or press RESET to run a wake immediately. The shortest sleep the board ever takes is
  300 s (`wake::MIN_SLEEP_S`).

## 1. Orientation and bit order
Set `USE_CALIBRATION_PATTERN 1` in `include/config.h`, then run `just flash`.
- A solid square is **top-left**, a dot is **bottom-right**, and the brackets are at the
  other two corners.
- The text reads normally (not mirrored), and it is black on white.
- If the colours are inverted, set `FRAME_INVERT true` and repeat.
Then set `USE_CALIBRATION_PATTERN 0` again.

## 2. First frame
Run `just flash`. The panel should match `SERVER_URL/v1/frame.png?<FRAME_QUERY>` in a
browser (or `just check`), and serial should show `GET -> 200`.

## 3. Unchanged frame
Press RESET within the same data cycle (a few minutes after step 2). Serial should show
`GET -> 304`, and the panel must not flash.

## 4. Offline badge
Stop the server (`just down`, or stop the dev server) and let the board wake three times.
- That takes 5 + 15 + 60 min; pressing RESET runs a wake immediately.
- On the third failure the **"offline since h:mm"** badge appears bottom-right, over the
  intact last dashboard.
- Power-cycle the board (unplug it) while it's still offline. The stored frame survives,
  and after three more failures the badge is drawn over it again. Its time now says just
  "offline", because RTC memory was lost.
Restart the server, then press RESET: the next wake gets a 304 and redraws the clean frame
without the badge. Repeat once with a power cut while the badge is showing, restarting the
server right away: the first wake after power-up also redraws the clean frame.

## 5. Config error
Set `FRAME_QUERY` to `"w=market_trends:2/3"` and run `just flash`. The error screen shows
"w: sizes add up to 2/3, need 3/3". Press RESET: the screen is not redrawn. Restore
`FRAME_QUERY` and flash again.

## 6. Power
Measure the current with a USB power meter or a meter on B+:
- deep sleep: ___ µA
- awake per cycle: ___ s at ___ mA

Record both in `docs/hardware.md` (Design notes for battery life).

## Results

| Step | Date | Result | Notes |
|---|---|---|---|
| 1 Orientation | | | |
| 2 First frame | | | |
| 3 Unchanged frame | | | |
| 4 Offline badge | | | |
| 5 Config error | | | |
| 6 Power | | | |
````

- [ ] **Step 2: Run the checklist with your human partner**

Walk through steps 1–5 together. If a step fails, stop and debug it with
superpowers:systematic-debugging before going further. Fill in the Results table from what
you actually observe. Step 6 (power) can wait for a meter.

- [ ] **Step 3: Commit**

```bash
git add docs/dashboard-bringup.md include/config.h
git commit -m "Docs: dashboard bring-up checklist + results

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```
Before committing `config.h`, check that it contains `USE_CALIBRATION_PATTERN 0` and the
real `FRAME_QUERY` (not a leftover test value), and the public `SERVER_URL`.
