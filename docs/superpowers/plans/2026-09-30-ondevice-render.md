# On-device rendering Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The ESP32-C6 fetches Open-Meteo and FRED itself, renders the 800×480 1-bit dashboard with the same look as the Python server, and the server is deleted.

**Architecture:** Portable header-only C++ in `include/` (canvas, fonts, widgets, compositor, query, time zones, streaming JSON, data cache, wake cycle) built for both the board and `pio test -e native`; thin hardware glue in `src/` (Wi-Fi/SNTP/HTTPS, LittleFS, panel). Parity with the server is checked against reference images that the server code itself renders from the same fixtures, until the server is removed in the last phase.

**Tech Stack:** C++17, Arduino-ESP32 3.3 (pioarduino), GxEPD2 (display only), PlatformIO native + Unity for host tests, Python 3 + Pillow 12.3.0 via `uv run` for dev-time generators (fonts, tz table, fixtures, reference images).

**Spec:** `docs/superpowers/specs/2026-09-30-ondevice-render-design.md` (section numbers "spec §N" below refer to it). The widget designs are in `docs/superpowers/specs/2026-09-27-dashboard-design.md` §4–§5. The Python server in `server/inkboard_server/` is the reference implementation for every port until Task 20 deletes it.

## Global Constraints

- Work only in the worktree `/home/garamizo/inkboard-ondevice` on branch `feat/ondevice-render`. Never touch the production containers (`just up/down/check/logs`).
- Flashing and serial reads need the physical board: ask the user before every upload (Tasks 1, 18, 19).
- Never print or commit secrets: `include/secrets.h` (Wi-Fi, `FRED_API_KEY`). Never log a FRED URL or an HTTPClient error string for FRED (both contain `api_key`).
- `include/pins.h` mirrors `docs/wiring.md`; this plan changes neither.
- Portable code lives header-only in `include/` with no Arduino includes; hardware calls stay in `src/`. Namespace `ink` (sub-namespaces `ink::json`, `ink::tz`, `ink::fonts`); the existing `wake`, `httptime` namespaces stay.
- Frame format: 800×480, 48,000 bytes, rows top to bottom, MSB = leftmost pixel, **bit 1 = white**.
- Pillow fill values are kept as ints: `BLACK = 0`, `WHITE = 255`, `NONE = -1`; a fill is black iff `<= 140` (server `frame.py` `THRESHOLD`).
- Geometry: `WIDGET_H = 464`, `FOOTER_H = 16`; column widths 1/3 = 266, 2/3 = 534, full = 800; three thirds leave x = 798–799 white.
- TTLs: weather 1,800 s, FRED 21,600 s; stale grace 5,400 s; FRED full refetch after 7 days; retry spacing after a failure 300 s.
- Sleep: clamp 300–21,600 s; back-off 300 / 900 / 3,600 s by consecutive network failures, longer if `Retry-After` says so; success → 60 s past the next local top of the hour.
- Network budget per wake: 45 s total; Wi-Fi connect ≤ 15 s; SNTP ≤ 5 s; FRED skipped when < 8 s remain.
- C++ comments say why, not what, matching the existing density. Commit subjects `<area>: <what>` (e.g. `Render: …`, `Firmware: …`, `Docs: …`, `Tools: …`), each commit ending with the line `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- Every task ends green: `pio test -e native` passes, and from Task 18 on `pio run -e supermini-c6` builds.

## Review Focus

- **Clock jumps after SNTP corrects a drifted RTC** (e.g. +40 min): cache ages, "updated" time and the next wake must use the corrected clock; no negative ages (an age < 0 counts as "due"). Test in Task 11 (`test_due_when_clock_went_backwards`).
- **A wake that crosses local midnight between fetch and render**: the date used for rendering, the forecast row selection and the market grid's `today` must come from one `now` read after the network step. Test in Task 17 (`test_cycle_uses_one_now_after_network`).
- **Config change across a reflash** (other `lat`, `units`, `tz`, more `years`, other series): old caches must not be shown as the new location's weather or under a different series. Tests in Task 11 (`test_weather_key_mismatch_is_due`, `test_years_raised_needs_full`), Task 15 (`test_weather_from_other_location_not_shown`) and Task 17 (`test_unconfigured_series_files_removed`).
- **FRED response with observations out of order, duplicate dates, or a `"."` as the last value**: the result must equal the server's `summarize()` (which sorts and filters). Test in Task 10 (`test_resampler_rejects_unsorted`, `test_dot_values_and_duplicates`); a duplicate date keeps the larger value, as Python's sort of (date, value) pairs does.
- **LittleFS full or a cache file corrupted by a power cut mid-write**: a bad CRC means "no cache" for that file, never garbage on screen; a failed save keeps the board running from RAM. Tests in Task 11 (`test_decode_rejects_bad_crc`) and Task 17 (`test_save_failure_still_renders`).

---

## File map

Created (portable, `include/`):

| File | Responsibility | Task |
|---|---|---|
| `crc32.h` | CRC-32 (frame change detection, cache files, PNG chunks) | 3 |
| `civil.h` | Day numbers, Gregorian dates, weekday, ISO date parse, floor div | 3 |
| `format.h` | Python-compatible number/date text | 3 |
| `render/png.h` | Encode 1-bit PNG with stored deflate blocks | 3 |
| `render/canvas.h` | `Bitmap`, `View`, Pillow-compatible primitives | 4 |
| `render/font.h`, `render/text.h`, `render/fonts/*.h` | Font format, text with Pillow anchors, generated DejaVu fonts | 5 |
| `tz_table.h` (generated), `tz.h` | IANA → POSIX rule, local time, next top of hour | 6 |
| `series.h`, `query.h` | Series catalog; `Layout`, `parse_query()` | 7 |
| `json_stream.h` | Streaming JSON tokenizer | 8 |
| `sources/openmeteo.h` | Weather URL + parser | 9 |
| `sources/fred.h`, `market_data.h` | FRED URL + parser + Sunday resampler; `Summary`, `summarize()` | 10 |
| `data_cache.h` | Cache records, encode/decode, due/stale rules, FRED fetch planning | 11 |
| `render/icons.h`, `render/lines.h`, `render/calendar.h` | Ports of `draw/icons.py`, `draw/lines.py`, `draw/calendar.py` | 12 |
| `widgets/calendar_weather.h` | Widget port (+ `WIDGET_H`, `FOOTER_H`) | 13 |
| `widgets/market_trends.h` | Widget port | 14 |
| `compositor.h`, `model.h` | Columns, footer, error screen, calibration; `Model`, `build_frame()` | 15 |

Rewritten: `include/wake_logic.h`, `include/cycle.h`, `include/http_time.h` (17); `include/config.h`, `include/secrets.h.example`, `include/ca_certs.h` (18).
Created (`src/`): `net.h/.cpp`, `data_store.h/.cpp` (18). Rewritten: `src/main.cpp`, `src/panel.h/.cpp` (18). Deleted: `src/fetch.*`, `src/frame_store.*`, `include/badge.h`, `include/body_reader.h` (18).
Tools: `tools/gen_fonts.py`, `tools/fonts/` (moved TTFs + LICENSE) (5); `tools/ref_primitives.py` (4); `tools/gen_tz_table.py` (6); `tools/record_fixtures.py` (9); `tools/ref_render.py` (13, deleted in 20); `tools/preview/main.cpp` (16); `tools/gen_ca_certs.sh` (18).
Tests: `test/support/ink_test.h` (2); suites `test/test_render/` (2–5), `test/test_data/` (2–11), `test/test_widgets/` (12–15), `test/test_logic/` (rewritten in 17); data in `test/fixtures/` (9), `test/reference/` (4, 5, 13), `test/goldens/` (13–15).
Extras: `extras/spike/main.cpp` (1, throwaway, deleted in 20); `extras/smoke/main.cpp` network step (18).
Order: tasks run in number order; each depends only on lower-numbered tasks.

## Shared types (defined once, used across tasks)

These signatures are binding. Each task that defines one repeats it in full; later tasks use exactly these names.

```cpp
// civil.h
namespace ink {
int64_t floor_div(int64_t a, int64_t b);
int64_t floor_mod(int64_t a, int64_t b);
int32_t days_from_civil(int y, int m, int d);           // days since 1970-01-01
struct Ymd { int y, m, d; };
Ymd civil_from_days(int32_t day);
int weekday(int32_t day);                                // Monday = 0 ... Sunday = 6 (Python)
bool is_leap(int y);
int days_in_month(int y, int m);
int32_t parse_iso_date(const char* s);                   // "YYYY-MM-DD" -> day, else INT32_MIN
int32_t first_sunday_on_or_after(int32_t day);
}
// render/canvas.h
namespace ink {
constexpr int FRAME_W = 800, FRAME_H = 480, FRAME_BYTES = 48000;
constexpr int NONE = -1, BLACK = 0, WHITE = 255;
struct Box { int x, y, w, h; };
struct Pt { double x, y; };
class Bitmap;   // (uint8_t* bits, int w, int h)
class View;     // (Bitmap&, Box); point/line/rectangle/ellipse/polygon/rounded_rectangle/fill/sub
}
// render/font.h, render/text.h
namespace ink {
struct Glyph; struct Kern; struct Font;
const Font& font(int size, bool bold = false);
double text_length(const Font& f, const char* utf8);     // Pillow getlength()
void draw_text(View& v, double x, double y, const char* utf8, const Font& f, int ink, const char* anchor = "la");
}
// query.h
namespace ink {
enum class WidgetType : uint8_t { MarketTrends, CalendarWeather };
enum class Size : uint8_t { Third = 1, TwoThirds = 2, Full = 3 };
int size_width(Size s);
struct Column { WidgetType type; Size size; };
struct Layout;   // columns, tz name + rule, series, years, lat, lon, metric
bool parse_query(const char* raw, Layout& out, char* error, size_t error_n);
}
// data_cache.h
namespace ink {
struct SourceStatus { int64_t fetched_at; int64_t retry_not_before; bool last_attempt_failed; bool auth_rejected; };
struct FetchResult { int http_status; bool complete; int32_t retry_after_s; int64_t date_epoch; bool auth_error; };
struct WeatherCache; struct SeriesCache;
}
// model.h
namespace ink {
struct Model;   // layout + weather cache + series caches + now
void build_frame(Bitmap& frame, const Model& m, const char* version);
}
```

---

## Phase 1: Hardware spike

### Task 1: Measure the board (throwaway spike)

Answers the open hardware questions before any porting (spec §6 phase 1, §7): heap during TLS with the frame and series resident, system time across sleep/reset, FRED and Open-Meteo payload sizes, keep-alive, CA roots, newlib TZ. Nothing here is kept except the numbers, which go into the spec.

**Files:**
- Create: `extras/spike/main.cpp`
- Modify: `platformio.ini` (add `[env:spike]`), `include/secrets.h.example` (add `FRED_API_KEY`)
- Modify: `docs/superpowers/specs/2026-09-30-ondevice-render-design.md` (append "§8 Spike results")

**Interfaces:**
- Consumes: `include/secrets.h` (`WIFI_SSID`, `WIFI_PASSWORD`, new `FRED_API_KEY`), `include/pins.h`.
- Produces: measured numbers only (spec §8). Later tasks read: max TLS heap use, which reset reasons keep `gettimeofday()`, whether HTTPClient reuses the TLS connection, the CA roots of both hosts.

- [ ] **Step 1: Add the FRED key placeholder and ask the user for theirs**

Append to `include/secrets.h.example`:

```cpp
// Free key from https://fredaccount.stlouisfed.org/apikeys (needed for market_trends).
#define FRED_API_KEY "your-fred-api-key"
```

Ask the user to add `#define FRED_API_KEY "<their key>"` to their `include/secrets.h` (gitignored). Do not read or print the file.

- [ ] **Step 2: Find the CA roots of both APIs**

Run:
```bash
for h in api.open-meteo.com api.stlouisfed.org; do
  echo "== $h"; openssl s_client -connect $h:443 -servername $h -showcerts </dev/null 2>/dev/null \
    | grep -E '^ *[0-9] s:|^ *i:' | tail -2
done
```
Expected: the issuer line of the last certificate in each chain names the root (e.g. "ISRG Root X1", "DigiCert Global Root G2", "Amazon Root CA 1"). Note both root names; find the matching files in `/etc/ssl/certs/` (`ls /etc/ssl/certs | grep -i <name>`). Record them for Step 3 and for Task 18.

- [ ] **Step 3: Write the spike**

Add to `platformio.ini` after `[env:smoke]`:

```ini
; Throwaway measurements for the on-device rendering plan (Task 1). Deleted in Task 20.
[env:spike]
extends = env:supermini-c6
build_src_filter = -<*> +<../extras/spike/>
```

Create `extras/spike/main.cpp`. It must, in order, printing one `SPIKE key=value` line per measurement:

```cpp
// Throwaway spike (plan Task 1): measures heap, time and payloads for on-device rendering.
#include <Arduino.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_sntp.h>
#include <sys/time.h>
#include <time.h>

#include "secrets.h"

// Roots found in Task 1 Step 2: paste the PEM blocks of both roots here.
static const char kRoots[] =
    "-----BEGIN CERTIFICATE-----\n"
    "...\n"  // replace with the real PEM lines, one "...\n" string per line
    "-----END CERTIFICATE-----\n";

static uint8_t g_frame[48000];                 // as in the dashboard: static, always resident
static double g_series[4][530];                // 4 × MAX_POINTS doubles
RTC_NOINIT_ATTR static uint32_t g_magic;
RTC_NOINIT_ATTR static int64_t g_synced_at;    // epoch of the last SNTP sync (0 = never)

static void heap(const char* tag) {
  Serial.printf("SPIKE heap_%s free=%u min=%u largest=%u stack_hwm=%u\n", tag,
                (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size(),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                (unsigned)uxTaskGetStackHighWaterMark(nullptr));
}

static int64_t now_s() { struct timeval tv; gettimeofday(&tv, nullptr); return tv.tv_sec; }

// GET over one client; counts body bytes without storing them (the parser will stream).
static void get(NetworkClientSecure& c, HTTPClient& http, const String& url, const char* tag) {
  uint32_t t0 = millis();
  http.setReuse(true);
  http.begin(c, url);
  const char* keys[] = {"Date", "Content-Length", "Transfer-Encoding"};
  http.collectHeaders(keys, 3);
  int code = http.GET();
  size_t total = 0;
  if (code > 0) {
    WiFiClient* s = http.getStreamPtr();
    uint8_t buf[512];
    uint32_t idle = millis();
    while (http.connected() && millis() - idle < 10000) {
      int n = s->available() ? s->read(buf, sizeof buf) : 0;
      if (n > 0) { total += n; idle = millis(); } else delay(2);
      if (http.getSize() > 0 && total >= (size_t)http.getSize()) break;
    }
  }
  heap(tag);
  Serial.printf("SPIKE get_%s code=%d bytes=%u ms=%lu te=%s date=\"%s\"\n", tag, code, (unsigned)total,
                millis() - t0, http.header("Transfer-Encoding").c_str(), http.header("Date").c_str());
  http.end();  // with setReuse(true) the TLS session stays open if the server allows keep-alive
}

void setup() {
  Serial.begin(115200);
  delay(2000);
  memset(g_frame, 0xFF, sizeof g_frame);
  for (auto& s : g_series) for (double& v : s) v = 1.0;
  bool rtc_ok = g_magic == 0x5B1CE001;
  Serial.printf("SPIKE reset_reason=%d rtc_magic_ok=%d now=%lld synced_at=%lld\n", (int)esp_reset_reason(),
                rtc_ok, (long long)now_s(), rtc_ok ? (long long)g_synced_at : -1LL);
  heap("boot");

  // newlib POSIX TZ with an angle-bracket name (Lord Howe): does localtime_r handle it?
  setenv("TZ", "<+1030>-10:30<+11>-11,M10.1.0,M4.1.0", 1);
  tzset();
  time_t probe = 1798761600;  // 2027-01-01 00:00 UTC (DST in Lord Howe)
  struct tm lt;
  localtime_r(&probe, &lt);
  Serial.printf("SPIKE newlib_tz_lordhowe=%02d:%02d (want 11:00)\n", lt.tm_hour, lt.tm_min);
  setenv("TZ", "UTC0", 1);
  tzset();

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) delay(100);
  Serial.printf("SPIKE wifi_ms=%lu ok=%d\n", millis() - t0, WiFi.status() == WL_CONNECTED);
  heap("wifi");

  t0 = millis();
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  while (sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED && millis() - t0 < 5000) delay(20);
  int64_t before = now_s();
  Serial.printf("SPIKE sntp_ms=%lu now=%lld drift_vs_rtc_s=%lld\n", millis() - t0, (long long)before,
                rtc_ok && g_synced_at ? (long long)(before - g_synced_at) : 0LL);
  g_magic = 0x5B1CE001;
  g_synced_at = before;

  NetworkClientSecure c;
  c.setCACert(kRoots);
  c.setHandshakeTimeout(10);
  HTTPClient http;
  get(c, http, "https://api.open-meteo.com/v1/forecast?latitude=34.1&longitude=-118.2&current=temperature_2m,weather_code"
               "&daily=weather_code,temperature_2m_max,temperature_2m_min&temperature_unit=fahrenheit"
               "&timezone=America%2FLos_Angeles&forecast_days=8", "openmeteo");

  NetworkClientSecure f;
  f.setCACert(kRoots);
  f.setHandshakeTimeout(10);
  // Full 10-year daily series (the largest payload), then three tails on the same client.
  String base = String("https://api.stlouisfed.org/fred/series/observations?file_type=json&api_key=") + FRED_API_KEY;
  get(f, http, base + "&series_id=CBBTCUSD&observation_start=2016-07-30", "fred_full_btc");
  get(f, http, base + "&series_id=SP500&observation_start=2026-05-25", "fred_tail_sp500");
  get(f, http, base + "&series_id=MORTGAGE30US&observation_start=2026-05-25", "fred_tail_mortgage");
  get(f, http, base + "&series_id=MEDLISPRI31080&observation_start=2026-05-25", "fred_tail_home");
  heap("after_fred");
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  Serial.printf("SPIKE awake_ms=%lu\n", millis());
  Serial.println("SPIKE done: sleeping 60 s, then tap RESET once, then unplug/replug power once");
  Serial.flush();
  esp_sleep_enable_timer_wakeup(60ULL * 1000000ULL);
  esp_deep_sleep_start();
}

void loop() {}
```

The URLs never get printed (they contain the key). Replace the `kRoots` placeholder lines with the PEM text of the two roots from Step 2 (`sed 's/.*/    "&\\n"/' /etc/ssl/certs/<root>.pem`).

- [ ] **Step 4: Build**

Run: `pio run -e spike`
Expected: `SUCCESS`. Note `RAM:` and `Flash:` usage from the output.

- [ ] **Step 5: Run on the board (ask the user first)**

Ask the user to plug the board in and confirm. Then run `pio run -e spike -t upload -t monitor` and capture the `SPIKE` lines across: the first boot, the timer wake after 60 s (reason 8 = deep sleep), a RESET tap, and a power cycle (unplug USB and battery, replug). Ask the user to do the RESET tap and power cycle at the right moments.

Expected: four runs of `SPIKE` lines. Key questions to answer from them:
- `heap_after_fred largest=` ≥ 30,000 and `min=` ≥ 40,000 → the budget holds. If not, Task 18 allocates the frame only after Wi-Fi is off (spec §7).
- `reset_reason` and `drift_vs_rtc_s` per run: which reasons keep a valid `now` (|drift| < 5 s). Expected: deep sleep keeps time; power-on does not.
- `get_fred_tail_*` faster than the first FRED call → keep-alive works.
- `newlib_tz_lordhowe` — informational only: the plan uses its own POSIX evaluator (Task 8).
- `bytes=` of each request.

- [ ] **Step 6: Record the results in the spec**

Append to the spec:

```markdown
## 8. Spike results (Task 1, <date>)

| Measurement | Value |
|---|---|
| Free heap at boot / after Wi-Fi / after FRED (min, largest block) | … |
| Wi-Fi connect, SNTP, Open-Meteo, FRED full (BTC 10 y), FRED tail ×3 | … ms, … bytes each |
| Keep-alive reused across FRED requests | yes / no |
| Reset reasons that keep system time (drift < 5 s) | e.g. 8 (deep sleep), 3 (SW) |
| Reset reasons that lose it | e.g. 1 (power-on), 2 (EXT/RESET pin) |
| CA roots | api.open-meteo.com: …; api.stlouisfed.org: … |
| Firmware size of the spike (RAM/Flash) | … |
```

Fill in the real numbers.

- [ ] **Step 7: Commit**

```bash
git add extras/spike/main.cpp platformio.ini include/secrets.h.example docs/superpowers/specs/2026-09-30-ondevice-render-design.md
git commit -m "Spike: heap, clock and payload measurements for on-device rendering

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

**Decision gate:** if `largest` after FRED is < 30,000, stop and tell the user before Phase 2 (the fallback in spec §7 changes Task 18).

---

## Phase 2: Foundations (host-tested)

### Task 2: Test scaffolding

**Files:**
- Modify: `platformio.ini` (`[env:native]`)
- Create: `test/support/ink_test.h`, `test/test_render/test_main.cpp`, `test/test_data/test_main.cpp`
- Modify: `.gitignore` (add `test/**/*.actual.png`, `test/reference/**/*.cpp.png`)

**Interfaces:**
- Consumes: nothing.
- Produces: `INK_TEST_DIR` (absolute path of `test/`, a string literal); in `ink_test` namespace: `std::string path(const char* rel)`, `bool read_file(const std::string&, std::string& out)`, `bool write_file(const std::string&, const void*, size_t)`, `bool update_goldens()`, `void golden(const char* name, const uint8_t* bits, int w, int h)` (added in Task 3), `bool read_pbm(const std::string& path, std::vector<uint8_t>& bits, int& w, int& h)`, `long match_reference(const char* rel_pbm, const uint8_t* bits, int w, int h)` (added in Task 3).

- [ ] **Step 1: Pass the test directory into native builds**

In `platformio.ini`, replace the `[env:native]` `build_flags` line with:

```ini
build_flags = -std=gnu++17 -Wall -Wextra -DINK_TEST_DIR=\"$PROJECT_DIR/test\"
```

- [ ] **Step 2: Write the helper header**

Create `test/support/ink_test.h`:

```cpp
#pragma once
// Shared helpers for the native test suites: file access, goldens, reference images.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unity.h>

#include <string>
#include <vector>

#ifndef INK_TEST_DIR
#error "INK_TEST_DIR comes from platformio.ini [env:native]"
#endif

namespace ink_test {

inline std::string path(const char* rel) { return std::string(INK_TEST_DIR) + "/" + rel; }

inline bool read_file(const std::string& p, std::string& out) {
  FILE* f = fopen(p.c_str(), "rb");
  if (!f) return false;
  out.clear();
  char buf[65536];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
  fclose(f);
  return true;
}

inline bool write_file(const std::string& p, const void* data, size_t n) {
  FILE* f = fopen(p.c_str(), "wb");
  if (!f) return false;
  bool ok = fwrite(data, 1, n, f) == n;
  return fclose(f) == 0 && ok;
}

inline bool update_goldens() {
  const char* v = getenv("INKBOARD_UPDATE_GOLDENS");
  return v != nullptr && v[0] == '1';
}

}  // namespace ink_test
```

- [ ] **Step 3: Write a failing smoke test that reads a file through `INK_TEST_DIR`**

Create `test/test_data/test_main.cpp`:

```cpp
#include <unity.h>

#include "../support/ink_test.h"

void setUp() {}
void tearDown() {}

void test_test_dir_is_absolute_and_readable() {
  std::string s;
  TEST_ASSERT_EQUAL_CHAR('/', INK_TEST_DIR[0]);
  TEST_ASSERT_TRUE(ink_test::read_file(ink_test::path("support/ink_test.h"), s));
  TEST_ASSERT_NOT_EQUAL(std::string::npos, s.find("INK_TEST_DIR"));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_test_dir_is_absolute_and_readable);
  return UNITY_END();
}
```

Create `test/test_render/test_main.cpp` with the same skeleton and one test `test_render_suite_builds` that does `TEST_ASSERT_TRUE(true);` (later tasks add real tests here).

- [ ] **Step 4: Run**

Run: `pio test -e native`
Expected: `test_logic`, `test_data`, `test_render` all PASSED. If `test_test_dir_is_absolute_and_readable` fails because `$PROJECT_DIR` was not expanded (the literal text `$PROJECT_DIR/test` appears), replace the flag with PlatformIO's dynamic form and rerun:

```ini
build_flags = -std=gnu++17 -Wall -Wextra !python3 -c "import os; print('-DINK_TEST_DIR=\\\"' + os.getcwd() + '/test\\\"')"
```

- [ ] **Step 5: Commit**

```bash
printf 'test/**/*.actual.png\ntest/reference/**/*.cpp.png\n' >> .gitignore
git add platformio.ini .gitignore test/support/ink_test.h test/test_data/test_main.cpp test/test_render/test_main.cpp
git commit -m "Tests: native suites for render and data, with shared file helpers

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

### Task 3: Dates, formatting, CRC and PNG

**Files:**
- Create: `include/civil.h`, `include/format.h`, `include/crc32.h`, `include/render/png.h`
- Modify: `test/support/ink_test.h` (golden + PBM helpers)
- Test: `test/test_data/test_main.cpp`, `test/test_render/test_main.cpp`

**Interfaces:**
- Consumes: `ink_test` helpers (Task 2).
- Produces (all `namespace ink`, header-only):
  - `civil.h`: `floor_div`, `floor_mod`, `days_from_civil`, `Ymd`, `civil_from_days`, `weekday`, `is_leap`, `days_in_month`, `parse_iso_date`, `first_sunday_on_or_after` (signatures in "Shared types").
  - `format.h`: `DAY_ABBR[7]`, `DAY_NAME[7]` (Monday first), `MONTH_ABBR[13]`, `MONTH_NAME[13]` (index 1–12); `void fmt_deg(char* out, size_t n, double v)`; `void fmt_thousands0(char* out, size_t n, double v)`; `void fmt_clock(char* out, size_t n, int hh, int mm)` ("10:00 AM").
  - `crc32.h`: `uint32_t crc32_update(uint32_t crc, const uint8_t* p, size_t n)`, `uint32_t crc32(const uint8_t* p, size_t n)`.
  - `render/png.h`: `std::vector<uint8_t> png_encode(const uint8_t* bits, int w, int h)` (rows of `(w + 7) / 8` bytes, bit 1 = white).
  - `ink_test::golden(name, bits, w, h)`, `ink_test::read_pbm(...)`, `ink_test::match_reference(rel_pbm, bits, w, h)` → number of differing pixels, or -1 if the reference is missing.

- [ ] **Step 1: Write the failing tests for civil.h and format.h**

Add to `test/test_data/test_main.cpp` (and `RUN_TEST` each in `main`):

```cpp
#include "civil.h"
#include "format.h"

using namespace ink;

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
```

- [ ] **Step 2: Run to see them fail**

Run: `pio test -e native -f test_data`
Expected: compile error, `civil.h: No such file or directory`.

- [ ] **Step 3: Implement `include/civil.h`**

```cpp
#pragma once
// Calendar arithmetic on day numbers (days since 1970-01-01), matching Python's date.
// Pure; host-tested in test/test_data.
#include <stdint.h>

namespace ink {

inline int64_t floor_div(int64_t a, int64_t b) {
  int64_t q = a / b;
  return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}
inline int64_t floor_mod(int64_t a, int64_t b) { return a - floor_div(a, b) * b; }

// Howard Hinnant's algorithms (proleptic Gregorian).
inline int32_t days_from_civil(int y, int m, int d) {
  y -= m <= 2;
  const int32_t era = (y >= 0 ? y : y - 399) / 400;
  const uint32_t yoe = static_cast<uint32_t>(y - era * 400);
  const uint32_t doy = (153 * static_cast<uint32_t>(m + (m > 2 ? -3 : 9)) + 2) / 5 + static_cast<uint32_t>(d) - 1;
  const uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int32_t>(doe) - 719468;
}

struct Ymd {
  int y, m, d;
};

inline Ymd civil_from_days(int32_t z) {
  z += 719468;
  const int32_t era = (z >= 0 ? z : z - 146096) / 146097;
  const uint32_t doe = static_cast<uint32_t>(z - era * 146097);
  const uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const uint32_t mp = (5 * doy + 2) / 153;
  const int d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
  const int m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
  return Ymd{static_cast<int>(yoe) + era * 400 + (m <= 2), m, d};
}

inline int weekday(int32_t day) { return static_cast<int>(floor_mod(day + 3, 7)); }  // Monday = 0

inline bool is_leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

inline int days_in_month(int y, int m) {
  static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  return m == 2 && is_leap(y) ? 29 : kDays[m - 1];
}

// Exactly "YYYY-MM-DD" (FRED and Open-Meteo dates); INT32_MIN otherwise.
inline int32_t parse_iso_date(const char* s) {
  int v[3] = {0, 0, 0};
  const int widths[3] = {4, 2, 2};
  for (int part = 0; part < 3; ++part) {
    for (int i = 0; i < widths[part]; ++i, ++s) {
      if (*s < '0' || *s > '9') return INT32_MIN;
      v[part] = v[part] * 10 + (*s - '0');
    }
    if (part < 2 && *s++ != '-') return INT32_MIN;
  }
  if (*s != '\0' || v[1] < 1 || v[1] > 12 || v[2] < 1 || v[2] > days_in_month(v[0], v[1])) return INT32_MIN;
  return days_from_civil(v[0], v[1], v[2]);
}

inline int32_t first_sunday_on_or_after(int32_t day) { return day + (6 - weekday(day) + 7) % 7; }

}  // namespace ink
```

- [ ] **Step 4: Implement `include/format.h`**

```cpp
#pragma once
// Python-compatible text for the widgets (spec §3.3): round-half-even like round(),
// "{:,.0f}" thousands separators, English names (never the C locale).
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace ink {

inline const char* const DAY_ABBR[7] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
inline const char* const DAY_NAME[7] = {"Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"};
inline const char* const MONTH_ABBR[13] = {"", "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                           "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
inline const char* const MONTH_NAME[13] = {"", "January", "February", "March", "April", "May", "June",
                                           "July", "August", "September", "October", "November", "December"};

// deg() in calendar_weather.py: int(round(v)) with banker's rounding, never "-0°".
inline void fmt_deg(char* out, size_t n, double v) {
  long r = lrint(v);  // default rounding mode is to-nearest-even, like Python's round()
  snprintf(out, n, "%ld\xC2\xB0", r);
}

// f"{v:,.0f}"
inline void fmt_thousands0(char* out, size_t n, double v) {
  char raw[48];
  snprintf(raw, sizeof raw, "%.0f", v);
  const char* digits = raw[0] == '-' ? raw + 1 : raw;
  size_t len = strlen(digits);
  char tmp[64];
  size_t k = 0;
  if (raw[0] == '-') tmp[k++] = '-';
  for (size_t i = 0; i < len; ++i) {
    if (i > 0 && (len - i) % 3 == 0) tmp[k++] = ',';
    tmp[k++] = digits[i];
  }
  tmp[k] = '\0';
  snprintf(out, n, "%s", tmp);
}

// compositor.py footer_time_text: "10:00 AM"
inline void fmt_clock(char* out, size_t n, int hh, int mm) {
  snprintf(out, n, "%d:%02d %s", hh % 12 == 0 ? 12 : hh % 12, mm, hh < 12 ? "AM" : "PM");
}

}  // namespace ink
```

- [ ] **Step 5: Run the data tests**

Run: `pio test -e native -f test_data`
Expected: PASS (5 new tests).

- [ ] **Step 6: Write the failing PNG/CRC tests**

Add to `test/test_render/test_main.cpp`:

```cpp
#include "crc32.h"
#include "render/png.h"

void test_crc32_known_value() {
  const char* s = "123456789";
  TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, ink::crc32(reinterpret_cast<const uint8_t*>(s), 9));
}

void test_png_header_and_size() {
  uint8_t bits[2 * 3] = {0xFF, 0xC0, 0x00, 0x00, 0xAA, 0x40};  // 10×3
  std::vector<uint8_t> png = ink::png_encode(bits, 10, 3);
  const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  TEST_ASSERT_EQUAL_MEMORY(sig, png.data(), 8);
  TEST_ASSERT_EQUAL_MEMORY("IHDR", png.data() + 12, 4);
  // 8 sig + IHDR(25) + IDAT(12 + 2 zlib hdr + 5 block hdr + 3 rows × 3 bytes + 4 adler) + IEND(12)
  TEST_ASSERT_EQUAL_UINT32(8 + 25 + 12 + 2 + 5 + 9 + 4 + 12, png.size());
}

void test_png_matches_python_decoder() {
  // Byte-exact against a file Pillow decodes; regenerate the expectation only if png.h changes.
  uint8_t bits[2 * 3] = {0xFF, 0xC0, 0x00, 0x00, 0xAA, 0x40};
  std::vector<uint8_t> png = ink::png_encode(bits, 10, 3);
  TEST_ASSERT_TRUE(ink_test::write_file(ink_test::path("reference/png_probe.png"), png.data(), png.size()));
}
```

`RUN_TEST` all three. Then verify the probe with Pillow (one-off, prints the pixels):

```bash
mkdir -p test/reference
pio test -e native -f test_render
uv run --with pillow==12.3.0 python -c "
from PIL import Image; im = Image.open('test/reference/png_probe.png'); print(im.mode, im.size, list(im.getdata()))"
```
Expected: `1 (10, 3) [255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 255, 0, 255, 0, 255, 0, 255, 0, 0, 255]`. (Row 0: 0xFF,0xC0 = 10 white; row 1 all black; row 2: 0xAA,0x40.) Then `rm test/reference/png_probe.png` and remove `test_png_matches_python_decoder` (it was only the one-off decoder check).

- [ ] **Step 7: Implement `include/crc32.h` and `include/render/png.h`**

`include/crc32.h`:

```cpp
#pragma once
// CRC-32 (IEEE, as zlib/PNG): frame change detection, cache files, PNG chunks.
#include <stddef.h>
#include <stdint.h>

namespace ink {

inline uint32_t crc32_update(uint32_t crc, const uint8_t* p, size_t n) {
  crc = ~crc;
  for (size_t i = 0; i < n; ++i) {
    crc ^= p[i];
    for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

inline uint32_t crc32(const uint8_t* p, size_t n) { return crc32_update(0, p, n); }

}  // namespace ink
```

`include/render/png.h`:

```cpp
#pragma once
// 1-bit grayscale PNG with stored (uncompressed) deflate blocks: no zlib, and the bytes
// depend only on the pixels, so goldens compare by file content. Host-side only.
#include <stddef.h>
#include <stdint.h>

#include <vector>

#include "crc32.h"

namespace ink {

namespace png_detail {
inline void put32(std::vector<uint8_t>& v, uint32_t x) {
  for (int s = 24; s >= 0; s -= 8) v.push_back(static_cast<uint8_t>(x >> s));
}
inline void chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data) {
  put32(out, static_cast<uint32_t>(data.size()));
  size_t start = out.size();
  out.insert(out.end(), type, type + 4);
  out.insert(out.end(), data.begin(), data.end());
  put32(out, crc32(out.data() + start, out.size() - start));
}
}  // namespace png_detail

// bits: h rows of (w + 7) / 8 bytes, MSB = leftmost pixel, bit 1 = white (PNG gray 1-bit agrees).
inline std::vector<uint8_t> png_encode(const uint8_t* bits, int w, int h) {
  using namespace png_detail;
  const size_t stride = static_cast<size_t>(w + 7) / 8;
  std::vector<uint8_t> raw;
  raw.reserve((stride + 1) * static_cast<size_t>(h));
  for (int y = 0; y < h; ++y) {
    raw.push_back(0);  // filter: none
    raw.insert(raw.end(), bits + y * stride, bits + (y + 1) * stride);
  }
  std::vector<uint8_t> z{0x78, 0x01};
  size_t off = 0;
  do {
    size_t n = raw.size() - off;
    if (n > 65535) n = 65535;
    const bool last = off + n == raw.size();
    const uint16_t len = static_cast<uint16_t>(n), nlen = static_cast<uint16_t>(~len);
    z.insert(z.end(), {static_cast<uint8_t>(last ? 1 : 0), static_cast<uint8_t>(len), static_cast<uint8_t>(len >> 8),
                       static_cast<uint8_t>(nlen), static_cast<uint8_t>(nlen >> 8)});
    z.insert(z.end(), raw.begin() + off, raw.begin() + off + n);
    off += n;
  } while (off < raw.size());
  uint32_t a = 1, b = 0;
  for (uint8_t c : raw) {
    a = (a + c) % 65521;
    b = (b + a) % 65521;
  }
  put32(z, (b << 16) | a);
  std::vector<uint8_t> out{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  std::vector<uint8_t> ihdr;
  put32(ihdr, static_cast<uint32_t>(w));
  put32(ihdr, static_cast<uint32_t>(h));
  ihdr.insert(ihdr.end(), {1, 0, 0, 0, 0});  // bit depth 1, grayscale, deflate, filter 0, no interlace
  chunk(out, "IHDR", ihdr);
  chunk(out, "IDAT", z);
  chunk(out, "IEND", {});
  return out;
}

}  // namespace ink
```

- [ ] **Step 8: Add golden and reference helpers to `test/support/ink_test.h`**

Append inside `namespace ink_test` (and add `#include "render/png.h"` at the top):

```cpp
// Compare with test/goldens/<name>.png (byte-exact: png_encode is deterministic).
inline void golden(const char* name, const uint8_t* bits, int w, int h) {
  const std::string p = path("goldens/") + name + ".png";
  const std::vector<uint8_t> png = ink::png_encode(bits, w, h);
  if (update_goldens()) {
    TEST_ASSERT_TRUE_MESSAGE(write_file(p, png.data(), png.size()), p.c_str());
    return;
  }
  std::string want;
  if (!read_file(p, want)) {
    TEST_FAIL_MESSAGE(("missing golden " + p + ": run INKBOARD_UPDATE_GOLDENS=1 pio test -e native, then look at it").c_str());
  }
  if (want.size() != png.size() || memcmp(want.data(), png.data(), png.size()) != 0) {
    const std::string actual = path("goldens/") + name + ".actual.png";
    write_file(actual, png.data(), png.size());
    TEST_FAIL_MESSAGE((std::string(name) + " differs from its golden; wrote " + actual).c_str());
  }
}

// Binary PBM (P4, bit 1 = black, as Pillow writes it) -> frame-style bits (bit 1 = white).
inline bool read_pbm(const std::string& p, std::vector<uint8_t>& bits, int& w, int& h) {
  std::string s;
  if (!read_file(p, s) || s.compare(0, 2, "P4") != 0) return false;
  size_t i = 2;
  int vals[2];
  for (int& v : vals) {
    while (i < s.size() && (isspace(static_cast<unsigned char>(s[i])) || s[i] == '#')) {
      if (s[i] == '#') while (i < s.size() && s[i] != '\n') ++i;
      else ++i;
    }
    v = 0;
    while (i < s.size() && isdigit(static_cast<unsigned char>(s[i]))) v = v * 10 + (s[i++] - '0');
  }
  ++i;  // the single whitespace byte before the raster
  w = vals[0];
  h = vals[1];
  const size_t n = static_cast<size_t>((w + 7) / 8) * h;
  if (s.size() < i + n) return false;
  bits.assign(s.begin() + i, s.begin() + i + n);
  for (uint8_t& b : bits) b = static_cast<uint8_t>(~b);
  return true;
}

// Pixels that differ from test/<rel_pbm>, ignoring padding bits; -1 if missing or a size
// mismatch. On a difference writes <rel_pbm>.cpp.png next to it for side-by-side review.
inline long match_reference(const char* rel_pbm, const uint8_t* bits, int w, int h) {
  std::vector<uint8_t> want;
  int ww, hh;
  if (!read_pbm(path(rel_pbm), want, ww, hh) || ww != w || hh != h) return -1;
  const int stride = (w + 7) / 8;
  long diff = 0;
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const uint8_t m = static_cast<uint8_t>(0x80 >> (x & 7));
      if ((want[y * stride + x / 8] & m) != (bits[y * stride + x / 8] & m)) ++diff;
    }
  if (diff) {
    const std::vector<uint8_t> png = ink::png_encode(bits, w, h);
    write_file(path(rel_pbm) + ".cpp.png", png.data(), png.size());
  }
  return diff;
}
```

Also add `#include <ctype.h>` at the top.

- [ ] **Step 9: Run all native tests**

Run: `pio test -e native`
Expected: all suites PASS.

- [ ] **Step 10: Commit**

```bash
git add include/civil.h include/format.h include/crc32.h include/render/png.h test/support/ink_test.h test/test_data/test_main.cpp test/test_render/test_main.cpp
git commit -m "Render: civil dates, Python-compatible formatting, CRC-32 and a zlib-free PNG writer

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

### Task 4: Canvas primitives with Pillow parity

The server draws on a Pillow `"L"` image with fills 0/255 and thresholds at 140. Pillow's
primitives are not anti-aliased, so a faithful port of its rasterizers reproduces the server's
pixels. The reference images are produced by Pillow itself; the C++ must match them **byte for
byte**.

Facts checked against Pillow 12.3.0 (`ImageDraw.py`): `line(..., joint="curve")` only adds joints
when `width > 4`; the server's widest line is 3, so curve joints are **not** needed. Pillow's
`rounded_rectangle` and the joint code are Python; everything else is C in `src/libImaging/Draw.c`
called through `src/_imaging.c` (`_draw_lines`, `_draw_ellipse`, `_draw_polygon`,
`_draw_rectangle`, `_draw_points`).

**Files:**
- Create: `tools/ref_primitives.py`, `include/render/canvas.h`
- Create (generated, committed): `test/reference/primitives/*.pbm`, `test/test_render/prim_cases.h`
- Test: `test/test_render/test_main.cpp`

**Interfaces:**
- Consumes: `ink_test::match_reference` (Task 3).
- Produces (`namespace ink`, `render/canvas.h`):

```cpp
constexpr int FRAME_W = 800, FRAME_H = 480, FRAME_BYTES = 48000;
constexpr int NONE = -1, BLACK = 0, WHITE = 255;
constexpr int INK_THRESHOLD = 140;          // frame.py: a value > 140 is white
struct Box { int x, y, w, h; };
struct Pt { double x, y; };

class Bitmap {                               // 1-bit, rows of (w + 7) / 8 bytes, bit 1 = white
 public:
  Bitmap(uint8_t* bits, int w, int h);
  int w() const; int h() const; int stride() const;
  uint8_t* bits(); const uint8_t* bits() const;
  void fill(int ink);                        // whole bitmap
  bool is_black(int x, int y) const;
  void set(int x, int y, int ink);           // ignores out-of-range; ink <= 140 is black
};

class View {                                 // a box of a Bitmap: local coordinates, clipped
 public:
  View(Bitmap& bm, Box box);
  int w() const; int h() const;
  View sub(Box local) const;                 // nested view, clipped to this one
  void px(int x, int y, int ink);            // one pixel, clipped
  void fill(int ink);                        // Image.paste(ink, box)
  void point(double x, double y, int ink);                                   // ImageDraw.point
  void line(const Pt* pts, int n, int ink, int width = 1);                   // ImageDraw.line
  void line(double x0, double y0, double x1, double y1, int ink, int width = 1);
  void rectangle(double x0, double y0, double x1, double y1, int fill, int outline = NONE, int width = 1);
  void ellipse(double x0, double y0, double x1, double y1, int fill, int outline = NONE, int width = 1);
  void polygon(const Pt* pts, int n, int fill, int outline = NONE, int width = 1);
  void rounded_rectangle(double x0, double y0, double x1, double y1, double radius, int fill,
                         int outline = NONE, int width = 1);
};
```

- [ ] **Step 1: Get Pillow's source for reference**

Run (scratch, not committed):
```bash
mkdir -p /tmp/claude-1000/pillow && curl -sL https://github.com/python-pillow/Pillow/archive/refs/tags/12.3.0.tar.gz \
  | tar xz -C /tmp/claude-1000/pillow && ls /tmp/claude-1000/pillow/Pillow-12.3.0/src/libImaging/Draw.c
```
Expected: the path is printed. Read `src/PIL/ImageDraw.py` (`line`, `point`, `rectangle`, `ellipse`, `polygon`, `rounded_rectangle`), `src/_imaging.c` (`_draw_*`: how float coordinates become ints), and `src/libImaging/Draw.c` (`ImagingDrawLine`, `ImagingDrawWideLine`, `ImagingDrawRectangle`, `ImagingDrawPolygon` and its polygon filler, `ImagingDrawEllipse` and the ellipse/quarter iterators, `ImagingDrawPoint`).

- [ ] **Step 2: Write the reference generator**

Create `tools/ref_primitives.py`:

```python
# /// script
# requires-python = ">=3.11"
# dependencies = ["pillow==12.3.0"]
# ///
"""Reference images for include/render/canvas.h (plan Task 4).

Each case is drawn by Pillow the way the server drew (mode "L", fills 0/255, threshold 140,
then mode "1"), saved as test/reference/primitives/<name>.pbm, and listed in
test/test_render/prim_cases.h so the C++ test replays exactly the same operations.
Run: uv run tools/ref_primitives.py
"""
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "test/reference/primitives"
HEADER = ROOT / "test/test_render/prim_cases.h"
W, H = 120, 80
THRESHOLD = 140

# (kind, numbers, fill, outline, width). fill/outline: 0 black, 255 white, -1 none.
# line: x0 y0 x1 y1 [x2 y2 ...]; points: x_from x_to step y (range(int, int, int) at y);
# rectangle/ellipse: x0 y0 x1 y1; polygon: x0 y0 x1 y1 ...; rounded_rectangle: x0 y0 x1 y1 r.
BLACK_BG = ("rectangle", [0, 0, W - 1, H - 1], 0, -1, 1)
CASES = {
    "line_w1_diag": [("line", [5.5, 10.2, 110.7, 70.9], 0, -1, 1)],
    "line_w2_diag": [("line", [5.5, 10.2, 110.7, 70.9], 0, -1, 2)],
    "line_w3_diag": [("line", [5.5, 70.2, 110.7, 10.9], 0, -1, 3)],
    "line_w5_steep": [("line", [40.3, 5.0, 60.8, 75.0], 0, -1, 5)],
    "line_w2_horizontal": [("line", [10, 40, 110, 40], 0, -1, 2)],
    "line_w4_vertical": [("line", [60, 5, 60, 75], 0, -1, 4)],
    "line_short_dash": [("line", [20.25, 30.5, 26.75, 33.1], 0, -1, 2)],
    "line_white_on_black": [BLACK_BG, ("line", [5, 5, 115, 75], 255, -1, 3)],
    "polyline_w1": [("line", [10, 60, 40, 20.5, 70.2, 65, 110, 15], 0, -1, 1)],
    "polyline_w2": [("line", [10, 60, 40, 20.5, 70.2, 65, 110, 15], 0, -1, 2)],
    "polyline_w3": [("line", [10, 60, 40, 20.5, 70.2, 65, 110, 15], 0, -1, 3)],
    "polyline_icon": [("line", [70.4, 40.2, 66.6, 48.7, 74.4, 48.7, 70.6, 58.9], 0, -1, 3)],
    "points_dotted": [("points", [10, 110, 6, 40.5], 0, -1, 1)],
    "rect_fill_float": [("rectangle", [10.4, 10.6, 30.2, 30.9], 0, -1, 1)],
    "rect_white_on_black": [BLACK_BG, ("rectangle", [20, 20, 60, 50], 255, -1, 1)],
    "ellipse_outline_w3": [("ellipse", [10, 10, 90, 60], -1, 0, 3)],
    "ellipse_outline_float_w4": [("ellipse", [10.3, 5.6, 50.9, 44.2], -1, 0, 4)],
    "ellipse_outline_w2_small": [("ellipse", [20.5, 20.5, 31.5, 31.5], -1, 0, 2)],
    "ellipse_fill_black": [("ellipse", [80, 40, 119, 79], 0, -1, 1)],
    "ellipse_marker": [("line", [0, 40, 119, 40], 0, -1, 3), ("ellipse", [55, 35, 65, 45], 255, 0, 2)],
    "ellipse_cloud": [("ellipse", [20.6, 18.2, 60.6, 58.2], -1, 0, 3), ("ellipse", [23.6, 21.2, 57.6, 55.2], 255, -1, 1)],
    "polygon_triangle": [("polygon", [33.7, 36.2, 38.7, 45.2, 28.7, 45.2], 0, -1, 1)],
    "polygon_diamond": [("line", [0, 32, 119, 32], 0, -1, 2), ("polygon", [60, 20, 72, 32, 60, 44, 48, 32], 255, 0, 2)],
    "rounded_small": [("rounded_rectangle", [10.5, 10, 60.5, 32, 4], 0, -1, 1)],
    "rounded_float": [("rounded_rectangle", [70.3, 40.1, 110.6, 62.9, 4], 0, -1, 1)],
}


def apply(d: ImageDraw.ImageDraw, op) -> None:
    kind, v, fill, outline, width = op
    f = None if fill < 0 else fill
    o = None if outline < 0 else outline
    if kind == "line":
        d.line(list(zip(v[0::2], v[1::2])), fill=f, width=width)
    elif kind == "points":
        for x in range(int(v[0]), int(v[1]), int(v[2])):
            d.point((x, v[3]), fill=f)
    elif kind == "rectangle":
        d.rectangle(v, fill=f, outline=o, width=width)
    elif kind == "ellipse":
        d.ellipse(v, fill=f, outline=o, width=width)
    elif kind == "polygon":
        d.polygon(list(zip(v[0::2], v[1::2])), fill=f, outline=o, width=width)
    elif kind == "rounded_rectangle":
        d.rounded_rectangle(v[:4], v[4], fill=f, outline=o, width=width)
    else:
        raise ValueError(kind)


def c_double(x: float) -> str:
    return repr(float(x))


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    rows = []
    for name, ops in CASES.items():
        img = Image.new("L", (W, H), 255)
        d = ImageDraw.Draw(img)
        for op in ops:
            apply(d, op)
        img.point(lambda p: 255 if p > THRESHOLD else 0).convert("1").save(OUT / f"{name}.pbm")
        cops = ", ".join(
            "{\"%s\", %d, {%s}, %d, %d, %d}" % (k, len(v), ", ".join(c_double(x) for x in v), fi, ou, wi)
            for k, v, fi, ou, wi in ops)
        rows.append("    {\"%s\", %d, {%s}}," % (name, len(ops), cops))
    HEADER.write_text(
        "#pragma once\n// Generated by tools/ref_primitives.py; do not edit.\n"
        "struct PrimOp { const char* kind; int n; double v[16]; int fill, outline, width; };\n"
        "struct PrimCase { const char* name; int n_ops; PrimOp ops[3]; };\n"
        f"static const int PRIM_W = {W}, PRIM_H = {H};\n"
        "static const PrimCase PRIM_CASES[] = {\n" + "\n".join(rows) + "\n};\n")
    print(f"wrote {len(CASES)} cases")


if __name__ == "__main__":
    main()
```

Run: `uv run tools/ref_primitives.py`
Expected: `wrote 25 cases`; `test/reference/primitives/` holds 25 `.pbm` files; `test/test_render/prim_cases.h` exists.

- [ ] **Step 3: Write the failing test**

Add to `test/test_render/test_main.cpp`:

```cpp
#include "render/canvas.h"
#include "prim_cases.h"

static void replay(ink::View& v, const PrimOp& op) {
  using ink::Pt;
  const std::string k = op.kind;
  if (k == "line") {
    Pt pts[8];
    for (int i = 0; i < op.n / 2; ++i) pts[i] = Pt{op.v[2 * i], op.v[2 * i + 1]};
    v.line(pts, op.n / 2, op.fill, op.width);
  } else if (k == "points") {
    for (int x = int(op.v[0]); x < int(op.v[1]); x += int(op.v[2])) v.point(x, op.v[3], op.fill);
  } else if (k == "rectangle") {
    v.rectangle(op.v[0], op.v[1], op.v[2], op.v[3], op.fill, op.outline, op.width);
  } else if (k == "ellipse") {
    v.ellipse(op.v[0], op.v[1], op.v[2], op.v[3], op.fill, op.outline, op.width);
  } else if (k == "polygon") {
    Pt pts[8];
    for (int i = 0; i < op.n / 2; ++i) pts[i] = Pt{op.v[2 * i], op.v[2 * i + 1]};
    v.polygon(pts, op.n / 2, op.fill, op.outline, op.width);
  } else if (k == "rounded_rectangle") {
    v.rounded_rectangle(op.v[0], op.v[1], op.v[2], op.v[3], op.v[4], op.fill, op.outline, op.width);
  } else {
    TEST_FAIL_MESSAGE(op.kind);
  }
}

void test_primitives_match_pillow() {
  int failed = 0;
  for (const PrimCase& c : PRIM_CASES) {
    std::vector<uint8_t> bits(static_cast<size_t>((PRIM_W + 7) / 8) * PRIM_H);
    ink::Bitmap bm(bits.data(), PRIM_W, PRIM_H);
    bm.fill(ink::WHITE);
    ink::View v(bm, ink::Box{0, 0, PRIM_W, PRIM_H});
    for (int i = 0; i < c.n_ops; ++i) replay(v, c.ops[i]);
    long diff = ink_test::match_reference((std::string("reference/primitives/") + c.name + ".pbm").c_str(),
                                          bits.data(), PRIM_W, PRIM_H);
    if (diff != 0) {
      printf("primitive %s: %ld pixels differ (see test/reference/primitives/%s.pbm.cpp.png)\n", c.name, diff, c.name);
      ++failed;
    }
  }
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, failed, "primitives differ from Pillow");
}

void test_view_clips_and_translates() {
  std::vector<uint8_t> bits(static_cast<size_t>(10 / 8 + 1) * 10, 0xFF);
  ink::Bitmap bm(bits.data(), 10, 10);
  ink::View v(bm, ink::Box{2, 3, 4, 4});
  v.rectangle(-5, -5, 50, 50, ink::BLACK);
  for (int y = 0; y < 10; ++y)
    for (int x = 0; x < 10; ++x)
      TEST_ASSERT_EQUAL(x >= 2 && x < 6 && y >= 3 && y < 7, bm.is_black(x, y));
  ink::View inner = v.sub(ink::Box{1, 1, 10, 10});  // clipped to the parent
  TEST_ASSERT_EQUAL_INT(3, inner.w());
  TEST_ASSERT_EQUAL_INT(3, inner.h());
}
```

`RUN_TEST` both. Run: `pio test -e native -f test_render`
Expected: compile error (`render/canvas.h` missing).

- [ ] **Step 4: Implement `include/render/canvas.h`**

Start from this skeleton; the bodies marked "port" are translated from the Pillow 12.3.0 source named in each comment, keeping its integer conversions and loop order exactly (that is what makes the pixels match):

```cpp
#pragma once
// 1-bit drawing with Pillow-identical rasterization (spec §3.2): the server drew with
// ImageDraw on an "L" image, so porting Pillow's rasterizers keeps the look pixel for pixel.
// Pure; host-tested against Pillow's own output (test/reference/primitives).
#include <math.h>
#include <stdint.h>
#include <string.h>

namespace ink {

constexpr int FRAME_W = 800, FRAME_H = 480, FRAME_BYTES = 48000;
constexpr int NONE = -1, BLACK = 0, WHITE = 255;
constexpr int INK_THRESHOLD = 140;

struct Box {
  int x, y, w, h;
};
struct Pt {
  double x, y;
};

class Bitmap {
 public:
  Bitmap(uint8_t* bits, int w, int h) : bits_(bits), w_(w), h_(h), stride_((w + 7) / 8) {}
  int w() const { return w_; }
  int h() const { return h_; }
  int stride() const { return stride_; }
  uint8_t* bits() { return bits_; }
  const uint8_t* bits() const { return bits_; }
  void fill(int ink) { memset(bits_, ink <= INK_THRESHOLD ? 0x00 : 0xFF, static_cast<size_t>(stride_) * h_); }
  bool is_black(int x, int y) const { return (bits_[y * stride_ + x / 8] & (0x80 >> (x & 7))) == 0; }
  void set(int x, int y, int ink) {
    if (x < 0 || y < 0 || x >= w_ || y >= h_) return;
    uint8_t& b = bits_[y * stride_ + x / 8];
    const uint8_t m = static_cast<uint8_t>(0x80 >> (x & 7));
    b = ink <= INK_THRESHOLD ? static_cast<uint8_t>(b & ~m) : static_cast<uint8_t>(b | m);
  }

 private:
  uint8_t* bits_;
  int w_, h_, stride_;
};

class View {
 public:
  View(Bitmap& bm, Box box) : bm_(bm), box_(clip_to(box, Box{0, 0, bm.w(), bm.h()})), ox_(box.x), oy_(box.y) {}
  int w() const { return box_.x + box_.w - ox_; }
  int h() const { return box_.y + box_.h - oy_; }
  View sub(Box local) const {
    View v(bm_, Box{ox_ + local.x, oy_ + local.y, local.w, local.h});
    v.box_ = clip_to(v.box_, box_);
    return v;
  }
  void px(int x, int y, int ink) {
    const int ax = x + ox_, ay = y + oy_;
    if (ax < box_.x || ay < box_.y || ax >= box_.x + box_.w || ay >= box_.y + box_.h) return;
    bm_.set(ax, ay, ink);
  }
  void fill(int ink) {
    for (int y = 0; y < h(); ++y)
      for (int x = 0; x < w(); ++x) px(x, y, ink);
  }

  // port: ImageDraw.point -> _draw_points -> ImagingDrawPoint (coordinates converted as _draw_points does)
  void point(double x, double y, int ink);
  // port: ImageDraw.line -> _draw_lines: width <= 1 -> ImagingDrawLine per segment,
  // else ImagingDrawWideLine per segment (no curve joints: Pillow adds them only when width > 4)
  void line(const Pt* pts, int n, int ink, int width = 1);
  void line(double x0, double y0, double x1, double y1, int ink, int width = 1) {
    const Pt p[2] = {{x0, y0}, {x1, y1}};
    line(p, 2, ink, width);
  }
  // port: ImageDraw.rectangle -> _draw_rectangle -> ImagingDrawRectangle (fill, then outline of `width`)
  void rectangle(double x0, double y0, double x1, double y1, int fill, int outline = NONE, int width = 1);
  // port: ImageDraw.ellipse -> _draw_ellipse -> ImagingDrawEllipse (Pillow >= 8 ellipse/quarter iterators)
  void ellipse(double x0, double y0, double x1, double y1, int fill, int outline = NONE, int width = 1);
  // port: ImageDraw.polygon -> _draw_polygon -> ImagingDrawPolygon (+ the width > 1 outline path in ImageDraw.polygon)
  void polygon(const Pt* pts, int n, int fill, int outline = NONE, int width = 1);
  // port: ImageDraw.rounded_rectangle (Python): pieslices for the corners + rectangles
  void rounded_rectangle(double x0, double y0, double x1, double y1, double radius, int fill,
                         int outline = NONE, int width = 1);

 private:
  static Box clip_to(Box a, Box b) {
    const int x0 = a.x > b.x ? a.x : b.x, y0 = a.y > b.y ? a.y : b.y;
    const int x1 = a.x + a.w < b.x + b.w ? a.x + a.w : b.x + b.w;
    const int y1 = a.y + a.h < b.y + b.h ? a.y + a.h : b.y + b.h;
    return Box{x0, y0, x1 > x0 ? x1 - x0 : 0, y1 > y0 ? y1 - y0 : 0};
  }
  // Horizontal span helper the ported fillers write through (local coordinates, clipped).
  void hspan(int x0, int x1, int y, int ink) {
    for (int x = x0; x <= x1; ++x) px(x, y, ink);
  }

  Bitmap& bm_;
  Box box_;      // absolute, already clipped
  int ox_, oy_;  // origin of local coordinates (unclipped box.x/box.y)
};

}  // namespace ink
```

Implement the six ported member functions below the class as `inline` definitions. Porting rules:
- Keep Pillow's exact float→int conversions (`_draw_*` in `_imaging.c` and the `ImagingDraw*` signatures decide where truncation or rounding happens). Do not "improve" them.
- Ellipse: port `ellipse_init`/`quarter_*`/`ellipse_new` (or whatever 12.3.0 names them) including the separate fill and outline passes and the `width` handling.
- Polygon fill: port the scanline filler used by `ImagingDrawPolygon` (edge list, `x_intersect` rounding).
- Pillow's `ImageDraw.polygon` with `width > 1` and an outline draws the outline via a mask; port that path too (`polygon_diamond` covers it).
- `rounded_rectangle`: port the Python method; its pieslices use `ImagingDrawPieslice` → port that too.

- [ ] **Step 5: Iterate until all cases match**

Run: `pio test -e native -f test_render`
Expected at first: some `primitive <name>: N pixels differ` lines. Open `test/reference/primitives/<name>.pbm.cpp.png` next to the `.pbm` (e.g. `uv run --with pillow python -c "from PIL import Image; Image.open('test/reference/primitives/<name>.pbm').save('/tmp/claude-1000/<name>.png')"`), fix the port, rerun. Done when: `test_primitives_match_pillow` and `test_view_clips_and_translates` PASS with 0 differing pixels for all 25 cases.

- [ ] **Step 6: Commit**

```bash
git add tools/ref_primitives.py include/render/canvas.h test/reference/primitives test/test_render/prim_cases.h test/test_render/test_main.cpp
git commit -m "Render: 1-bit canvas with Pillow-identical primitives, checked against Pillow's own output

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

### Task 5: Fonts and text

The server renders text with `ImageDraw.fontmode = "1"` (FreeType monochrome) and Pillow's
**raqm** layout engine (checked: `features.check("raqm")` is `True` in the server's environment,
`layout_engine == 1`). The generator therefore takes everything (glyph bitmaps, offsets, advances,
kerning) from Pillow's own API with that engine, and the C++ reproduces Pillow's placement.

**Files:**
- Move: `server/inkboard_server/fonts/*` → `tools/fonts/` (`git mv`; the server keeps working because Step 1 points its `FONT_DIR` there)
- Create: `tools/gen_fonts.py`, `include/render/font.h`, `include/render/text.h`
- Create (generated, committed): `include/render/fonts/dejavu_*.h`, `include/render/fonts/all.h`, `test/reference/text/*.pbm`, `test/test_render/text_cases.h`
- Modify: `server/inkboard_server/draw/fonts.py` (`FONT_DIR`)
- Test: `test/test_render/test_main.cpp`

**Interfaces:**
- Consumes: `View`, `BLACK`/`WHITE` (Task 4); `match_reference` (Task 3).
- Produces (`namespace ink`):

```cpp
// render/font.h
struct Glyph {
  uint16_t cp;         // Unicode code point
  int16_t dx, dy;      // top-left of the ink box relative to the pen on the baseline (y down)
  uint8_t w, h;        // ink box size; 0×0 for blank glyphs (space)
  int32_t adv64;       // advance in 1/64 px (Pillow getlength() * 64)
  uint32_t offset;     // first byte in Font::bits; rows of (w + 7) / 8 bytes, MSB left, bit 1 = ink
};
struct Kern { uint16_t left, right; int16_t adj64; };   // sorted by (left, right)
struct Font {
  uint8_t size; bool bold;
  int16_t ascent, descent;          // ImageFont.getmetrics()
  const Glyph* glyphs; uint16_t n_glyphs;              // sorted by cp
  const uint8_t* bits;
  const Kern* kern; uint16_t n_kern;
};
const Font* find_font(int size, bool bold);   // nullptr if not generated
const Font& font(int size, bool bold = false);  // the generated font; aborts in tests if missing
const Glyph* find_glyph(const Font& f, uint32_t cp);   // nullptr -> drawn as nothing, advance 0
// render/text.h
double text_length(const Font& f, const char* utf8);  // == ImageFont.getlength(text)
void draw_text(View& v, double x, double y, const char* utf8, const Font& f, int ink, const char* anchor = "la");
```

- [ ] **Step 1: Move the fonts and keep the server working**

```bash
git mv server/inkboard_server/fonts tools/fonts
```
Edit `server/inkboard_server/draw/fonts.py`: `FONT_DIR = Path(__file__).resolve().parents[3] / "tools" / "fonts"`.
Run: `just test` (server tests). Expected: all pass (goldens unchanged).

- [ ] **Step 2: Write the generator**

Create `tools/gen_fonts.py`:

```python
# /// script
# requires-python = ">=3.11"
# dependencies = ["pillow==12.3.0"]
# ///
"""Bitmap fonts for include/render/fonts/ from DejaVu (plan Task 5, spec §3.1).

Everything comes from Pillow with the same settings the server used (fontmode "1", default
layout engine = raqm), so the C++ text matches the server's. Also writes reference images
for test/test_render (text_cases.h + test/reference/text/*.pbm).
Run: uv run tools/gen_fonts.py
"""
from itertools import product
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont, features

ROOT = Path(__file__).resolve().parent.parent
TTF = ROOT / "tools" / "fonts"
OUT = ROOT / "include" / "render" / "fonts"
REF = ROOT / "test" / "reference" / "text"
CASES_H = ROOT / "test" / "test_render" / "text_cases.h"

REGULAR = [10, 11, 12, 13, 14, 15, 18]
BOLD = [8, 11, 12, 13, 14, 15, 17, 18, 20, 22, 26, 36, 46]
CHARS = [chr(c) for c in range(0x20, 0x7F)] + ["°", "×", "·", "—", "⚠"]

TEXT_CASES = [  # (size, bold, text, anchor) - every anchor and the special characters the widgets use
    (46, True, "73°", "la"), (14, False, "Mostly clear", "la"), (14, True, "H 89°  L 67°", "la"),
    (15, True, "Wed", "lm"), (15, False, "62°", "rm"), (13, False, "27", "mm"), (13, True, "27", "mm"),
    (11, False, "1.5×", "rm"), (11, False, "2024", "mt"), (11, False, "now", "rb"),
    (11, False, "as of Aug 1", "ra"), (10, False, "updated 10:00 AM   ⚠ stale   fw 2.0.0", "lm"),
    (10, False, "FRED · S&P DJI · Coinbase", "rm"), (22, True, "Markets", "la"),
    (12, False, "5-yr, × own average, log", "la"), (13, True, "S&P 500 (since 2021)", "lm"),
    (13, False, "$84.6k", "rm"), (17, True, "*", "mm"), (8, True, "*", "mm"), (14, True, "No data yet: fred", "mm"),
    (20, True, "Sun, September 27", "la"), (26, True, "Sep 27, 2026", "la"), (12, False, "H —  L —", "la"),
    (36, True, "inkboard calibration", "mm"), (18, False, "AV Ty To WA", "la"),
]
REF_W, REF_H, REF_X, REF_Y = 520, 90, 260, 45


def ttf(bold: bool) -> Path:
    return TTF / ("DejaVuSans-Bold.ttf" if bold else "DejaVuSans.ttf")


def ident(size: int, bold: bool) -> str:
    return f"dejavu_sans{'_bold' if bold else ''}_{size}"


def glyph(f: ImageFont.FreeTypeFont, ch: str):
    """Ink box of one character drawn at the pen origin (anchor "ls") the way ImageDraw.text does."""
    img = Image.new("1", (4 * f.size + 64, 4 * f.size + 64), 0)
    d = ImageDraw.Draw(img)
    d.fontmode = "1"
    ox, oy = 2 * f.size, 3 * f.size
    d.text((ox, oy), ch, font=f, fill=1, anchor="ls")
    box = img.getbbox()
    if box is None:
        return 0, 0, 0, 0, b""
    x0, y0, x1, y1 = box
    crop = img.crop(box)
    rows = []
    for y in range(crop.height):
        row = 0
        bits = bytearray((crop.width + 7) // 8)
        for x in range(crop.width):
            if crop.getpixel((x, y)):
                bits[x // 8] |= 0x80 >> (x % 8)
        rows.append(bytes(bits))
    return x0 - ox, y0 - oy, crop.width, crop.height, b"".join(rows)


def font_header(size: int, bold: bool) -> str:
    f = ImageFont.truetype(str(ttf(bold)), size)
    name = ident(size, bold)
    ascent, descent = f.getmetrics()
    bits, glyphs = bytearray(), []
    for ch in CHARS:
        dx, dy, w, h, data = glyph(f, ch)
        assert w < 256 and h < 256, (name, ch)
        glyphs.append((ord(ch), dx, dy, w, h, round(f.getlength(ch) * 64), len(bits)))
        bits += data
    kerns = []
    for a, b in product(CHARS, CHARS):
        adj = round((f.getlength(a + b) - f.getlength(a) - f.getlength(b)) * 64)
        if adj:
            kerns.append((ord(a), ord(b), adj))
    glyphs.sort()
    kerns.sort()
    hexbytes = ",".join(f"0x{x:02x}" for x in bits) or "0"
    g = ",\n    ".join("{%d, %d, %d, %d, %d, %d, %d}" % x for x in glyphs)
    k = ",\n    ".join("{%d, %d, %d}" % x for x in kerns) or "{0, 0, 0}"
    return (
        "#pragma once\n// Generated by tools/gen_fonts.py from DejaVu Sans (tools/fonts/LICENSE); do not edit.\n"
        '#include "render/font.h"\n\nnamespace ink::fonts {\n\n'
        f"inline constexpr uint8_t {name}_bits[] = {{{hexbytes}}};\n"
        f"inline constexpr Glyph {name}_glyphs[] = {{\n    {g}}};\n"
        f"inline constexpr Kern {name}_kern[] = {{\n    {k}}};\n"
        f"inline constexpr Font {name} = {{{size}, {'true' if bold else 'false'}, {ascent}, {descent}, "
        f"{name}_glyphs, {len(glyphs)}, {name}_bits, {name}_kern, {len(kerns) if kerns else 0}}};\n\n"
        "}  // namespace ink::fonts\n")


def main() -> None:
    assert features.check("raqm"), "Pillow without raqm lays text out differently from the server"
    OUT.mkdir(parents=True, exist_ok=True)
    names = []
    for bold, sizes in ((False, REGULAR), (True, BOLD)):
        for size in sizes:
            (OUT / f"{ident(size, bold)}.h").write_text(font_header(size, bold))
            names.append((size, bold))
    inc = "\n".join(f'#include "render/fonts/{ident(s, b)}.h"' for s, b in names)
    lst = ",\n    ".join(f"&{ident(s, b)}" for s, b in names)
    (OUT / "all.h").write_text(
        "#pragma once\n// Generated by tools/gen_fonts.py; do not edit.\n" + inc +
        "\n\nnamespace ink::fonts {\ninline constexpr const Font* ALL[] = {\n    " + lst + "};\n}  // namespace ink::fonts\n")
    REF.mkdir(parents=True, exist_ok=True)
    rows = []
    for i, (size, bold, text, anchor) in enumerate(TEXT_CASES):
        img = Image.new("L", (REF_W, REF_H), 255)
        d = ImageDraw.Draw(img)
        d.fontmode = "1"
        d.text((REF_X, REF_Y), text, font=ImageFont.truetype(str(ttf(bold)), size), fill=0, anchor=anchor)
        img.point(lambda p: 255 if p > 140 else 0).convert("1").save(REF / f"case_{i:02d}.pbm")
        # Octal escapes for non-ASCII bytes: C++ hex escapes are greedy ("\xb0C" would be one escape).
        lit = "".join(chr(b) if 32 <= b < 127 and chr(b) not in '"\\' else "\\%03o" % b for b in text.encode())
        rows.append('    {%d, %s, "%s", "%s", %r},' % (size, "true" if bold else "false", lit,
                                                         anchor, ImageFont.truetype(str(ttf(bold)), size).getlength(text)))
    CASES_H.write_text(
        "#pragma once\n// Generated by tools/gen_fonts.py; do not edit.\n"
        "struct TextCase { int size; bool bold; const char* text; const char* anchor; double length; };\n"
        f"static const int TEXT_W = {REF_W}, TEXT_H = {REF_H}, TEXT_X = {REF_X}, TEXT_Y = {REF_Y};\n"
        "static const TextCase TEXT_CASES[] = {\n" + "\n".join(rows) + "\n};\n")
    print(f"wrote {len(names)} fonts, {len(TEXT_CASES)} text cases")


if __name__ == "__main__":
    main()
```

Run: `uv run tools/gen_fonts.py`
Expected: `wrote 20 fonts, 25 text cases`. Check sizes: `du -ch include/render/fonts/*.h | tail -1` (a few MB of source text is fine; the compiled data is much smaller).

- [ ] **Step 3: Write the failing tests**

Add to `test/test_render/test_main.cpp`:

```cpp
#include "render/text.h"
#include "text_cases.h"

void test_every_widget_font_exists() {
  const int regular[] = {10, 11, 12, 13, 14, 15, 18};
  const int bold[] = {8, 11, 12, 13, 14, 15, 17, 18, 20, 22, 26, 36, 46};
  for (int s : regular) TEST_ASSERT_NOT_NULL(ink::find_font(s, false));
  for (int s : bold) TEST_ASSERT_NOT_NULL(ink::find_font(s, true));
  TEST_ASSERT_NULL(ink::find_font(9, false));
  TEST_ASSERT_NOT_NULL(ink::find_glyph(ink::font(10), 0x26A0));  // ⚠
}

void test_text_length_matches_pillow() {
  for (const TextCase& c : TEXT_CASES) {
    TEST_ASSERT_DOUBLE_WITHIN_MESSAGE(1.0 / 64, c.length, ink::text_length(ink::font(c.size, c.bold), c.text), c.text);
  }
}

void test_text_matches_pillow() {
  int failed = 0;
  for (size_t i = 0; i < sizeof(TEXT_CASES) / sizeof(TEXT_CASES[0]); ++i) {
    const TextCase& c = TEXT_CASES[i];
    std::vector<uint8_t> bits(static_cast<size_t>((TEXT_W + 7) / 8) * TEXT_H);
    ink::Bitmap bm(bits.data(), TEXT_W, TEXT_H);
    bm.fill(ink::WHITE);
    ink::View v(bm, ink::Box{0, 0, TEXT_W, TEXT_H});
    ink::draw_text(v, TEXT_X, TEXT_Y, c.text, ink::font(c.size, c.bold), ink::BLACK, c.anchor);
    char rel[64];
    snprintf(rel, sizeof rel, "reference/text/case_%02zu.pbm", i);
    long diff = ink_test::match_reference(rel, bits.data(), TEXT_W, TEXT_H);
    if (diff != 0) {
      printf("text case %zu (%s, %s): %ld pixels differ\n", i, c.text, c.anchor, diff);
      ++failed;
    }
  }
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, failed, "text differs from Pillow");
}
```

Run: `pio test -e native -f test_render` → Expected: compile error (`render/text.h` missing).

- [ ] **Step 4: Implement `include/render/font.h`**

```cpp
#pragma once
// Bitmap font format written by tools/gen_fonts.py (spec §3.1). Data lives in flash
// (constexpr arrays); C++17 inline variables keep one copy across translation units.
#include <stdint.h>
#include <stdlib.h>

namespace ink {

struct Glyph {
  uint16_t cp;
  int16_t dx, dy;
  uint8_t w, h;
  int32_t adv64;
  uint32_t offset;
};
struct Kern {
  uint16_t left, right;
  int16_t adj64;
};
struct Font {
  uint8_t size;
  bool bold;
  int16_t ascent, descent;
  const Glyph* glyphs;
  uint16_t n_glyphs;
  const uint8_t* bits;
  const Kern* kern;
  uint16_t n_kern;
};

}  // namespace ink

#include "render/fonts/all.h"

namespace ink {

inline const Font* find_font(int size, bool bold) {
  for (const Font* f : fonts::ALL)
    if (f->size == size && f->bold == bold) return f;
  return nullptr;
}

// Every size the widgets use is generated (test_every_widget_font_exists); a miss is a bug.
inline const Font& font(int size, bool bold = false) {
  const Font* f = find_font(size, bold);
  if (f == nullptr) abort();
  return *f;
}

inline const Glyph* find_glyph(const Font& f, uint32_t cp) {
  int lo = 0, hi = f.n_glyphs;
  while (lo < hi) {
    const int mid = (lo + hi) / 2;
    if (f.glyphs[mid].cp == cp) return &f.glyphs[mid];
    if (f.glyphs[mid].cp < cp) lo = mid + 1;
    else hi = mid;
  }
  return nullptr;
}

inline int32_t kern64(const Font& f, uint32_t left, uint32_t right) {
  int lo = 0, hi = f.n_kern;
  while (lo < hi) {
    const int mid = (lo + hi) / 2;
    const Kern& k = f.kern[mid];
    if (k.left == left && k.right == right) return k.adj64;
    if (k.left < left || (k.left == left && k.right < right)) lo = mid + 1;
    else hi = mid;
  }
  return 0;
}

}  // namespace ink
```

- [ ] **Step 5: Implement `include/render/text.h`**

Placement must follow Pillow's: read `src/_imagingft.c` in the Pillow source from Task 4 Step 1 (`text_layout_raqm`, `bounding_box_and_anchors`, `font_render`) and `src/PIL/ImageDraw.py` `text()` (how `xy` and the mask offset combine; `fontmode == "1"` path). Start from this skeleton:

```cpp
#pragma once
// Text measure and draw with Pillow's anchors (spec §3.2): "la" "lm" "rm" "mm" "ra" "rb" "mt".
#include <math.h>
#include <stdint.h>

#include "render/canvas.h"
#include "render/font.h"

namespace ink {

// Next code point of a UTF-8 string (advances s); invalid bytes decode as U+FFFD.
inline uint32_t next_cp(const char*& s) {
  const uint8_t c = static_cast<uint8_t>(*s++);
  if (c < 0x80) return c;
  int n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : -1;
  if (n < 0) return 0xFFFD;
  uint32_t cp = c & (0x3F >> n);
  while (n-- > 0) {
    const uint8_t d = static_cast<uint8_t>(*s);
    if ((d & 0xC0) != 0x80) return 0xFFFD;
    cp = (cp << 6) | (d & 0x3F);
    ++s;
  }
  return cp;
}

// Pen positions in 1/64 px: advance plus pair kerning, as measured from Pillow's raqm layout.
inline double text_length(const Font& f, const char* utf8) {
  int64_t pen = 0;
  uint32_t prev = 0;
  for (const char* s = utf8; *s;) {
    const uint32_t cp = next_cp(s);
    if (prev) pen += kern64(f, prev, cp);
    if (const Glyph* g = find_glyph(f, cp)) pen += g->adv64;
    prev = cp;
  }
  return pen / 64.0;
}

inline void draw_text(View& v, double x, double y, const char* utf8, const Font& f, int ink, const char* anchor = "la") {
  // 1. Anchor offset (port of bounding_box_and_anchors for a single horizontal line):
  //    horizontal 'l' = 0, 'm' = width / 2, 'r' = width (width = pen advance, in Pillow's rounding);
  //    vertical 'a' = ascent, 's' = baseline, 'd' = descent, 'm' = (ascent - descent) / 2 from the top,
  //    't' / 'b' = top / bottom of the line's ink box. Follow the C source for the exact integer math.
  // 2. Origin: ImageDraw.text computes the mask offset from xy and the anchor and pastes the mask;
  //    keep its rounding of fractional xy (the widgets pass floats such as y + rh / 2).
  // 3. Per glyph: pixel x = origin + (pen64 rounded as Pillow rounds) + dx, y = baseline + dy;
  //    set every ink bit with v.px(x, y, ink).
  (void)v; (void)x; (void)y; (void)utf8; (void)f; (void)ink; (void)anchor;
}

}  // namespace ink
```

Replace the comment block with the port. Note that `text_length` already passes `test_text_length_matches_pillow` if the generator's kerning pairs capture the layout; if a case fails by more than 1/64, the raqm layout is doing more than pair kerning for that string: report it to the user before changing the generator.

- [ ] **Step 6: Iterate until the text cases match**

Run: `pio test -e native -f test_render`
Expected at the end: `test_every_widget_font_exists`, `test_text_length_matches_pillow`, `test_text_matches_pillow` PASS (0 differing pixels in all 25 cases). Debug with `test/reference/text/case_NN.pbm.cpp.png` versus the `.pbm`.

If a case cannot reach 0 after porting the anchor math faithfully (e.g. raqm's sub-pixel positioning differs from the 64ths captured), stop and report the case and the pixel count to the user: the spec's acceptance is visual equivalence (spec §7), and the user decides whether a residual difference is acceptable.

- [ ] **Step 7: Build for the board to check flash size**

The firmware does not include the fonts until Task 18, so measure their compiled size with a scratch program:

```bash
cat > /tmp/claude-1000/fontsize.cpp <<'EOF'
#include "render/font.h"
int main() { int n = 0; for (const ink::Font* f : ink::fonts::ALL) n += f->n_glyphs; return n; }
EOF
g++ -std=gnu++17 -O2 -Iinclude /tmp/claude-1000/fontsize.cpp -o /tmp/claude-1000/fontsize && size /tmp/claude-1000/fontsize
```
Expected: the `text`+`data` total is well under 1 MB (the app slot is 1.9 MB). Note the number in the commit message.

- [ ] **Step 8: Commit**

```bash
git add tools/fonts tools/gen_fonts.py include/render/font.h include/render/text.h include/render/fonts \
  test/reference/text test/test_render/text_cases.h test/test_render/test_main.cpp server/inkboard_server/draw/fonts.py
git commit -m "Render: DejaVu bitmap fonts generated from Pillow, text placement matching the server

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

### Task 6: Time zones without a tz database

The board gets a POSIX TZ rule for the configured IANA zone from a generated table and evaluates it itself (no libc TZ, so the
host tests and the board run the same code). Expected values come from Python's `zoneinfo`.

**Files:**
- Create: `tools/gen_tz_table.py`, `include/tz.h`
- Create (generated, committed): `include/tz_table.h`, `test/test_data/tz_cases.h`
- Test: `test/test_data/test_main.cpp`

**Interfaces:**
- Consumes: `civil.h` (Task 3).
- Produces (`namespace ink::tz`):

```cpp
struct Entry { const char* name; const char* rule; };   // tz_table.h: TABLE[], sorted by name (strcmp)
const char* lookup(const char* iana);                     // POSIX rule or nullptr
struct Transition { char kind; int m, w, d; int day_n; int32_t time_s; };   // 'M', 'J', 'N'
struct Rule { int32_t std_offset, dst_offset; bool has_dst; Transition start, end; };  // offsets: seconds east of UTC
bool parse(const char* posix, Rule& out);
int32_t utc_offset(const Rule& r, int64_t epoch);
struct Local { int32_t day; int hh, mm, ss; int32_t offset; };
Local to_local(const Rule& r, int64_t epoch);
int64_t next_top_of_hour(const Rule& r, int64_t now);       // schedule.py next_top_of_hour
int32_t next_refresh_seconds(const Rule& r, int64_t now);   // + 60 s, clamped 300..21600
```

- [ ] **Step 1: Write the generator**

Create `tools/gen_tz_table.py`:

```python
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""IANA zone -> POSIX TZ rule for include/tz_table.h (spec §2.2), from the footer of each
TZif file in the system tzdata, plus test/test_data/tz_cases.h with expected local times
from Python's zoneinfo. Run: uv run tools/gen_tz_table.py
"""
import zoneinfo
from datetime import datetime, timedelta, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TABLE = ROOT / "include" / "tz_table.h"
CASES = ROOT / "test" / "test_data" / "tz_cases.h"
CASE_ZONES = ["UTC", "America/Los_Angeles", "America/New_York", "Europe/London", "Asia/Kolkata",
              "Asia/Kathmandu", "Australia/Lord_Howe", "Australia/Sydney", "Pacific/Chatham",
              "America/Nuuk", "America/Sao_Paulo", "Africa/Casablanca"]
EPOCH = datetime(1970, 1, 1, tzinfo=timezone.utc)


def footer(name: str) -> str | None:
    for base in zoneinfo.TZPATH:
        p = Path(base) / name
        if p.is_file():
            data = p.read_bytes()
            if not data.startswith(b"TZif") or data[4:5] not in (b"2", b"3", b"4"):
                return None
            rule = data.rstrip(b"\n").rsplit(b"\n", 1)[-1].decode("ascii")
            return rule or None
    return None


def next_top_of_hour(now: datetime, tz) -> datetime:  # server/inkboard_server/schedule.py
    t = now.astimezone(timezone.utc).replace(second=0, microsecond=0)
    t = t.replace(minute=t.minute - t.minute % 15)
    while True:
        t += timedelta(minutes=15)
        local = t.astimezone(tz)
        if local.minute == 0 and t > now:
            return local


def main() -> None:
    entries = []
    for name in sorted(zoneinfo.available_timezones()):
        rule = footer(name)
        if rule and not name.startswith(("posix/", "right/")) and name not in ("Factory", "localtime"):
            entries.append((name, rule))
    body = "\n".join(f'    {{"{n}", "{r}"}},' for n, r in entries)
    TABLE.write_text(
        "#pragma once\n// Generated by tools/gen_tz_table.py from the system tzdata; do not edit.\n"
        "// Regenerate (and reflash) when a zone's rules change.\n\nnamespace ink::tz {\n\n"
        "struct Entry {\n  const char* name;\n  const char* rule;\n};\n\n"
        f"inline constexpr Entry TABLE[] = {{\n{body}\n}};\n\n}}  // namespace ink::tz\n")
    rows = []
    for zname in CASE_ZONES:
        tz = zoneinfo.ZoneInfo(zname)
        start = datetime(2026, 1, 1, tzinfo=timezone.utc)
        instants = set()
        prev = start.astimezone(tz).utcoffset()
        t = start
        while t < datetime(2029, 1, 1, tzinfo=timezone.utc):
            off = t.astimezone(tz).utcoffset()
            if off != prev:
                for k in range(-12, 13):
                    instants.add(t + timedelta(minutes=15 * k) - timedelta(minutes=30))
                prev = off
            if t.hour == 0 and t.weekday() == 2:
                instants.add(t + timedelta(minutes=7))
            t += timedelta(hours=1)
        for t in sorted(instants):
            local = t.astimezone(tz)
            epoch = int((t - EPOCH).total_seconds())
            day = (local.date() - EPOCH.date()).days
            nxt = int((next_top_of_hour(t, tz) - EPOCH).total_seconds())
            rows.append(f'    {{"{zname}", {epoch}, {day}, {local.hour}, {local.minute}, '
                        f'{int(local.utcoffset().total_seconds())}, {nxt}}},')
    CASES.write_text(
        "#pragma once\n// Generated by tools/gen_tz_table.py from Python's zoneinfo; do not edit.\n"
        "#include <stdint.h>\n"
        "struct TzCase { const char* zone; int64_t epoch; int32_t day; int hh, mm; int32_t offset; int64_t next_top; };\n"
        "static const TzCase TZ_CASES[] = {\n" + "\n".join(rows) + "\n};\n")
    print(f"{len(entries)} zones, {len(rows)} cases")


if __name__ == "__main__":
    main()
```

Run: `uv run tools/gen_tz_table.py`
Expected: something like `5xx zones, 3xxx cases`; `grep -c '"America/Los_Angeles", "PST8PDT,M3.2.0,M11.1.0"' include/tz_table.h` prints `1`.

- [ ] **Step 2: Write the failing tests**

Add to `test/test_data/test_main.cpp`:

```cpp
#include "tz.h"
#include "tz_cases.h"

void test_tz_lookup() {
  TEST_ASSERT_EQUAL_STRING("PST8PDT,M3.2.0,M11.1.0", ink::tz::lookup("America/Los_Angeles"));
  TEST_ASSERT_NOT_NULL(ink::tz::lookup("UTC"));
  TEST_ASSERT_NULL(ink::tz::lookup("America"));
  TEST_ASSERT_NULL(ink::tz::lookup("../../etc/passwd"));
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
```

Note: if Africa/Casablanca's footer has no DST rule the generator still emits its cases; if its zoneinfo cases diverge because tzdata lists explicit future transitions not expressible in the footer (Casablanca's Ramadan switches), delete it from `CASE_ZONES` and note it in the generator's docstring as a known limitation of footer rules.

Run: `pio test -e native -f test_data` → Expected: compile error (`tz.h` missing).

- [ ] **Step 3: Implement `include/tz.h`**

```cpp
#pragma once
// IANA zone -> POSIX TZ rule (generated table) and local time without a tz database
// (spec §2.2). Our own rule evaluator, so the host tests and the board run the same code.
// Pure; host-tested against Python's zoneinfo (test/test_data/tz_cases.h).
#include <stdint.h>
#include <string.h>

#include "civil.h"
#include "tz_table.h"

namespace ink::tz {

inline const char* lookup(const char* name) {
  size_t lo = 0, hi = sizeof(TABLE) / sizeof(TABLE[0]);
  while (lo < hi) {
    const size_t mid = (lo + hi) / 2;
    const int c = strcmp(TABLE[mid].name, name);
    if (c == 0) return TABLE[mid].rule;
    if (c < 0) lo = mid + 1;
    else hi = mid;
  }
  return nullptr;
}

struct Transition {
  char kind;     // 'M' month.week.day, 'J' Julian 1..365 without Feb 29, 'N' 0..365 with it
  int m, w, d;   // 'M': month 1-12, week 1-5 (5 = last), weekday 0 = Sunday
  int day_n;     // 'J' / 'N'
  int32_t time_s;  // local time of day of the switch, may be negative or > 24 h
};

struct Rule {
  int32_t std_offset;  // seconds east of UTC (POSIX writes the opposite sign)
  int32_t dst_offset;
  bool has_dst;
  Transition start, end;
};

namespace detail {

inline bool name(const char*& s) {
  if (*s == '<') {
    const char* e = strchr(s, '>');
    if (e == nullptr) return false;
    s = e + 1;
    return true;
  }
  const char* b = s;
  while ((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z')) ++s;
  return s - b >= 3;
}

inline bool number(const char*& s, int& out) {
  if (*s < '0' || *s > '9') return false;
  out = 0;
  while (*s >= '0' && *s <= '9') out = out * 10 + (*s++ - '0');
  return true;
}

// [+-]hh[:mm[:ss]]
inline bool hms(const char*& s, int32_t& out) {
  int sign = 1;
  if (*s == '+' || *s == '-') sign = *s++ == '-' ? -1 : 1;
  int v[3] = {0, 0, 0};
  if (!number(s, v[0])) return false;
  for (int i = 1; i < 3 && *s == ':'; ++i) {
    ++s;
    if (!number(s, v[i])) return false;
  }
  out = sign * (v[0] * 3600 + v[1] * 60 + v[2]);
  return true;
}

inline bool transition(const char*& s, Transition& t) {
  t = Transition{'M', 0, 0, 0, 0, 7200};
  if (*s == 'M') {
    ++s;
    if (!number(s, t.m) || *s != '.') return false;
    ++s;
    if (!number(s, t.w) || *s != '.') return false;
    ++s;
    if (!number(s, t.d)) return false;
    if (t.m < 1 || t.m > 12 || t.w < 1 || t.w > 5 || t.d > 6) return false;
  } else if (*s == 'J') {
    ++s;
    t.kind = 'J';
    if (!number(s, t.day_n) || t.day_n < 1 || t.day_n > 365) return false;
  } else {
    t.kind = 'N';
    if (!number(s, t.day_n) || t.day_n > 365) return false;
  }
  if (*s == '/') {
    ++s;
    if (!hms(s, t.time_s)) return false;
  }
  return true;
}

inline int32_t transition_day(const Transition& t, int y) {
  const int32_t jan1 = days_from_civil(y, 1, 1);
  if (t.kind == 'J') return jan1 + t.day_n - 1 + (is_leap(y) && t.day_n >= 60 ? 1 : 0);
  if (t.kind == 'N') return jan1 + t.day_n;
  const int32_t first = days_from_civil(y, t.m, 1);
  const int sun0 = (weekday(first) + 1) % 7;  // 0 = Sunday
  int32_t d = first + (t.d - sun0 + 7) % 7 + (t.w - 1) * 7;
  while (civil_from_days(d).m != t.m) d -= 7;  // week 5 means "last"
  return d;
}

}  // namespace detail

inline bool parse(const char* s, Rule& r) {
  using namespace detail;
  if (s == nullptr) return false;
  int32_t off;
  if (!name(s) || !hms(s, off)) return false;
  r = Rule{};
  r.std_offset = -off;
  r.dst_offset = r.std_offset;
  if (*s == '\0') return true;
  if (!name(s)) return false;
  r.has_dst = true;
  r.dst_offset = r.std_offset + 3600;
  if (*s != ',') {
    if (!hms(s, off)) return false;
    r.dst_offset = -off;
  }
  // tzdata footers with DST always spell out both transitions.
  if (*s != ',') return false;
  ++s;
  if (!transition(s, r.start) || *s != ',') return false;
  ++s;
  if (!transition(s, r.end)) return false;
  return *s == '\0';
}

inline bool is_dst(const Rule& r, int64_t t) {
  if (!r.has_dst) return false;
  const int y = civil_from_days(static_cast<int32_t>(floor_div(t + r.std_offset, 86400))).y;
  // The start time is local standard time, the end time local daylight time.
  const int64_t start = int64_t(detail::transition_day(r.start, y)) * 86400 + r.start.time_s - r.std_offset;
  const int64_t end = int64_t(detail::transition_day(r.end, y)) * 86400 + r.end.time_s - r.dst_offset;
  return start < end ? (t >= start && t < end) : !(t >= end && t < start);
}

inline int32_t utc_offset(const Rule& r, int64_t t) { return is_dst(r, t) ? r.dst_offset : r.std_offset; }

struct Local {
  int32_t day;
  int hh, mm, ss;
  int32_t offset;
};

inline Local to_local(const Rule& r, int64_t t) {
  const int32_t off = utc_offset(r, t);
  const int64_t l = t + off;
  const int32_t day = static_cast<int32_t>(floor_div(l, 86400));
  const int32_t sod = static_cast<int32_t>(l - int64_t(day) * 86400);
  return Local{day, sod / 3600, sod / 60 % 60, sod % 60, off};
}

// schedule.py: step UTC in 15-minute increments (every real offset is a multiple of 15 min)
// until local time reads hh:00, so DST and half-hour zones need no special cases.
inline int64_t next_top_of_hour(const Rule& r, int64_t now) {
  int64_t t = now - floor_mod(now, 900);
  for (;;) {
    t += 900;
    const Local l = to_local(r, t);
    if (l.mm == 0 && t > now) return t;
  }
}

inline int32_t next_refresh_seconds(const Rule& r, int64_t now) {
  const int64_t s = next_top_of_hour(r, now) + 60 - now;
  return static_cast<int32_t>(s < 300 ? 300 : s > 21600 ? 21600 : s);
}

}  // namespace ink::tz
```

- [ ] **Step 4: Run**

Run: `pio test -e native -f test_data`
Expected: the four tz tests PASS. If `test_tz_matches_zoneinfo` prints mismatches only around a transition instant, re-check which offset the start/end time is measured in for that zone's rule (POSIX: start in standard time, end in daylight time).

- [ ] **Step 5: Commit**

```bash
git add tools/gen_tz_table.py include/tz.h include/tz_table.h test/test_data/tz_cases.h test/test_data/test_main.cpp
git commit -m "Time: IANA zones via generated POSIX rules, checked against zoneinfo

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

### Task 7: Series catalog and FRAME_QUERY parser

Port of `server/inkboard_server/series.py`, `query.py` and `widgets/params.py` with the same
error messages, so the README's layout section and the config error screen stay valid. Known
deviation (documented in the code): Python's `float()`/`int()` accept `_` digit separators
(`1_0`); the C++ does not.

**Files:**
- Create: `include/series.h`, `include/query.h`
- Test: `test/test_data/test_main.cpp`

**Interfaces:**
- Consumes: `fmt_thousands0` (Task 3); `ink::tz::lookup`, `ink::tz::Rule`, `ink::tz::parse` (Task 6).
- Produces (`namespace ink`):

```cpp
// series.h
enum class ValueFmt : uint8_t { Thousands0, KiloDollars1, Percent2, MegaDollars2, Fixed1 };
struct SeriesDef { const char* id; const char* fred_id; const char* label; const char* short_label;
                   ValueFmt fmt; const char* attribution[3]; };   // attribution nullptr-terminated
inline constexpr SeriesDef CATALOG[6];   // order: sp500, btc, mortgage30, home_la, ust10y, usd_broad
constexpr int N_SERIES = 6;
inline constexpr uint8_t DEFAULT_SERIES[4] = {0, 1, 2, 3};
int find_series(const char* id, size_t len);                 // CATALOG index or -1
void format_value(ValueFmt f, double v, char* out, size_t n); // SeriesDef.fmt in series.py
// query.h
enum class WidgetType : uint8_t { MarketTrends, CalendarWeather };
enum class Size : uint8_t { Third = 1, TwoThirds = 2, Full = 3 };
int size_width(Size s);  const char* size_token(Size s);  const char* widget_name(WidgetType t);
struct Column { WidgetType type; Size size; };
struct Layout {
  uint8_t n_columns; Column columns[3];
  char tz[64]; tz::Rule zone;
  bool has_market, has_weather;
  uint8_t n_series; uint8_t series[4]; int years;     // market_trends (shared by all its columns)
  double lat, lon; bool metric;                       // calendar_weather
};
constexpr size_t MAX_QUERY_LEN = 1024;
bool parse_query(const char* raw, Layout& out, char* error, size_t error_n);  // false + one-line message
```

- [ ] **Step 1: Write the failing tests**

Add to `test/test_data/test_main.cpp`:

```cpp
#include "query.h"

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
```

`RUN_TEST` all five. Run `pio test -e native -f test_data` → Expected: compile error (`query.h` missing).

- [ ] **Step 2: Implement `include/series.h`**

```cpp
#pragma once
// Market series a board can show (spec §1.1). Port of server/inkboard_server/series.py.
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "format.h"

namespace ink {

enum class ValueFmt : uint8_t { Thousands0, KiloDollars1, Percent2, MegaDollars2, Fixed1 };

struct SeriesDef {
  const char* id;
  const char* fred_id;
  const char* label;
  const char* short_label;
  ValueFmt fmt;
  const char* attribution[3];
};

inline constexpr SeriesDef CATALOG[] = {
    {"sp500", "SP500", "S&P 500", "S&P", ValueFmt::Thousands0, {"FRED", "S&P DJI", nullptr}},
    {"btc", "CBBTCUSD", "Bitcoin", "BTC", ValueFmt::KiloDollars1, {"FRED", "Coinbase", nullptr}},
    {"mortgage30", "MORTGAGE30US", "Mortgage", "Mort", ValueFmt::Percent2, {"FRED", "Freddie Mac", nullptr}},
    {"home_la", "MEDLISPRI31080", "LA home", "Home", ValueFmt::MegaDollars2, {"FRED", "Realtor.com", nullptr}},
    {"ust10y", "DGS10", "10-yr Treasury", "10y", ValueFmt::Percent2, {"FRED", nullptr, nullptr}},
    {"usd_broad", "DTWEXBGS", "Dollar index", "USD", ValueFmt::Fixed1, {"FRED", nullptr, nullptr}},
};
constexpr int N_SERIES = 6;
inline constexpr uint8_t DEFAULT_SERIES[] = {0, 1, 2, 3};

inline int find_series(const char* id, size_t len) {
  for (int i = 0; i < N_SERIES; ++i)
    if (strlen(CATALOG[i].id) == len && strncmp(CATALOG[i].id, id, len) == 0) return i;
  return -1;
}

inline void format_value(ValueFmt f, double v, char* out, size_t n) {
  switch (f) {
    case ValueFmt::Thousands0: fmt_thousands0(out, n, v); return;
    case ValueFmt::KiloDollars1: snprintf(out, n, "$%.1fk", v / 1000); return;
    case ValueFmt::Percent2: snprintf(out, n, "%.2f%%", v); return;
    case ValueFmt::MegaDollars2: snprintf(out, n, "$%.2fM", v / 1e6); return;
    case ValueFmt::Fixed1: snprintf(out, n, "%.1f", v); return;
  }
}

}  // namespace ink
```

- [ ] **Step 3: Implement `include/query.h`**

```cpp
#pragma once
// FRAME_QUERY -> Layout (spec §1.1). Port of server/inkboard_server/query.py and
// widgets/params.py with the same one-line messages (they reach the config error screen).
// Deviation: Python's float()/int() also accept "_" digit separators; these parsers don't.
#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "series.h"
#include "tz.h"

namespace ink {

enum class WidgetType : uint8_t { MarketTrends, CalendarWeather };
enum class Size : uint8_t { Third = 1, TwoThirds = 2, Full = 3 };

inline int size_width(Size s) { return s == Size::Third ? 266 : s == Size::TwoThirds ? 534 : 800; }
inline const char* size_token(Size s) { return s == Size::Third ? "1/3" : s == Size::TwoThirds ? "2/3" : "1"; }
inline const char* widget_name(WidgetType t) { return t == WidgetType::MarketTrends ? "market_trends" : "calendar_weather"; }

struct Column {
  WidgetType type;
  Size size;
};

struct Layout {
  uint8_t n_columns = 0;
  Column columns[3] = {};
  char tz[64] = "UTC";
  tz::Rule zone = {};
  bool has_market = false, has_weather = false;
  uint8_t n_series = 0;
  uint8_t series[4] = {};
  int years = 5;
  double lat = 0, lon = 0;
  bool metric = false;
};

constexpr size_t MAX_QUERY_LEN = 1024;

namespace query_detail {

constexpr int MAX_PAIRS = 16;
constexpr size_t MAX_TEXT = 256;

struct Pair {
  char key[32];
  char value[MAX_TEXT];
};

inline int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// urllib.parse.unquote_plus: '+' -> ' ', %XX decoded, malformed escapes kept verbatim.
inline void unquote(const char* s, size_t n, char* out, size_t cap) {
  size_t k = 0;
  for (size_t i = 0; i < n && k + 1 < cap; ++i) {
    if (s[i] == '+') out[k++] = ' ';
    else if (s[i] == '%' && i + 2 < n && hexval(s[i + 1]) >= 0 && hexval(s[i + 2]) >= 0) {
      out[k++] = static_cast<char>(hexval(s[i + 1]) * 16 + hexval(s[i + 2]));
      i += 2;
    } else out[k++] = s[i];
  }
  out[k] = '\0';
}

// Python repr() of a str, enough for these messages: single quotes unless the text has one.
inline void repr(const char* s, char* out, size_t n) {
  const char q = strchr(s, '\'') && !strchr(s, '"') ? '"' : '\'';
  snprintf(out, n, "%c%s%c", q, s, q);
}

inline void err(char* e, size_t n, const char* fmt, const char* a = "", const char* b = "") { snprintf(e, n, fmt, a, b); }

inline bool trim_span(const char* s, const char*& b, const char*& end) {
  b = s;
  end = s + strlen(s);
  while (b < end && isspace(static_cast<unsigned char>(*b))) ++b;
  while (end > b && isspace(static_cast<unsigned char>(end[-1]))) --end;
  return b < end;
}

// Python float(): whitespace allowed around; decimal, exponent, inf/nan; no hex.
inline bool py_float(const char* s, double& v) {
  const char *b, *e;
  if (!trim_span(s, b, e)) return false;
  char buf[64];
  const size_t n = static_cast<size_t>(e - b);
  if (n >= sizeof buf) return false;
  memcpy(buf, b, n);
  buf[n] = '\0';
  if (strpbrk(buf, "xX")) return false;
  char* endp = nullptr;
  v = strtod(buf, &endp);
  return endp == buf + n;
}

inline bool py_int(const char* s, long& v) {
  const char *b, *e;
  if (!trim_span(s, b, e)) return false;
  const char* p = b;
  if (*p == '+' || *p == '-') ++p;
  if (p == e) return false;
  for (const char* q = p; q < e; ++q)
    if (!isdigit(static_cast<unsigned char>(*q))) return false;
  v = strtol(b, nullptr, 10);
  return true;
}

inline bool coordinate(const char* name, const char* text, double lo, double hi, double& out, char* e, size_t en) {
  double v;
  char r[MAX_TEXT + 4];
  if (!py_float(text, v)) {
    repr(text, r, sizeof r);
    snprintf(e, en, "%s: could not convert string to float: %s", name, r);
    return false;
  }
  if (!isfinite(v) || v < lo || v > hi) {
    snprintf(e, en, "%s: must be a number from %g to %g", name, lo, hi);
    return false;
  }
  char buf[64];
  snprintf(buf, sizeof buf, "%.1f", v);  // 0.1 degree cells, like f"{v:.1f}"
  out = strtod(buf, nullptr) + 0.0;      // -0.0 -> 0.0
  return true;
}

}  // namespace query_detail

inline bool parse_query(const char* raw, Layout& out, char* error, size_t error_n) {
  using namespace query_detail;
  out = Layout{};
  const size_t len = strlen(raw);
  if (len > MAX_QUERY_LEN) return err(error, error_n, "query longer than 1024 bytes"), false;

  Pair pairs[MAX_PAIRS];
  int n_pairs = 0;
  if (len > 0) {  // parse_qsl(strict_parsing=bool(raw), keep_blank_values=True)
    const char* p = raw;
    for (;;) {
      const char* amp = strchr(p, '&');
      const size_t flen = amp ? static_cast<size_t>(amp - p) : strlen(p);
      const char* eq = static_cast<const char*>(memchr(p, '=', flen));
      if (eq == nullptr || n_pairs == MAX_PAIRS) return err(error, error_n, "malformed query string"), false;
      unquote(p, static_cast<size_t>(eq - p), pairs[n_pairs].key, sizeof pairs[n_pairs].key);
      unquote(eq + 1, flen - static_cast<size_t>(eq + 1 - p), pairs[n_pairs].value, sizeof pairs[n_pairs].value);
      for (int i = 0; i < n_pairs; ++i)
        if (strcmp(pairs[i].key, pairs[n_pairs].key) == 0)
          return err(error, error_n, "%s: given more than once", pairs[n_pairs].key), false;
      ++n_pairs;
      if (!amp) break;
      p = amp + 1;
    }
  }
  auto find = [&](const char* k) -> const char* {
    for (int i = 0; i < n_pairs; ++i)
      if (strcmp(pairs[i].key, k) == 0) return pairs[i].value;
    return nullptr;
  };

  const char* w = find("w");
  if (w == nullptr) return err(error, error_n, "w: required, e.g. w=market_trends:2/3,calendar_weather:1/3"), false;
  {  // parse_w
    int items = 1;
    for (const char* c = w; *c; ++c) items += *c == ',';
    if (items > 3) return err(error, error_n, "w: at most 3 widgets"), false;
    const char* p = w;
    int total = 0;
    for (int i = 0; i < items; ++i) {
      const char* comma = strchr(p, ',');
      char item[MAX_TEXT];
      const size_t n = comma ? static_cast<size_t>(comma - p) : strlen(p);
      snprintf(item, sizeof item, "%.*s", static_cast<int>(n), p);
      p = comma ? comma + 1 : p + n;
      char r[MAX_TEXT + 4];
      char* colon = strchr(item, ':');
      if (colon == nullptr) {
        repr(item, r, sizeof r);
        return err(error, error_n, "w: expected type:size, got %s", r), false;
      }
      *colon = '\0';
      const char* token = colon + 1;
      Column c;
      if (strcmp(item, "market_trends") == 0) c.type = WidgetType::MarketTrends;
      else if (strcmp(item, "calendar_weather") == 0) c.type = WidgetType::CalendarWeather;
      else {
        repr(item, r, sizeof r);
        return err(error, error_n, "w: unknown widget %s", r), false;
      }
      if (strcmp(token, "1/3") == 0) c.size = Size::Third;
      else if (strcmp(token, "2/3") == 0) c.size = Size::TwoThirds;
      else if (strcmp(token, "1") == 0) c.size = Size::Full;
      else {
        repr(token, r, sizeof r);
        return err(error, error_n, "w: size must be 1/3, 2/3 or 1, got %s", r), false;
      }
      out.columns[out.n_columns++] = c;
      total += static_cast<int>(c.size);
      (c.type == WidgetType::MarketTrends ? out.has_market : out.has_weather) = true;
    }
    if (total != 3) {
      char t[8];
      snprintf(t, sizeof t, "%d", total);
      return err(error, error_n, "w: sizes add up to %s/3, need 3/3", t), false;
    }
  }

  const char* tzv = find("tz");
  if (tzv == nullptr) tzv = "UTC";
  const char* rule = tz::lookup(tzv);
  if (rule == nullptr || strlen(tzv) >= sizeof out.tz || !tz::parse(rule, out.zone)) {
    char r[MAX_TEXT + 4];
    repr(tzv, r, sizeof r);
    return err(error, error_n, "tz: unknown timezone %s", r), false;
  }
  snprintf(out.tz, sizeof out.tz, "%s", tzv);

  // Parameter ownership in registry order (market_trends, then calendar_weather), like query.py.
  struct Param { const char* name; WidgetType owner; };
  static const Param kParams[] = {{"series", WidgetType::MarketTrends}, {"years", WidgetType::MarketTrends},
                                  {"lat", WidgetType::CalendarWeather}, {"lon", WidgetType::CalendarWeather},
                                  {"units", WidgetType::CalendarWeather}};
  auto used = [&](WidgetType t) { return t == WidgetType::MarketTrends ? out.has_market : out.has_weather; };
  for (int i = 0; i < n_pairs; ++i) {
    const char* k = pairs[i].key;
    if (strcmp(k, "w") == 0 || strcmp(k, "tz") == 0) continue;
    const Param* p = nullptr;
    for (const Param& q : kParams)
      if (strcmp(q.name, k) == 0) p = &q;
    if (p == nullptr) return err(error, error_n, "%s: unknown parameter", k), false;
    if (!used(p->owner)) return err(error, error_n, "%s: not used by any widget in w", k), false;
  }

  // Options in sorted name order: lat, lon, series, units, years.
  if (out.has_weather) {
    const char* lat = find("lat");
    const char* lon = find("lon");
    if (lat == nullptr) return err(error, error_n, "lat: required by calendar_weather"), false;
    if (!coordinate("lat", lat, -90, 90, out.lat, error, error_n)) return false;
    if (lon == nullptr) return err(error, error_n, "lon: required by calendar_weather"), false;
    if (!coordinate("lon", lon, -180, 180, out.lon, error, error_n)) return false;
  }
  if (out.has_market) {
    const char* s = find("series");
    if (s == nullptr) {
      out.n_series = 4;
      memcpy(out.series, DEFAULT_SERIES, 4);
    } else {
      int ids = 1;
      for (const char* c = s; *c; ++c) ids += *c == ',';
      if (ids > 4) return err(error, error_n, "series: give 1 to 4 ids"), false;
      // params.py order: count, uniqueness (on the raw strings), then the first unknown id.
      p = s;
      char raw_ids[4][MAX_TEXT];
      for (int i = 0; i < ids; ++i) {
        const char* comma = strchr(p, ',');
        const size_t n = comma ? static_cast<size_t>(comma - p) : strlen(p);
        snprintf(raw_ids[i], MAX_TEXT, "%.*s", static_cast<int>(n), p);
        p = comma ? comma + 1 : p + n;
        for (int j = 0; j < i; ++j)
          if (strcmp(raw_ids[i], raw_ids[j]) == 0) return err(error, error_n, "series: ids must be unique"), false;
      }
      for (int i = 0; i < ids; ++i) {
        const int k = find_series(raw_ids[i], strlen(raw_ids[i]));
        if (k < 0) {
          char r[MAX_TEXT + 4];
          repr(raw_ids[i], r, sizeof r);
          return err(error, error_n,
                     "series: unknown id %s; choose from sp500, btc, mortgage30, home_la, ust10y, usd_broad", r), false;
        }
        out.series[i] = static_cast<uint8_t>(k);
      }
      out.n_series = static_cast<uint8_t>(ids);
    }
  }
  if (out.has_weather) {
    const char* u = find("units");
    if (u == nullptr || strcmp(u, "imperial") == 0) out.metric = false;
    else if (strcmp(u, "metric") == 0) out.metric = true;
    else return err(error, error_n, "units: must be one of imperial, metric"), false;
  }
  if (out.has_market) {
    const char* y = find("years");
    long v = 5;
    if (y != nullptr && (!py_int(y, v) || v < 1 || v > 10))
      return err(error, error_n, "years: must be a whole number from 1 to 10"), false;
    out.years = static_cast<int>(v);
  }
  return true;
}

}  // namespace ink
```

- [ ] **Step 4: Run**

Run: `pio test -e native -f test_data`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add include/series.h include/query.h test/test_data/test_main.cpp
git commit -m "Query: FRAME_QUERY parser and series catalog, ported with the server's messages

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

### Task 8: Streaming JSON tokenizer

**Files:**
- Create: `include/json_stream.h`
- Test: `test/test_data/test_main.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces (`namespace ink::json`):

```cpp
constexpr int MAX_DEPTH = 8;  constexpr size_t MAX_KEY = 31, MAX_STRING = 127, MAX_NUMBER = 63;
enum class Type : uint8_t { String, Number, True, False, Null };
struct Handler {
  virtual void scalar(const Parser& p, Type t, const char* text) = 0;  // text NUL-terminated
  virtual void end_container(const Parser& p) {}                       // p.depth() = depth after closing
};
class Parser {
 public:
  explicit Parser(Handler& h);
  bool feed(const char* data, size_t n);   // false after any error
  bool finish();                            // true iff exactly one complete value and no error
  bool failed() const;
  int depth() const;
  bool is_array(int level) const; const char* key(int level) const; int32_t index(int level) const;
  bool truncated() const;                   // the last string value was cut at MAX_STRING
  bool at(std::initializer_list<const char*> path) const;  // nullptr matches any array index
};
```

- [ ] **Step 1: Write the failing tests**

Add to `test/test_data/test_main.cpp`:

```cpp
#include "json_stream.h"

struct Recorder : ink::json::Handler {
  std::string log;
  void scalar(const ink::json::Parser& p, ink::json::Type t, const char* text) override {
    for (int i = 0; i < p.depth(); ++i) {
      if (p.is_array(i)) log += "[" + std::to_string(p.index(i)) + "]";
      else log += std::string(".") + p.key(i);
    }
    log += "=" + std::to_string(static_cast<int>(t)) + ":" + text + ";";
  }
  void end_container(const ink::json::Parser& p) override { log += "end@" + std::to_string(p.depth()) + ";"; }
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
  const std::string whole = parse_all(doc, doc.size());
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
  TEST_ASSERT_NOT_EQUAL(std::string::npos, r.log.find(std::string(127, 'z') + ";"));
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
```

Also add `#include <algorithm>` at the top. Run `pio test -e native -f test_data` → Expected: compile error (`json_stream.h` missing).

- [ ] **Step 2: Implement `include/json_stream.h`**

```cpp
#pragma once
// Streaming (SAX-style) JSON tokenizer with bounded state (spec §2.6): fed in any chunks,
// it reports each scalar with the path that leads to it, so a 370 KB FRED response never
// has to fit in RAM. Pure; host-tested with every chunk split.
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <initializer_list>

namespace ink::json {

constexpr int MAX_DEPTH = 8;
constexpr size_t MAX_KEY = 31;      // longer keys are an error: no API we read has one
constexpr size_t MAX_STRING = 127;  // longer values are truncated (FRED error messages)
constexpr size_t MAX_NUMBER = 63;

enum class Type : uint8_t { String, Number, True, False, Null };

class Parser;

struct Handler {
  virtual ~Handler() = default;
  virtual void scalar(const Parser& p, Type t, const char* text) = 0;
  // A container closed; p.depth() is the depth after closing, and its key/index is still readable.
  virtual void end_container(const Parser& p) { (void)p; }
};

class Parser {
 public:
  explicit Parser(Handler& h) : h_(h) {}

  bool feed(const char* data, size_t n) {
    for (size_t i = 0; i < n && state_ != St::Error; ++i) step(data[i]);
    return state_ != St::Error;
  }

  bool finish() {
    if (state_ == St::Number && depth_ == 0) end_number();  // a bare top-level number ends at EOF
    return state_ == St::Done;
  }

  bool failed() const { return state_ == St::Error; }
  int depth() const { return depth_; }
  bool is_array(int level) const { return frames_[level].array; }
  const char* key(int level) const { return frames_[level].key; }
  int32_t index(int level) const { return frames_[level].index; }
  bool truncated() const { return truncated_; }

  bool at(std::initializer_list<const char*> path) const {
    if (static_cast<int>(path.size()) != depth_) return false;
    int i = 0;
    for (const char* k : path) {
      const Frame& f = frames_[i++];
      if (k == nullptr) {
        if (!f.array) return false;
      } else if (f.array || strcmp(f.key, k) != 0) {
        return false;
      }
    }
    return true;
  }

 private:
  enum class St : uint8_t { Value, FirstValueOrEnd, FirstKeyOrEnd, Key, Colon, Comma, String, Escape, Unicode,
                            Number, Literal, Done, Error };
  struct Frame {
    bool array;
    int32_t index;
    char key[MAX_KEY + 1];
  };

  static bool ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
  static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  }
  // -?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?
  static bool valid_number(const char* s) {
    if (*s == '-') ++s;
    if (*s == '0') ++s;
    else if (*s >= '1' && *s <= '9') while (*s >= '0' && *s <= '9') ++s;
    else return false;
    if (*s == '.') {
      ++s;
      if (*s < '0' || *s > '9') return false;
      while (*s >= '0' && *s <= '9') ++s;
    }
    if (*s == 'e' || *s == 'E') {
      ++s;
      if (*s == '+' || *s == '-') ++s;
      if (*s < '0' || *s > '9') return false;
      while (*s >= '0' && *s <= '9') ++s;
    }
    return *s == '\0';
  }

  void fail() { state_ = St::Error; }
  void after_value() { state_ = depth_ == 0 ? St::Done : St::Comma; }

  void start_string(bool is_key) {
    string_is_key_ = is_key;
    len_ = 0;
    truncated_ = false;
    hi_ = 0;
    state_ = St::String;
  }

  void value_start(char c) {
    if (ws(c)) return;
    switch (c) {
      case '{': return open(false);
      case '[': return open(true);
      case '"': return start_string(false);
      case 't': return literal("true", Type::True);
      case 'f': return literal("false", Type::False);
      case 'n': return literal("null", Type::Null);
      default:
        if (c == '-' || (c >= '0' && c <= '9')) {
          len_ = 0;
          buf_[len_++] = c;
          state_ = St::Number;
          return;
        }
        return fail();
    }
  }

  void literal(const char* word, Type t) {
    literal_ = word;
    lit_pos_ = 1;
    lit_type_ = t;
    state_ = St::Literal;
  }

  void open(bool array) {
    if (depth_ == MAX_DEPTH) return fail();
    Frame& f = frames_[depth_++];
    f.array = array;
    f.index = 0;
    f.key[0] = '\0';
    state_ = array ? St::FirstValueOrEnd : St::FirstKeyOrEnd;
  }

  void close(char c) {
    if (frames_[depth_ - 1].array != (c == ']')) return fail();
    --depth_;
    h_.end_container(*this);
    after_value();
  }

  void push(char c) {
    const size_t cap = string_is_key_ ? MAX_KEY : MAX_STRING;
    if (len_ < cap) buf_[len_++] = c;
    else if (string_is_key_) fail();
    else truncated_ = true;
  }

  void put_utf8(uint32_t cp) {
    if (cp < 0x80) return push(static_cast<char>(cp));
    if (cp < 0x800) {
      push(static_cast<char>(0xC0 | (cp >> 6)));
    } else if (cp < 0x10000) {
      push(static_cast<char>(0xE0 | (cp >> 12)));
      push(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    } else {
      push(static_cast<char>(0xF0 | (cp >> 18)));
      push(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      push(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    }
    push(static_cast<char>(0x80 | (cp & 0x3F)));
  }

  void string_char(char c) {
    if (hi_ != 0 && c != '\\') return fail();  // a high surrogate must be followed by \u low half
    if (c == '"') return end_string();
    if (c == '\\') {
      state_ = St::Escape;
      return;
    }
    if (static_cast<unsigned char>(c) < 0x20) return fail();
    push(c);
  }

  void escape_char(char c) {
    state_ = St::String;
    if (hi_ != 0 && c != 'u') return fail();
    switch (c) {
      case '"': case '\\': case '/': return push(c);
      case 'b': return push('\b');
      case 'f': return push('\f');
      case 'n': return push('\n');
      case 'r': return push('\r');
      case 't': return push('\t');
      case 'u':
        uni_ = 0;
        uni_n_ = 0;
        state_ = St::Unicode;
        return;
      default: return fail();
    }
  }

  void unicode_char(char c) {
    const int v = hexval(c);
    if (v < 0) return fail();
    uni_ = (uni_ << 4) | static_cast<uint32_t>(v);
    if (++uni_n_ < 4) return;
    state_ = St::String;
    if (uni_ >= 0xD800 && uni_ < 0xDC00) {
      if (hi_ != 0) return fail();
      hi_ = uni_;
      return;
    }
    uint32_t cp = uni_;
    if (uni_ >= 0xDC00 && uni_ < 0xE000) {
      if (hi_ == 0) return fail();
      cp = 0x10000 + ((hi_ - 0xD800) << 10) + (uni_ - 0xDC00);
    } else if (hi_ != 0) {
      return fail();
    }
    hi_ = 0;
    put_utf8(cp);
  }

  void end_string() {
    buf_[len_] = '\0';
    if (string_is_key_) {
      memcpy(frames_[depth_ - 1].key, buf_, len_ + 1);
      state_ = St::Colon;
      return;
    }
    h_.scalar(*this, Type::String, buf_);
    after_value();
  }

  void end_number() {
    buf_[len_] = '\0';
    if (!valid_number(buf_)) return fail();
    h_.scalar(*this, Type::Number, buf_);
    after_value();
  }

  void step(char c) {
    switch (state_) {
      case St::Value: return value_start(c);
      case St::FirstValueOrEnd:
        if (ws(c)) return;
        if (c == ']') return close(c);
        state_ = St::Value;
        return value_start(c);
      case St::FirstKeyOrEnd:
        if (ws(c)) return;
        if (c == '}') return close(c);
        if (c != '"') return fail();
        return start_string(true);
      case St::Key:
        if (ws(c)) return;
        if (c != '"') return fail();
        return start_string(true);
      case St::Colon:
        if (ws(c)) return;
        if (c != ':') return fail();
        state_ = St::Value;
        return;
      case St::Comma:
        if (ws(c)) return;
        if (c == ',') {
          Frame& f = frames_[depth_ - 1];
          if (f.array) {
            ++f.index;
            state_ = St::Value;
          } else {
            state_ = St::Key;
          }
          return;
        }
        if (c == ']' || c == '}') return close(c);
        return fail();
      case St::String: return string_char(c);
      case St::Escape: return escape_char(c);
      case St::Unicode: return unicode_char(c);
      case St::Number:
        if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E') {
          if (len_ >= MAX_NUMBER) return fail();
          buf_[len_++] = c;
          return;
        }
        end_number();
        if (state_ != St::Error) step(c);  // the terminator belongs to the next state
        return;
      case St::Literal:
        if (c != literal_[lit_pos_]) return fail();
        if (literal_[++lit_pos_] == '\0') {
          h_.scalar(*this, lit_type_, literal_);
          after_value();
        }
        return;
      case St::Done:
        if (!ws(c)) fail();
        return;
      case St::Error: return;
    }
  }

  Handler& h_;
  St state_ = St::Value;
  Frame frames_[MAX_DEPTH] = {};
  int depth_ = 0;
  bool string_is_key_ = false;
  bool truncated_ = false;
  char buf_[MAX_STRING + 1] = {};
  size_t len_ = 0;
  uint32_t uni_ = 0, hi_ = 0;
  int uni_n_ = 0;
  const char* literal_ = nullptr;
  size_t lit_pos_ = 0;
  Type lit_type_ = Type::Null;
};

}  // namespace ink::json
```

Note the depth test in `test_json_rejects_malformed`: `[[[[[[[[[1]]]]]]]]]` opens 9 containers > `MAX_DEPTH` 8 → error.

- [ ] **Step 3: Run**

Run: `pio test -e native -f test_data`
Expected: the five JSON tests PASS.

- [ ] **Step 4: Commit**

```bash
git add include/json_stream.h test/test_data/test_main.cpp
git commit -m "Data: streaming JSON tokenizer with bounded state

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

### Task 9: Recorded fixtures and the Open-Meteo parser

The server's FRED fixtures were rebuilt from CSV (no API metadata, no `realtime_*` fields), so
new raw responses are recorded. Needs network and the user's FRED key.

**Files:**
- Create: `tools/record_fixtures.py`, `include/sources/openmeteo.h`
- Create (recorded, committed): `test/fixtures/fred_<ID>_full.json`, `test/fixtures/fred_<ID>_tail.json` (6 series each), `test/fixtures/fred_error_bad_key.json`, `test/fixtures/weather_la.json`, `test/fixtures/fixtures.h`, `test/fixtures/summaries.h`
- Test: `test/test_data/test_main.cpp`

**Interfaces:**
- Consumes: `json::Parser`, `json::Handler` (Task 8); `parse_iso_date` (Task 3).
- Produces:

```cpp
// test/fixtures/fixtures.h (generated)
#define FIXTURE_TODAY   <day number of the recording date in America/Los_Angeles>
#define FIXTURE_NOW     <epoch of that date at 10:00 America/Los_Angeles>
#define FIXTURE_TAIL_S0 <day number of the Sunday the tail fixtures start after>
#define FIXTURE_FULL_START <day number used as observation_start of the full fixtures>
// include/sources/openmeteo.h (namespace ink)
constexpr int FORECAST_DAYS = 8;
constexpr const char* OPENMETEO_HOST = "api.open-meteo.com";
struct DayForecast { int32_t day; int16_t code; double hi, lo; };
struct Weather { double temp; int16_t code; uint8_t n_daily; DayForecast daily[FORECAST_DAYS]; };
void openmeteo_path(char* out, size_t n, double lat, double lon, bool metric, const char* tz);
class OpenMeteoParser : public json::Handler { public: bool result(Weather& out) const; };
```

- [ ] **Step 1: Write the recorder**

Create `tools/record_fixtures.py`:

```python
# /// script
# requires-python = ">=3.11"
# dependencies = ["httpx"]
# ///
"""Record raw API responses for the native tests (plan Task 9, spec §3.4).

FRED key: $FRED_API_KEY, else read from include/secrets.h. The key is never printed, and
HTTP errors are reported without their URL (it contains the key).
Also writes test/fixtures/summaries.h: expected market summaries computed with a copy of the
server's market_data.summarize(), so the C++ pipeline is checked against the Python one.
Run: uv run tools/record_fixtures.py
"""
import os
import re
from datetime import date, datetime, time, timedelta
from pathlib import Path
from zoneinfo import ZoneInfo

import httpx

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "test" / "fixtures"
FRED_URL = "https://api.stlouisfed.org/fred/series/observations"
FRED_IDS = ["SP500", "CBBTCUSD", "MORTGAGE30US", "MEDLISPRI31080", "DGS10", "DTWEXBGS"]
LA = ZoneInfo("America/Los_Angeles")
EPOCH = date(1970, 1, 1)


def fred_key() -> str:
    if key := os.environ.get("FRED_API_KEY", "").strip():
        return key
    m = re.search(r'#define\s+FRED_API_KEY\s+"([^"]+)"', (ROOT / "include" / "secrets.h").read_text())
    if not m or m.group(1) == "your-fred-api-key":
        raise SystemExit("set FRED_API_KEY or put it in include/secrets.h")
    return m.group(1)


def get(client: httpx.Client, url: str, params: dict, expect: int = 200) -> bytes:
    r = client.get(url, params=params)
    if r.status_code != expect:
        raise SystemExit(f"{params.get('series_id', url)}: HTTP {r.status_code}")  # no URL: it has the key
    return r.content


def window_start(today: date, years: int) -> date:
    return today - timedelta(days=round(365.25 * years))


# --- copy of server/inkboard_server/market_data.py (summarize and helpers) ---
def weekly_grid(today: date, years: int) -> list[date]:
    start = window_start(today, years)
    d = start + timedelta(days=(6 - start.weekday()) % 7)
    grid = []
    while d <= today:
        grid.append(d)
        d += timedelta(days=7)
    if not grid or grid[-1] != today:
        grid.append(today)
    return grid


def summarize(obs: list[tuple[date, float]], today: date, years: int) -> dict:
    obs = sorted((d, v) for d, v in obs if d <= today and v > 0)
    grid = weekly_grid(today, years)
    pts, i, last = [], 0, None
    for g in grid:
        while i < len(obs) and obs[i][0] <= g:
            last = obs[i][1]
            i += 1
        if last is not None:
            pts.append((g, last))
    mean = sum(v for _, v in pts) / len(pts)
    lastv = pts[-1][1]
    prior = [v for d, v in pts if d <= today - timedelta(weeks=52)]
    return {"n": len(pts), "last": lastv, "ratio": lastv / mean, "yoy": (lastv / prior[-1] - 1) if prior else 0.0,
            "has_yoy": bool(prior), "last_date": (obs[-1][0] - EPOCH).days, "window_start": (pts[0][0] - EPOCH).days,
            "short": pts[0][0] > grid[0] + timedelta(days=31), "first_norm": pts[0][1] / mean,
            "last_norm": lastv / mean}
# --- end of copy ---


def main() -> None:
    import json
    key = fred_key()
    today = datetime.now(LA).date()
    d = today - timedelta(days=120)
    s0 = d - timedelta(days=(d.weekday() + 1) % 7)  # the Sunday on or before today - 120
    full_start = window_start(today, 10) - timedelta(days=62)
    OUT.mkdir(parents=True, exist_ok=True)
    rows = []
    with httpx.Client(timeout=60) as c:
        for sid in FRED_IDS:
            base = {"series_id": sid, "api_key": key, "file_type": "json"}
            full = get(c, FRED_URL, base | {"observation_start": full_start.isoformat()})
            (OUT / f"fred_{sid}_full.json").write_bytes(full)
            (OUT / f"fred_{sid}_tail.json").write_bytes(
                get(c, FRED_URL, base | {"observation_start": (s0 + timedelta(days=1)).isoformat()}))
            obs = [(date.fromisoformat(o["date"]), float(o["value"])) for o in json.loads(full)["observations"]
                   if o["value"] not in (".", "")]
            for years in (1, 5, 10):
                s = summarize(obs, today, years)
                rows.append('    {"%s", %d, %d, %r, %r, %r, %s, %d, %d, %s, %r, %r},' % (
                    sid, years, s["n"], s["last"], s["ratio"], s["yoy"], "true" if s["has_yoy"] else "false",
                    s["last_date"], s["window_start"], "true" if s["short"] else "false", s["first_norm"], s["last_norm"]))
        (OUT / "fred_error_bad_key.json").write_bytes(
            get(c, FRED_URL, {"series_id": "SP500", "api_key": "0" * 32, "file_type": "json"}, expect=400))
        (OUT / "weather_la.json").write_bytes(get(c, "https://api.open-meteo.com/v1/forecast", {
            "latitude": 34.1, "longitude": -118.2, "current": "temperature_2m,weather_code",
            "daily": "weather_code,temperature_2m_max,temperature_2m_min", "temperature_unit": "fahrenheit",
            "timezone": "America/Los_Angeles", "forecast_days": 8}))
    now = datetime.combine(today, time(10, 0), LA)
    (OUT / "fixtures.h").write_text(
        "#pragma once\n// Generated by tools/record_fixtures.py; do not edit.\n"
        f"#define FIXTURE_TODAY {(today - EPOCH).days}  // {today}\n"
        f"#define FIXTURE_NOW {int(now.timestamp())}LL  // {now.isoformat()}\n"
        f"#define FIXTURE_TAIL_S0 {(s0 - EPOCH).days}  // {s0}\n"
        f"#define FIXTURE_FULL_START {(full_start - EPOCH).days}  // {full_start}\n")
    (OUT / "summaries.h").write_text(
        "#pragma once\n// Generated by tools/record_fixtures.py (Python summarize()); do not edit.\n#include <stdint.h>\n"
        "struct SummaryExpect { const char* fred_id; int years; int n; double last, ratio, yoy; bool has_yoy;\n"
        "                       int32_t last_date, window_start; bool short_history; double first_norm, last_norm; };\n"
        "static const SummaryExpect SUMMARIES[] = {\n" + "\n".join(rows) + "\n};\n")
    print(f"recorded fixtures for {today}")


if __name__ == "__main__":
    main()
```

Run: `uv run tools/record_fixtures.py`
Expected: `recorded fixtures for <today>`; `ls -la test/fixtures` shows 15 JSON files plus two headers. Check none contains the key: `grep -l "$(sed -n 's/.*FRED_API_KEY "\(.*\)".*/\1/p' include/secrets.h)" test/fixtures/* || echo clean` → `clean`.

- [ ] **Step 2: Write the failing Open-Meteo tests**

Add to `test/test_data/test_main.cpp`:

```cpp
#include "sources/openmeteo.h"
#include "../fixtures/fixtures.h"

static bool parse_weather(const std::string& doc, size_t chunk, ink::Weather& w) {
  ink::OpenMeteoParser h;
  ink::json::Parser p(h);
  for (size_t i = 0; i < doc.size(); i += chunk) p.feed(doc.data() + i, std::min(chunk, doc.size() - i));
  return p.finish() && h.result(w);
}

void test_openmeteo_path() {
  char b[400];
  ink::openmeteo_path(b, sizeof b, 34.1, -118.2, false, "America/Los_Angeles");
  TEST_ASSERT_EQUAL_STRING("/v1/forecast?latitude=34.1&longitude=-118.2&current=temperature_2m,weather_code"
                           "&daily=weather_code,temperature_2m_max,temperature_2m_min&temperature_unit=fahrenheit"
                           "&timezone=America%2FLos_Angeles&forecast_days=8", b);
  ink::openmeteo_path(b, sizeof b, 0.0, 0.0, true, "UTC");
  TEST_ASSERT_NOT_NULL(strstr(b, "temperature_unit=celsius&timezone=UTC&"));
}

void test_openmeteo_fixture_any_chunking() {
  std::string doc;
  TEST_ASSERT_TRUE(ink_test::read_file(ink_test::path("fixtures/weather_la.json"), doc));
  ink::Weather a, b;
  TEST_ASSERT_TRUE(parse_weather(doc, doc.size(), a));
  TEST_ASSERT_TRUE(parse_weather(doc, 7, b));
  TEST_ASSERT_EQUAL_MEMORY(&a, &b, sizeof a);
  TEST_ASSERT_EQUAL_UINT8(8, a.n_daily);
  TEST_ASSERT_EQUAL_INT32(FIXTURE_TODAY, a.daily[0].day);
  TEST_ASSERT_TRUE(a.temp > -60 && a.temp < 140);
}

void test_openmeteo_skips_null_rows_and_requires_current() {
  ink::Weather w;
  const std::string ok = R"({"current":{"temperature_2m":70.4,"weather_code":2},"daily":{
    "time":["2026-09-27","2026-09-28","2026-09-29"],"weather_code":[0,null,3],
    "temperature_2m_max":[80.0,81.0,82.5],"temperature_2m_min":[60.0,61.0]}})";
  TEST_ASSERT_TRUE(parse_weather(ok, 5, w));
  TEST_ASSERT_EQUAL_DOUBLE(70.4, w.temp);
  TEST_ASSERT_EQUAL_INT16(2, w.code);
  TEST_ASSERT_EQUAL_UINT8(1, w.n_daily);  // zip() stops at the shortest column; row 2 has a null code
  TEST_ASSERT_EQUAL_DOUBLE(80.0, w.daily[0].hi);
  TEST_ASSERT_FALSE(parse_weather(R"({"current":{"temperature_2m":null,"weather_code":2},"daily":{"time":[],
    "weather_code":[],"temperature_2m_max":[],"temperature_2m_min":[]}})", 4, w));
  TEST_ASSERT_FALSE(parse_weather(R"({"current":{"temperature_2m":1,"weather_code":2}})", 4, w));
  TEST_ASSERT_FALSE(parse_weather(R"({"error":true,"reason":"Parameter 'latitude' is out of range"})", 4, w));
}
```

Run: `pio test -e native -f test_data` → Expected: compile error (`sources/openmeteo.h` missing).

- [ ] **Step 3: Implement `include/sources/openmeteo.h`**

```cpp
#pragma once
// Open-Meteo forecast: request path and streaming parser (spec §2.4).
// Port of server/inkboard_server/sources/openmeteo.py.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "civil.h"
#include "json_stream.h"

namespace ink {

constexpr int FORECAST_DAYS = 8;
constexpr const char* OPENMETEO_HOST = "api.open-meteo.com";

struct DayForecast {
  int32_t day;
  int16_t code;
  double hi, lo;
};

struct Weather {
  double temp = 0;
  int16_t code = 0;
  uint8_t n_daily = 0;
  DayForecast daily[FORECAST_DAYS] = {};
};

inline void openmeteo_path(char* out, size_t n, double lat, double lon, bool metric, const char* tz) {
  char tzq[128];
  size_t k = 0;
  for (const char* c = tz; *c && k + 4 < sizeof tzq; ++c) {
    if (*c == '/') k += static_cast<size_t>(snprintf(tzq + k, sizeof tzq - k, "%%2F"));
    else tzq[k++] = *c;  // IANA names are letters, digits, '_', '-', '+'
  }
  tzq[k] = '\0';
  snprintf(out, n,
           "/v1/forecast?latitude=%.1f&longitude=%.1f&current=temperature_2m,weather_code"
           "&daily=weather_code,temperature_2m_max,temperature_2m_min&temperature_unit=%s&timezone=%s&forecast_days=%d",
           lat, lon, metric ? "celsius" : "fahrenheit", tzq, FORECAST_DAYS);
}

class OpenMeteoParser : public json::Handler {
 public:
  void scalar(const json::Parser& p, json::Type t, const char* s) override {
    const bool num = t == json::Type::Number;
    if (p.at({"current", "temperature_2m"})) {
      has_temp_ = num;
      if (num) temp_ = strtod(s, nullptr);
      return;
    }
    if (p.at({"current", "weather_code"})) {
      has_code_ = num;
      if (num) code_ = static_cast<int16_t>(strtod(s, nullptr));
      return;
    }
    if (p.depth() != 3 || p.is_array(1) || !p.is_array(2) || strcmp(p.key(0), "daily") != 0) return;
    const int col = column(p.key(1));
    const int32_t i = p.index(2);
    if (col < 0 || i >= FORECAST_DAYS) return;
    if (i + 1 > len_[col]) len_[col] = i + 1;
    ok_[col][i] = col == 0 ? t == json::Type::String : num;
    if (col == 0) date_[i] = ok_[0][i] ? parse_iso_date(s) : INT32_MIN;
    else if (num) val_[col][i] = strtod(s, nullptr);
  }

  void end_container(const json::Parser& p) override {
    if (p.depth() == 2 && strcmp(p.key(0), "daily") == 0 && !p.is_array(1)) {
      const int col = column(p.key(1));
      if (col >= 0) seen_ |= 1 << col;
    }
  }

  // false when a field the server required is missing (its parse_forecast raised).
  bool result(Weather& out) const {
    if (!has_temp_ || !has_code_ || seen_ != 0xF) return false;
    out = Weather{};
    out.temp = temp_;
    out.code = code_;
    int n = len_[0];
    for (int c = 1; c < 4; ++c) n = len_[c] < n ? len_[c] : n;  // zip()
    for (int i = 0; i < n; ++i) {
      if (!ok_[1][i] || !ok_[2][i] || !ok_[3][i] || date_[i] == INT32_MIN) continue;
      out.daily[out.n_daily++] = DayForecast{date_[i], static_cast<int16_t>(val_[1][i]), val_[2][i], val_[3][i]};
    }
    return true;
  }

 private:
  static int column(const char* k) {
    if (strcmp(k, "time") == 0) return 0;
    if (strcmp(k, "weather_code") == 0) return 1;
    if (strcmp(k, "temperature_2m_max") == 0) return 2;
    if (strcmp(k, "temperature_2m_min") == 0) return 3;
    return -1;
  }

  double temp_ = 0;
  int16_t code_ = 0;
  bool has_temp_ = false, has_code_ = false;
  int seen_ = 0;
  int len_[4] = {0, 0, 0, 0};
  bool ok_[4][FORECAST_DAYS] = {};
  int32_t date_[FORECAST_DAYS] = {};
  double val_[4][FORECAST_DAYS] = {};
};

}  // namespace ink
```

- [ ] **Step 4: Run**

Run: `pio test -e native -f test_data`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add tools/record_fixtures.py test/fixtures include/sources/openmeteo.h test/test_data/test_main.cpp
git commit -m "Data: recorded API fixtures and the Open-Meteo parser

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

### Task 10: FRED parser, Sunday resampler, market summaries

**Files:**
- Create: `include/sources/fred.h`, `include/market_data.h`
- Test: `test/test_data/test_main.cpp`

**Interfaces:**
- Consumes: Task 3 (`civil.h`), Task 7 (`series.h`), Task 8 (`json_stream.h`), Task 9 fixtures.
- Produces (`namespace ink`):

```cpp
// sources/fred.h
constexpr int MAX_POINTS = 528;            // 10 years of Sundays (≤ 523) plus slack
constexpr const char* FRED_HOST = "api.stlouisfed.org";
void fred_path(char* out, size_t n, const char* fred_id, const char* api_key, int32_t observation_start);
struct SeriesData {
  int32_t first_sunday = INT32_MIN;        // day of values[0]
  uint16_t n = 0;
  double values[MAX_POINTS];               // last observation on or before first_sunday + 7 * i
  int32_t latest_date = INT32_MIN;         // newest observation
  double latest_value = 0;
  int32_t covered_from = INT32_MIN;        // Sundays >= this are complete
};
class SundayResampler {
 public:
  void begin_full(SeriesData& out, int32_t covered_from, int32_t today);
  bool begin_tail(SeriesData& out, const SeriesData& base, int32_t s0, int32_t today);
  void add(int32_t date, double value);
  bool finish(int32_t keep_from);
  int count() const;
};
class FredParser : public json::Handler {
 public:
  explicit FredParser(SundayResampler& r);
  bool saw_observations() const;           // the "observations" array was present
  bool api_key_error() const;              // error_message mentions api_key
};
// market_data.h
inline constexpr double TICKS[6] = {0.25, 0.5, 1, 1.5, 2, 3};
int32_t window_start_day(int32_t today, int years);   // today - round(365.25 * years), half to even
struct Summary {
  int series;                              // CATALOG index
  int n;
  int32_t dates[MAX_POINTS + 1];
  double norm[MAX_POINTS + 1];             // value / window mean
  double last, ratio, yoy; bool has_yoy;
  int32_t last_date, window_start; bool short_history;
};
bool summarize(int series, const SeriesData& d, int32_t today, int years, Summary& out);  // false: no points
void y_range(const Summary* const* s, int n, double& lo, double& hi);
int log_ticks(double lo, double hi, double* out);    // returns count, out has room for 6
```

- [ ] **Step 1: Write the failing tests**

Add to `test/test_data/test_main.cpp`:

```cpp
#include "market_data.h"
#include "sources/fred.h"
#include "../fixtures/summaries.h"

static bool feed_fred(const char* file, ink::SundayResampler& r, size_t chunk, ink::FredParser** out_parser = nullptr) {
  std::string doc;
  TEST_ASSERT_TRUE_MESSAGE(ink_test::read_file(ink_test::path(file), doc), file);
  static ink::FredParser* keep = nullptr;
  delete keep;
  keep = new ink::FredParser(r);
  ink::json::Parser p(*keep);
  for (size_t i = 0; i < doc.size(); i += chunk) p.feed(doc.data() + i, std::min(chunk, doc.size() - i));
  if (out_parser) *out_parser = keep;
  return p.finish() && keep->saw_observations();
}

static ink::SeriesData g_full, g_base, g_merged;

void test_fred_path() {
  char b[300];
  ink::fred_path(b, sizeof b, "SP500", "KEY", ink::days_from_civil(2016, 7, 30));
  TEST_ASSERT_EQUAL_STRING("/fred/series/observations?series_id=SP500&api_key=KEY&file_type=json&observation_start=2016-07-30", b);
}

void test_resampler_matches_python_summaries() {
  for (const SummaryExpect& e : SUMMARIES) {
    char file[64];
    snprintf(file, sizeof file, "fixtures/fred_%s_full.json", e.fred_id);
    ink::SundayResampler r;
    const int32_t g0 = ink::first_sunday_on_or_after(ink::window_start_day(FIXTURE_TODAY, 10));
    r.begin_full(g_full, g0, FIXTURE_TODAY);
    TEST_ASSERT_TRUE(feed_fred(file, r, 4096));
    TEST_ASSERT_TRUE(r.finish(g0));
    static ink::Summary s;
    TEST_ASSERT_TRUE(ink::summarize(0, g_full, FIXTURE_TODAY, e.years, s));
    TEST_ASSERT_EQUAL_INT_MESSAGE(e.n, s.n, e.fred_id);
    TEST_ASSERT_EQUAL_DOUBLE(e.last, s.last);
    TEST_ASSERT_EQUAL_DOUBLE(e.ratio, s.ratio);      // same summation order: bit-exact
    TEST_ASSERT_EQUAL(e.has_yoy, s.has_yoy);
    if (e.has_yoy) TEST_ASSERT_EQUAL_DOUBLE(e.yoy, s.yoy);
    TEST_ASSERT_EQUAL_INT32(e.last_date, s.last_date);
    TEST_ASSERT_EQUAL_INT32(e.window_start, s.window_start);
    TEST_ASSERT_EQUAL(e.short_history, s.short_history);
    TEST_ASSERT_EQUAL_DOUBLE(e.first_norm, s.norm[0]);
    TEST_ASSERT_EQUAL_DOUBLE(e.last_norm, s.norm[s.n - 1]);
  }
}

void test_tail_merge_equals_full_fetch() {
  const char* ids[] = {"SP500", "CBBTCUSD", "MORTGAGE30US", "MEDLISPRI31080", "DGS10", "DTWEXBGS"};
  const int32_t g0 = ink::first_sunday_on_or_after(ink::window_start_day(FIXTURE_TODAY, 10));
  for (const char* id : ids) {
    char full[64], tail[64];
    snprintf(full, sizeof full, "fixtures/fred_%s_full.json", id);
    snprintf(tail, sizeof tail, "fixtures/fred_%s_tail.json", id);
    ink::SundayResampler r;
    r.begin_full(g_full, g0, FIXTURE_TODAY);            // today's full fetch
    TEST_ASSERT_TRUE(feed_fred(full, r, 997));
    TEST_ASSERT_TRUE(r.finish(g0));
    r.begin_full(g_base, g0, FIXTURE_TAIL_S0);          // an older full fetch, made on day S0
    TEST_ASSERT_TRUE(feed_fred(full, r, 997));          // observations after S0 are ignored (date > today)
    TEST_ASSERT_TRUE(r.finish(g0));
    TEST_ASSERT_TRUE(r.begin_tail(g_merged, g_base, FIXTURE_TAIL_S0, FIXTURE_TODAY));
    TEST_ASSERT_TRUE(feed_fred(tail, r, 61));
    TEST_ASSERT_TRUE(r.finish(g0));
    TEST_ASSERT_EQUAL_INT32_MESSAGE(g_full.first_sunday, g_merged.first_sunday, id);
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(g_full.n, g_merged.n, id);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(g_full.values, g_merged.values, sizeof(double) * g_full.n, id);
    TEST_ASSERT_EQUAL_INT32(g_full.latest_date, g_merged.latest_date);
    TEST_ASSERT_EQUAL_DOUBLE(g_full.latest_value, g_merged.latest_value);
  }
}

void test_resampler_rejects_unsorted() {
  ink::SundayResampler r;
  r.begin_full(g_full, ink::days_from_civil(2026, 1, 4), ink::days_from_civil(2026, 2, 1));
  r.add(ink::days_from_civil(2026, 1, 10), 5);
  r.add(ink::days_from_civil(2026, 1, 9), 6);
  TEST_ASSERT_FALSE(r.finish(ink::days_from_civil(2026, 1, 4)));
}

void test_dot_values_and_duplicates() {
  ink::SundayResampler r;
  const int32_t sun = ink::days_from_civil(2026, 1, 4);
  r.begin_full(g_full, sun, sun + 14);
  const char* doc = R"({"observations":[{"date":"2026-01-02","value":"10"},{"date":"2026-01-05","value":"."},
    {"date":"2026-01-06","value":"12"},{"date":"2026-01-06","value":"11"},{"date":"2026-01-16","value":""}]})";
  ink::FredParser fp(r);
  ink::json::Parser p(fp);
  p.feed(doc, strlen(doc));
  TEST_ASSERT_TRUE(p.finish() && fp.saw_observations());
  TEST_ASSERT_TRUE(r.finish(sun));
  TEST_ASSERT_EQUAL_UINT16(3, g_full.n);             // Jan 4, 11, 18
  TEST_ASSERT_EQUAL_DOUBLE(10, g_full.values[0]);
  TEST_ASSERT_EQUAL_DOUBLE(12, g_full.values[1]);    // duplicate date: the larger value, as sorted() pairs give
  TEST_ASSERT_EQUAL_INT32(ink::days_from_civil(2026, 1, 6), g_full.latest_date);
}

void test_fred_bad_key_detected() {
  ink::SundayResampler r;
  r.begin_full(g_full, FIXTURE_TAIL_S0, FIXTURE_TODAY);
  ink::FredParser* fp = nullptr;
  TEST_ASSERT_FALSE(feed_fred("fixtures/fred_error_bad_key.json", r, 13, &fp));
  TEST_ASSERT_TRUE(fp->api_key_error());
}

void test_y_range_and_ticks() {
  static ink::Summary a, b;
  a.n = 2; a.norm[0] = 0.5; a.norm[1] = 1.2;
  b.n = 1; b.norm[0] = 2.0;
  const ink::Summary* s[] = {&a, &b};
  double lo, hi, t[6];
  ink::y_range(s, 2, lo, hi);
  TEST_ASSERT_EQUAL_DOUBLE(0.5 * 0.92, lo);
  TEST_ASSERT_EQUAL_DOUBLE(2.0 * 1.08, hi);
  TEST_ASSERT_EQUAL_INT(4, ink::log_ticks(lo, hi, t));  // 0.5, 1, 1.5, 2
  TEST_ASSERT_EQUAL_DOUBLE(0.5, t[0]);
  TEST_ASSERT_EQUAL_INT32(ink::days_from_civil(2024, 9, 27), ink::window_start_day(ink::days_from_civil(2026, 9, 27), 2));
}
```

The last assertion pins Python's `round(730.5) == 730` (half to even): 2026-09-27 minus 730 days is 2024-09-27 (731 would give 09-26).

Run: `pio test -e native -f test_data` → Expected: compile error (`market_data.h` missing).

- [ ] **Step 2: Implement `include/sources/fred.h`**

```cpp
#pragma once
// FRED observations: request path, streaming parser, and the weekly (Sunday) resampler
// (spec §2.5). The resampler is market_data.py's resample() run while the body streams in,
// so a 10-year daily series never sits in RAM.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "civil.h"
#include "json_stream.h"

namespace ink {

constexpr int MAX_POINTS = 528;
constexpr const char* FRED_HOST = "api.stlouisfed.org";

inline void fred_path(char* out, size_t n, const char* fred_id, const char* api_key, int32_t observation_start) {
  const Ymd d = civil_from_days(observation_start);
  snprintf(out, n, "/fred/series/observations?series_id=%s&api_key=%s&file_type=json&observation_start=%04d-%02d-%02d",
           fred_id, api_key, d.y, d.m, d.d);
}

struct SeriesData {
  int32_t first_sunday = INT32_MIN;
  uint16_t n = 0;
  double values[MAX_POINTS] = {};
  int32_t latest_date = INT32_MIN;
  double latest_value = 0;
  int32_t covered_from = INT32_MIN;
};

class SundayResampler {
 public:
  void begin_full(SeriesData& out, int32_t covered_from, int32_t today) {
    start(out, today);
    out.covered_from = covered_from;
    next_ = covered_from;
  }

  // Keeps base's Sundays up to s0 and recomputes the later ones from the tail response.
  // Exact: every observation the tail lacks is on or before s0, and values[s0] already
  // is the last of those.
  bool begin_tail(SeriesData& out, const SeriesData& base, int32_t s0, int32_t today) {
    if (base.n == 0 || s0 < base.first_sunday || (s0 - base.first_sunday) % 7 != 0) return false;
    const int k0 = (s0 - base.first_sunday) / 7;
    if (k0 >= base.n) return false;
    start(out, today);
    out.first_sunday = base.first_sunday;
    out.covered_from = base.covered_from;
    out.n = static_cast<uint16_t>(k0 + 1);
    memcpy(out.values, base.values, sizeof(double) * out.n);
    has_last_ = true;
    last_value_ = base.values[k0];
    next_ = s0 + 7;
    tail_ = true;
    s0_ = s0;
    base_latest_date_ = base.latest_date;
    base_latest_value_ = base.latest_value;
    return true;
  }

  void add(int32_t date, double value) {
    if (!ok_ || date == INT32_MIN || date > today_ || !(value > 0)) return;  // also drops NaN
    if (tail_ && date <= s0_) return;
    if (count_ > 0 && date < obs_date_) {
      ok_ = false;  // FRED sorts ascending; anything else would need the whole series in RAM
      return;
    }
    if (count_ > 0 && date == obs_date_) {
      if (value > obs_value_) obs_value_ = last_value_ = value;  // sorted((d, v)) puts the larger value last
      return;
    }
    while (next_ <= today_ && next_ < date) emit();
    has_last_ = true;
    last_value_ = obs_value_ = value;
    obs_date_ = date;
    ++count_;
  }

  bool finish(int32_t keep_from) {
    if (!ok_) return false;
    while (next_ <= today_) emit();
    SeriesData& o = *out_;
    if (count_ > 0) {
      o.latest_date = obs_date_;
      o.latest_value = obs_value_;
    } else if (tail_ && base_latest_date_ <= s0_) {
      o.latest_date = base_latest_date_;
      o.latest_value = base_latest_value_;
    } else {
      return false;  // nothing usable (server: "FRED returned no observations"), or the tail lost data
    }
    if (o.n > 0 && keep_from > o.first_sunday) {
      int drop = (keep_from - o.first_sunday + 6) / 7;
      if (drop > o.n) drop = o.n;
      memmove(o.values, o.values + drop, sizeof(double) * static_cast<size_t>(o.n - drop));
      o.n = static_cast<uint16_t>(o.n - drop);
      o.first_sunday = o.n ? o.first_sunday + 7 * drop : INT32_MIN;
    }
    if (keep_from > o.covered_from) o.covered_from = first_sunday_on_or_after(keep_from);
    return true;
  }

  int count() const { return count_; }

 private:
  void start(SeriesData& out, int32_t today) {
    out_ = &out;
    out = SeriesData{};
    today_ = today;
    has_last_ = tail_ = false;
    ok_ = true;
    count_ = 0;
    obs_date_ = INT32_MIN;
  }

  void emit() {
    if (has_last_) {
      SeriesData& o = *out_;
      if (o.n == MAX_POINTS) {  // long offline spell: drop the oldest; finish() trims to the window anyway
        memmove(o.values, o.values + 1, sizeof(double) * (MAX_POINTS - 1));
        --o.n;
        o.first_sunday += 7;
      }
      if (o.n == 0) o.first_sunday = next_;
      o.values[o.n++] = last_value_;
    }
    next_ += 7;
  }

  SeriesData* out_ = nullptr;
  int32_t today_ = 0, next_ = 0, s0_ = 0, obs_date_ = INT32_MIN, base_latest_date_ = INT32_MIN;
  double last_value_ = 0, obs_value_ = 0, base_latest_value_ = 0;
  bool has_last_ = false, tail_ = false, ok_ = true;
  int count_ = 0;
};

class FredParser : public json::Handler {
 public:
  explicit FredParser(SundayResampler& r) : r_(r) {}

  void scalar(const json::Parser& p, json::Type t, const char* s) override {
    if (p.at({"observations", nullptr, "date"})) {
      date_ = t == json::Type::String ? parse_iso_date(s) : INT32_MIN;
    } else if (p.at({"observations", nullptr, "value"})) {
      snprintf(value_, sizeof value_, "%s", t == json::Type::String ? s : "");
    } else if (p.at({"error_message"}) && t == json::Type::String) {
      key_error_ = strstr(s, "api_key") != nullptr;
    }
  }

  void end_container(const json::Parser& p) override {
    if (p.at({"observations", nullptr})) {
      if (date_ != INT32_MIN && value_[0] != '\0' && strcmp(value_, ".") != 0) {
        char* end = nullptr;
        const double v = strtod(value_, &end);
        if (end != value_ && *end == '\0') r_.add(date_, v);
      }
      date_ = INT32_MIN;
      value_[0] = '\0';
    } else if (p.at({"observations"})) {
      saw_ = true;
    }
  }

  bool saw_observations() const { return saw_; }
  bool api_key_error() const { return key_error_; }

 private:
  SundayResampler& r_;
  int32_t date_ = INT32_MIN;
  char value_[32] = "";
  bool saw_ = false, key_error_ = false;
};

}  // namespace ink
```

`end_container` for the observations array: after the array closes, `p.depth()` is 1 and `p.key(0)` is still `"observations"`, so `p.at({"observations"})` matches.

- [ ] **Step 3: Implement `include/market_data.h`**

```cpp
#pragma once
// Weekly grid, normalization and chart range for market_trends (spec §2.5).
// Port of server/inkboard_server/market_data.py on top of the Sunday cache.
#include <math.h>
#include <stdint.h>

#include "civil.h"
#include "sources/fred.h"

namespace ink {

inline constexpr double TICKS[6] = {0.25, 0.5, 1, 1.5, 2, 3};

inline int32_t window_start_day(int32_t today, int years) {
  return today - static_cast<int32_t>(nearbyint(365.25 * years));  // round() is half-to-even
}

struct Summary {
  int series = 0;
  int n = 0;
  int32_t dates[MAX_POINTS + 1] = {};
  double norm[MAX_POINTS + 1] = {};
  double last = 0, ratio = 0, yoy = 0;
  bool has_yoy = false;
  int32_t last_date = 0, window_start = 0;
  bool short_history = false;
};

// Last observation on or before g, from the Sunday cache plus the latest observation.
inline bool value_on_or_before(const SeriesData& d, int32_t g, double& v) {
  if (d.latest_date == INT32_MIN) return false;
  if (g >= d.latest_date) {
    v = d.latest_value;
    return true;
  }
  if (d.n == 0 || g < d.first_sunday) return false;
  int32_t k = (g - d.first_sunday) / 7;
  if (k >= d.n) k = d.n - 1;
  v = d.values[k];
  return true;
}

inline bool summarize(int series, const SeriesData& d, int32_t today, int years, Summary& out) {
  out = Summary{};
  out.series = series;
  const int32_t g0 = first_sunday_on_or_after(window_start_day(today, years));
  double v;
  for (int32_t g = g0; g <= today; g += 7)
    if (value_on_or_before(d, g, v)) {
      out.dates[out.n] = g;
      out.norm[out.n++] = v;
    }
  if (weekday(today) != 6 && value_on_or_before(d, today, v)) {  // weekly_grid appends a non-Sunday today
    out.dates[out.n] = today;
    out.norm[out.n++] = v;
  }
  if (out.n == 0) return false;
  double sum = 0;
  for (int i = 0; i < out.n; ++i) sum += out.norm[i];  // same order as Python's sum()
  const double mean = sum / out.n;
  out.last = out.norm[out.n - 1];
  out.ratio = out.last / mean;
  const int32_t year_ago = today - 364;
  for (int i = 0; i < out.n; ++i)
    if (out.dates[i] <= year_ago) {
      out.has_yoy = true;
      out.yoy = out.last / out.norm[i] - 1;  // overwritten until the last such point
    }
  out.last_date = d.latest_date;
  out.window_start = out.dates[0];
  out.short_history = out.dates[0] > g0 + 31;
  for (int i = 0; i < out.n; ++i) out.norm[i] /= mean;
  return true;
}
```

Careful: in the `has_yoy` loop the ratio must use the **raw** value, which is why it runs before the final division (as written).

```cpp
inline void y_range(const Summary* const* s, int n, double& lo, double& hi) {
  lo = INFINITY;
  hi = -INFINITY;
  for (int k = 0; k < n; ++k)
    for (int i = 0; i < s[k]->n; ++i) {
      if (s[k]->norm[i] < lo) lo = s[k]->norm[i];
      if (s[k]->norm[i] > hi) hi = s[k]->norm[i];
    }
  lo *= 0.92;
  hi *= 1.08;
}

inline int log_ticks(double lo, double hi, double* out) {
  int n = 0;
  for (double g : TICKS)
    if (lo <= g && g <= hi) out[n++] = g;
  return n;
}

}  // namespace ink
```

- [ ] **Step 4: Run**

Run: `pio test -e native -f test_data`
Expected: PASS. If `test_resampler_matches_python_summaries` fails on `ratio` by one ulp, check that `sum` adds the raw values in grid order before normalizing (Python: `sum(v for _, v in pts) / len(pts)`).

- [ ] **Step 5: Commit**

```bash
git add include/sources/fred.h include/market_data.h test/test_data/test_main.cpp
git commit -m "Data: FRED streaming parser with an exact Sunday resampler and tail merge

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

### Task 11: Cache records, due/stale rules, FRED fetch planning

**Files:**
- Create: `include/data_cache.h`
- Test: `test/test_data/test_main.cpp`

**Interfaces:**
- Consumes: `Weather` (Task 9), `SeriesData`, `window_start_day` (Task 10), `crc32` (Task 3).
- Produces (`namespace ink`):

```cpp
constexpr int64_t WEATHER_TTL_S = 1800, FRED_TTL_S = 21600, STALE_GRACE_S = 5400,
                  FULL_REFETCH_S = 7 * 86400, RETRY_SPACING_S = 300;
struct SourceStatus { int64_t fetched_at = 0; int64_t retry_not_before = 0;
                      bool last_attempt_failed = false; bool auth_rejected = false; };
struct FetchResult { int http_status = -1; bool complete = false; int32_t retry_after_s = -1;
                     int64_t date_epoch = -1; bool auth_error = false; };
bool fetch_ok(const FetchResult& r);                       // 200 and complete
bool is_due(const SourceStatus& s, int64_t now, int64_t ttl);
bool is_stale(const SourceStatus& s, int64_t now, int64_t ttl);
void record_success(SourceStatus& s, int64_t now);
void record_failure(SourceStatus& s, const FetchResult& r, int64_t now);
struct WeatherKey { double lat, lon; bool metric; char tz[64]; };
bool same_key(const WeatherKey& a, const WeatherKey& b);
struct WeatherCache { bool valid = false; WeatherKey key = {}; Weather data = {}; SourceStatus status = {}; };
struct SeriesCache { bool valid = false; char fred_id[16] = ""; SeriesData data = {}; int64_t full_fetched_at = 0;
                     SourceStatus status = {}; };
bool weather_due(const WeatherCache& c, const WeatherKey& want, int64_t now);
bool weather_usable(const WeatherCache& c, const WeatherKey& want);    // valid and same key
enum class FredFetch : uint8_t { None, Full, Tail };
struct FredPlan { FredFetch kind; int32_t observation_start; int32_t s0; int32_t keep_from; };
FredPlan plan_fred(const SeriesCache& c, int64_t now, int32_t today, int years);
enum class CacheKind : uint16_t { Weather = 1, Series = 2 };
constexpr size_t CACHE_HEADER_BYTES = 16;
template <class T> size_t encode_cache(CacheKind k, const T& rec, uint8_t* out, size_t cap);  // 0 if too small
template <class T> bool decode_cache(CacheKind k, const uint8_t* in, size_t len, T& rec);
```

- [ ] **Step 1: Write the failing tests**

Add to `test/test_data/test_main.cpp`:

```cpp
#include "data_cache.h"

using ink::SourceStatus;

void test_due_rules() {
  SourceStatus s;
  TEST_ASSERT_TRUE(ink::is_due(s, 1000, 1800));             // never fetched
  s.fetched_at = 1000;
  TEST_ASSERT_FALSE(ink::is_due(s, 2799, 1800));
  TEST_ASSERT_TRUE(ink::is_due(s, 2800, 1800));
  s.retry_not_before = 3000;
  TEST_ASSERT_FALSE(ink::is_due(s, 2900, 1800));            // spacing after a failure
  TEST_ASSERT_TRUE(ink::is_due(s, 3000, 1800));
}

void test_due_when_clock_went_backwards() {
  SourceStatus s;
  s.fetched_at = 100000;
  TEST_ASSERT_TRUE(ink::is_due(s, 99000, 1800));            // negative age: refetch rather than trust
}

void test_stale_rules() {
  SourceStatus s;
  s.fetched_at = 1000;
  TEST_ASSERT_FALSE(ink::is_stale(s, 1000 + 1799, 1800));   // fresh is never stale (base.py)
  s.last_attempt_failed = true;
  TEST_ASSERT_FALSE(ink::is_stale(s, 1000 + 1799, 1800));
  TEST_ASSERT_TRUE(ink::is_stale(s, 1000 + 1800, 1800));    // expired and the refresh failed
  s.last_attempt_failed = false;
  TEST_ASSERT_FALSE(ink::is_stale(s, 1000 + 1800 + 5399, 1800));
  TEST_ASSERT_TRUE(ink::is_stale(s, 1000 + 1800 + 5400, 1800));
}

void test_record_failure_spacing() {
  SourceStatus s;
  ink::FetchResult r;
  r.http_status = 503;
  r.retry_after_s = 30;
  ink::record_failure(s, r, 1000);
  TEST_ASSERT_EQUAL_INT64(1300, s.retry_not_before);        // at least the 5-minute spacing
  r.retry_after_s = 900;
  ink::record_failure(s, r, 1000);
  TEST_ASSERT_EQUAL_INT64(1900, s.retry_not_before);
  TEST_ASSERT_TRUE(s.last_attempt_failed);
  ink::record_success(s, 2000);
  TEST_ASSERT_FALSE(s.last_attempt_failed);
  TEST_ASSERT_EQUAL_INT64(2000, s.fetched_at);
  TEST_ASSERT_EQUAL_INT64(0, s.retry_not_before);
}

void test_weather_key_mismatch_is_due() {
  ink::WeatherCache c;
  c.valid = true;
  c.key = ink::WeatherKey{34.1, -118.2, false, "America/Los_Angeles"};
  c.status.fetched_at = 1000;
  ink::WeatherKey want = c.key;
  TEST_ASSERT_FALSE(ink::weather_due(c, want, 1100));
  TEST_ASSERT_TRUE(ink::weather_usable(c, want));
  want.metric = true;
  TEST_ASSERT_TRUE(ink::weather_due(c, want, 1100));
  TEST_ASSERT_FALSE(ink::weather_usable(c, want));
  want = c.key;
  snprintf(want.tz, sizeof want.tz, "UTC");
  TEST_ASSERT_TRUE(ink::weather_due(c, want, 1100));
}

static ink::SeriesCache g_sc, g_sc2;

void test_encode_decode_roundtrip() {
  g_sc = ink::SeriesCache{};
  g_sc.valid = true;
  snprintf(g_sc.fred_id, sizeof g_sc.fred_id, "SP500");
  g_sc.data.n = 3;
  g_sc.data.values[2] = 42.5;
  static uint8_t buf[sizeof(ink::SeriesCache) + 64];
  const size_t n = ink::encode_cache(ink::CacheKind::Series, g_sc, buf, sizeof buf);
  TEST_ASSERT_EQUAL_size_t(sizeof(ink::SeriesCache) + ink::CACHE_HEADER_BYTES, n);
  TEST_ASSERT_TRUE(ink::decode_cache(ink::CacheKind::Series, buf, n, g_sc2));
  TEST_ASSERT_EQUAL_MEMORY(&g_sc, &g_sc2, sizeof g_sc);
  TEST_ASSERT_EQUAL_size_t(0, ink::encode_cache(ink::CacheKind::Series, g_sc, buf, 100));  // too small
}

void test_decode_rejects_bad_crc() {
  static uint8_t buf[sizeof(ink::SeriesCache) + 64];
  const size_t n = ink::encode_cache(ink::CacheKind::Series, g_sc, buf, sizeof buf);
  buf[n / 2] ^= 0x01;
  TEST_ASSERT_FALSE(ink::decode_cache(ink::CacheKind::Series, buf, n, g_sc2));
  buf[n / 2] ^= 0x01;
  TEST_ASSERT_FALSE(ink::decode_cache(ink::CacheKind::Series, buf, n - 1, g_sc2));       // truncated
  ink::WeatherCache w;
  TEST_ASSERT_FALSE(ink::decode_cache(ink::CacheKind::Weather, buf, n, w));             // wrong kind
}

void test_plan_fred() {
  const int32_t today = ink::days_from_civil(2026, 9, 30);
  const int64_t now = int64_t(today) * 86400 + 36000;
  ink::SeriesCache c;
  ink::FredPlan p = ink::plan_fred(c, now, today, 5);
  TEST_ASSERT_TRUE(p.kind == ink::FredFetch::Full);                  // nothing cached
  TEST_ASSERT_EQUAL_INT32(ink::window_start_day(today, 5) - 62, p.observation_start);
  const int32_t g0 = ink::first_sunday_on_or_after(ink::window_start_day(today, 5));
  TEST_ASSERT_EQUAL_INT32(g0, p.keep_from);
  c.valid = true;
  c.data.first_sunday = g0;
  c.data.n = static_cast<uint16_t>((today - g0) / 7 + 1);
  c.data.covered_from = g0;
  c.data.latest_date = today - 1;
  c.full_fetched_at = now - 3600;
  c.status.fetched_at = now - 3600;
  TEST_ASSERT_TRUE(ink::plan_fred(c, now, today, 5).kind == ink::FredFetch::None);    // fresh
  c.status.fetched_at = now - ink::FRED_TTL_S;
  p = ink::plan_fred(c, now, today, 5);
  TEST_ASSERT_TRUE(p.kind == ink::FredFetch::Tail);
  TEST_ASSERT_EQUAL_INT(6, ink::weekday(p.s0));
  TEST_ASSERT_TRUE(p.s0 <= today - 120 && p.s0 > today - 127);
  TEST_ASSERT_EQUAL_INT32(p.s0 + 1, p.observation_start);
  c.full_fetched_at = now - ink::FULL_REFETCH_S;
  TEST_ASSERT_TRUE(ink::plan_fred(c, now, today, 5).kind == ink::FredFetch::Full);    // weekly full refresh
  c.full_fetched_at = now - 3600;
  c.status.retry_not_before = now + 10;
  TEST_ASSERT_TRUE(ink::plan_fred(c, now, today, 5).kind == ink::FredFetch::None);    // spacing wins
}

void test_years_raised_needs_full() {
  const int32_t today = ink::days_from_civil(2026, 9, 30);
  const int64_t now = int64_t(today) * 86400;
  ink::SeriesCache c;
  c.valid = true;
  const int32_t g5 = ink::first_sunday_on_or_after(ink::window_start_day(today, 5));
  c.data.first_sunday = c.data.covered_from = g5;
  c.data.n = 10;
  c.data.latest_date = today;
  c.full_fetched_at = c.status.fetched_at = now;
  TEST_ASSERT_TRUE(ink::plan_fred(c, now, today, 5).kind == ink::FredFetch::None);
  TEST_ASSERT_TRUE(ink::plan_fred(c, now, today, 10).kind == ink::FredFetch::Full);   // needs older Sundays
}
```

`RUN_TEST` all nine. Run → Expected: compile error (`data_cache.h` missing).

- [ ] **Step 2: Implement `include/data_cache.h`**

```cpp
#pragma once
// Cached source data on the board (spec §2.5-§2.7): records, their binary file format,
// and the rules for when to fetch and when to call data stale. Pure; host-tested.
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <type_traits>

#include "civil.h"
#include "crc32.h"
#include "market_data.h"
#include "sources/fred.h"
#include "sources/openmeteo.h"

namespace ink {

constexpr int64_t WEATHER_TTL_S = 1800;     // the server's TTLs
constexpr int64_t FRED_TTL_S = 21600;
constexpr int64_t STALE_GRACE_S = 5400;     // base.py: stale once 90 min past the TTL
constexpr int64_t FULL_REFETCH_S = 7 * 86400;
constexpr int64_t RETRY_SPACING_S = 300;    // base.py retry_after

struct SourceStatus {
  int64_t fetched_at = 0;  // last success (unix s)
  int64_t retry_not_before = 0;
  bool last_attempt_failed = false;
  bool auth_rejected = false;
};

struct FetchResult {
  int http_status = -1;    // < 0: no HTTP response
  bool complete = false;   // whole body read and the parser reached the end of the document
  int32_t retry_after_s = -1;
  int64_t date_epoch = -1;
  bool auth_error = false;
};

inline bool fetch_ok(const FetchResult& r) { return r.http_status == 200 && r.complete; }

inline bool is_due(const SourceStatus& s, int64_t now, int64_t ttl) {
  if (now < s.retry_not_before) return false;
  if (s.fetched_at <= 0) return true;
  const int64_t age = now - s.fetched_at;
  return age < 0 || age >= ttl;  // a clock that went backwards: refetch rather than trust
}

inline bool is_stale(const SourceStatus& s, int64_t now, int64_t ttl) {
  const int64_t age = now - s.fetched_at;
  if (age < ttl) return false;
  return s.last_attempt_failed || age >= ttl + STALE_GRACE_S;
}

inline void record_success(SourceStatus& s, int64_t now) {
  s.fetched_at = now;
  s.retry_not_before = 0;
  s.last_attempt_failed = false;
  s.auth_rejected = false;
}

inline void record_failure(SourceStatus& s, const FetchResult& r, int64_t now) {
  s.last_attempt_failed = true;
  s.auth_rejected = r.auth_error;
  s.retry_not_before = now + (r.retry_after_s > RETRY_SPACING_S ? r.retry_after_s : RETRY_SPACING_S);
}

struct WeatherKey {
  double lat, lon;
  bool metric;
  char tz[64];
};

inline bool same_key(const WeatherKey& a, const WeatherKey& b) {
  return a.lat == b.lat && a.lon == b.lon && a.metric == b.metric && strcmp(a.tz, b.tz) == 0;
}

struct WeatherCache {
  bool valid = false;
  WeatherKey key = {};
  Weather data = {};
  SourceStatus status = {};
};

struct SeriesCache {
  bool valid = false;
  char fred_id[16] = "";
  SeriesData data = {};
  int64_t full_fetched_at = 0;
  SourceStatus status = {};
};

inline bool weather_usable(const WeatherCache& c, const WeatherKey& want) { return c.valid && same_key(c.key, want); }

inline bool weather_due(const WeatherCache& c, const WeatherKey& want, int64_t now) {
  if (!weather_usable(c, want)) return true;
  return is_due(c.status, now, WEATHER_TTL_S);
}

enum class FredFetch : uint8_t { None, Full, Tail };

struct FredPlan {
  FredFetch kind;
  int32_t observation_start;
  int32_t s0;         // Tail: the cached Sunday the tail starts after
  int32_t keep_from;  // first Sunday of the configured window
};

inline FredPlan plan_fred(const SeriesCache& c, int64_t now, int32_t today, int years) {
  const int32_t ws = window_start_day(today, years);
  const int32_t g0 = first_sunday_on_or_after(ws);
  const FredPlan none{FredFetch::None, 0, 0, g0};
  const FredPlan full{FredFetch::Full, ws - 62, 0, g0};  // 62 days back: a monthly series has a value by g0
  if (now < c.status.retry_not_before) return none;
  if (!c.valid || c.data.latest_date == INT32_MIN || c.data.n == 0 || c.data.covered_from == INT32_MIN ||
      c.data.covered_from > g0)
    return full;
  if (!is_due(c.status, now, FRED_TTL_S)) return none;
  if (now - c.full_fetched_at >= FULL_REFETCH_S || now < c.full_fetched_at) return full;
  const int32_t limit = today - 120;
  if (limit < c.data.first_sunday) return full;
  int32_t k = (limit - c.data.first_sunday) / 7;
  if (k >= c.data.n) k = c.data.n - 1;
  const int32_t s0 = c.data.first_sunday + 7 * k;
  return FredPlan{FredFetch::Tail, s0 + 1, s0, g0};
}

enum class CacheKind : uint16_t { Weather = 1, Series = 2 };

constexpr uint32_t CACHE_MAGIC = 0x434B4E49;  // "INKC"
constexpr uint16_t CACHE_VERSION = 1;          // bump when a record's layout changes
constexpr size_t CACHE_HEADER_BYTES = 16;

// [magic u32][version u16][kind u16][size u32][crc32 of the record u32][record bytes]
// Raw struct bytes: the same firmware writes and reads them; the version guards layout changes.
template <class T>
size_t encode_cache(CacheKind k, const T& rec, uint8_t* out, size_t cap) {
  static_assert(std::is_trivially_copyable<T>::value, "cache records are copied as bytes");
  if (cap < CACHE_HEADER_BYTES + sizeof(T)) return 0;
  const uint32_t magic = CACHE_MAGIC, size = sizeof(T);
  const uint16_t version = CACHE_VERSION, kind = static_cast<uint16_t>(k);
  memcpy(out + CACHE_HEADER_BYTES, &rec, sizeof(T));
  const uint32_t crc = crc32(out + CACHE_HEADER_BYTES, sizeof(T));
  memcpy(out, &magic, 4);
  memcpy(out + 4, &version, 2);
  memcpy(out + 6, &kind, 2);
  memcpy(out + 8, &size, 4);
  memcpy(out + 12, &crc, 4);
  return CACHE_HEADER_BYTES + sizeof(T);
}

template <class T>
bool decode_cache(CacheKind k, const uint8_t* in, size_t len, T& rec) {
  static_assert(std::is_trivially_copyable<T>::value, "cache records are copied as bytes");
  if (len != CACHE_HEADER_BYTES + sizeof(T)) return false;
  uint32_t magic, size, crc;
  uint16_t version, kind;
  memcpy(&magic, in, 4);
  memcpy(&version, in + 4, 2);
  memcpy(&kind, in + 6, 2);
  memcpy(&size, in + 8, 4);
  memcpy(&crc, in + 12, 4);
  if (magic != CACHE_MAGIC || version != CACHE_VERSION || kind != static_cast<uint16_t>(k) || size != sizeof(T) ||
      crc != crc32(in + CACHE_HEADER_BYTES, sizeof(T)))
    return false;
  memcpy(&rec, in + CACHE_HEADER_BYTES, sizeof(T));
  return true;
}

}  // namespace ink
```

`test_encode_decode_roundtrip` compares the whole struct with `TEST_ASSERT_EQUAL_MEMORY`; padding bytes are copied as-is by `memcpy`, so the comparison holds.

- [ ] **Step 3: Run**

Run: `pio test -e native -f test_data`
Expected: PASS.

- [ ] **Step 4: Commit**

```bash
git add include/data_cache.h test/test_data/test_main.cpp
git commit -m "Data: cache records with CRC, the server's due/stale rules, FRED fetch planning

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

## Phase 3: Widgets and compositor

All ports below keep the Python's arithmetic order and types (ints stay ints until Python divides
with `/`), because the pixels depend on it. When in doubt, the Python file named in each step is
the specification.

### Task 12: Icons, chart lines, month grid

**Files:**
- Create: `include/render/icons.h`, `include/render/lines.h`, `include/render/calendar.h`
- Create: `test/test_widgets/test_main.cpp`
- Test: `test/test_widgets/test_main.cpp`

**Interfaces:**
- Consumes: `View`, `Pt`, `BLACK`/`WHITE`/`NONE` (Task 4); `font`, `draw_text` (Task 5); `civil.h`, `format.h` (Task 3).
- Produces (`namespace ink`):

```cpp
// render/icons.h
enum class Icon : uint8_t { Sun, Part, Cloud, Fog, Rain, Snow, Storm };
struct WmoInfo { const char* label; Icon icon; };
WmoInfo wmo_info(int code);
void draw_icon(View& d, Icon kind, double cx, double cy, double r);
// render/lines.h
struct LineStyle { int width; int dash_on, dash_off; char marker; };   // dash_on 0 = solid; marker 's','c','t','d'
inline constexpr LineStyle STYLES[4];
constexpr double MARKER_STEP = 56;
void styled_line(View& d, const Pt* pts, int n, const LineStyle& s);
void draw_marker(View& d, double x, double y, char kind, int r = 4);
int marker_xs(double x0, double x1, int k, int n, double* out, int cap, double step = MARKER_STEP);
double y_at(const Pt* pts, int n, double x);
void line_with_markers(View& d, const Pt* pts, int n, const LineStyle& s, int k, int count);
void legend_sample(View& d, double x, double y, const LineStyle& s, int length = 24);
// render/calendar.h
int month_weeks(int y, int m, int weeks[6][7]);                 // Sunday first, 0 = padding; returns rows
int month_grid(View& d, double x, double y, double w, int32_t today, bool big = false);  // returns height
```

- [ ] **Step 1: Write the failing tests**

Create `test/test_widgets/test_main.cpp`:

```cpp
#include <unity.h>

#include "../support/ink_test.h"
#include "render/calendar.h"
#include "render/icons.h"
#include "render/lines.h"

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

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_wmo_mapping);
  RUN_TEST(test_month_weeks_sunday_first);
  RUN_TEST(test_marker_positions_and_y_at);
  return UNITY_END();
}
```

Run: `pio test -e native -f test_widgets` → Expected: compile error.

- [ ] **Step 2: Implement `include/render/icons.h`** (port of `server/inkboard_server/draw/icons.py`)

```cpp
#pragma once
// Geometric weather icons and the WMO code mapping (spec §3.3). Port of draw/icons.py.
#include <math.h>
#include <stdint.h>

#include "render/canvas.h"
#include "render/text.h"

namespace ink {

enum class Icon : uint8_t { Sun, Part, Cloud, Fog, Rain, Snow, Storm };

struct WmoInfo {
  const char* label;
  Icon icon;
};

inline WmoInfo wmo_info(int code) {
  if (code == 0) return {"Clear", Icon::Sun};
  if (code == 1) return {"Mostly clear", Icon::Sun};
  if (code == 2) return {"Partly cloudy", Icon::Part};
  if (code == 3) return {"Overcast", Icon::Cloud};
  if (code == 45 || code == 48) return {"Fog", Icon::Fog};
  if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) return {"Rain", Icon::Rain};
  if ((code >= 71 && code <= 77) || code == 85 || code == 86) return {"Snow", Icon::Snow};
  if (code >= 95 && code <= 99) return {"Storms", Icon::Storm};
  return {"\xE2\x80\x94", Icon::Cloud};
}

namespace icon_detail {

inline int max2(int a, int b) { return a > b ? a : b; }

inline void sun(View& d, double cx, double cy, double r) {
  const int w = max2(2, static_cast<int>(r * 0.12));
  d.ellipse(cx - r * .45, cy - r * .45, cx + r * .45, cy + r * .45, NONE, BLACK, w);
  for (int k = 0; k < 8; ++k) {
    const double a = k * M_PI / 4;
    d.line(cx + cos(a) * r * .65, cy + sin(a) * r * .65, cx + cos(a) * r * .95, cy + sin(a) * r * .95, BLACK,
           max2(2, static_cast<int>(r * .1)));
  }
}

inline void cloud(View& d, double cx, double cy, double r) {
  const int w = max2(2, static_cast<int>(r * .1));
  const double parts[3][3] = {{cx - r * .45, cy + r * .1, r * .38}, {cx, cy - r * .15, r * .5},
                              {cx + r * .45, cy + r * .12, r * .36}};
  for (const auto& p : parts) d.ellipse(p[0] - p[2], p[1] - p[2], p[0] + p[2], p[1] + p[2], NONE, BLACK, w);
  for (const auto& p : parts) d.ellipse(p[0] - p[2] + w, p[1] - p[2] + w, p[0] + p[2] - w, p[1] + p[2] - w, WHITE);
  d.rectangle(cx - r * .45, cy + r * .1, cx + r * .45, cy + r * .48 - w, WHITE);
  d.line(cx - r * .45, cy + r * .48, cx + r * .45, cy + r * .48, BLACK, w);
}

}  // namespace icon_detail

inline void draw_icon(View& d, Icon kind, double cx, double cy, double r) {
  using namespace icon_detail;
  const int lw = max2(2, static_cast<int>(r * .1));
  switch (kind) {
    case Icon::Sun: return sun(d, cx, cy, r);
    case Icon::Part:
      sun(d, cx - r * .3, cy - r * .3, r * .7);
      return cloud(d, cx + r * .1, cy + r * .15, r * .8);
    case Icon::Cloud: return cloud(d, cx, cy, r);
    case Icon::Rain:
    case Icon::Snow:
    case Icon::Storm:
      cloud(d, cx, cy - r * .2, r * .85);
      for (int k = 0; k < 3; ++k) {
        const double x = cx - r * .35 + k * r * .35;
        if (kind == Icon::Snow) {
          draw_text(d, x, cy + r * .55, "*", font(max2(8, static_cast<int>(r * .5)), true), BLACK, "mm");
        } else if (kind == Icon::Storm && k == 1) {
          const Pt bolt[] = {{x + r * .1, cy + r * .3}, {x - r * .1, cy + r * .55}, {x + r * .1, cy + r * .55},
                             {x - r * .1, cy + r * .85}};
          d.line(bolt, 4, BLACK, lw);
        } else {
          d.line(x, cy + r * .35, x - r * .12, cy + r * .75, BLACK, lw);
        }
      }
      return;
    case Icon::Fog:
      for (int k = 0; k < 4; ++k) {
        const double y = cy - r * .4 + k * r * .27;
        d.line(cx - r * .7, y, cx + r * .7, y, BLACK, lw);
      }
      return;
  }
}

}  // namespace ink
```

- [ ] **Step 3: Implement `include/render/lines.h`** (port of `draw/lines.py`)

```cpp
#pragma once
// Chart line styles for 1-bit output: width + dash + marker shape (spec §3.3). Port of draw/lines.py.
// Solid lines skip Pillow's joint="curve": Pillow only draws joints for width > 4.
#include <math.h>

#include "render/canvas.h"

namespace ink {

struct LineStyle {
  int width;
  int dash_on, dash_off;  // dash_on == 0: solid
  char marker;            // 's' square, 'c' circle, 't' triangle, 'd' diamond
};

inline constexpr LineStyle STYLES[4] = {{3, 0, 0, 's'}, {2, 7, 4, 'c'}, {1, 0, 0, 't'}, {2, 2, 3, 'd'}};
constexpr double MARKER_STEP = 56;

inline void styled_line(View& d, const Pt* pts, int n, const LineStyle& s) {
  if (n < 2) return;
  if (s.dash_on == 0) return d.line(pts, n, BLACK, s.width);
  const double on = s.dash_on, off = s.dash_off;
  double acc = 0;
  bool drawing = true;
  for (int i = 0; i + 1 < n; ++i) {
    const double x0 = pts[i].x, y0 = pts[i].y, x1 = pts[i + 1].x, y1 = pts[i + 1].y;
    const double seg = hypot(x1 - x0, y1 - y0);
    double t = 0;
    while (t < seg) {
      const double limit = drawing ? on : off;
      const double step = limit - acc < seg - t ? limit - acc : seg - t;
      if (drawing) {
        const double a = t / seg, b = (t + step) / seg;
        d.line(x0 + (x1 - x0) * a, y0 + (y1 - y0) * a, x0 + (x1 - x0) * b, y0 + (y1 - y0) * b, BLACK, s.width);
      }
      t += step;
      acc += step;
      if (acc >= limit) {
        acc = 0;
        drawing = !drawing;
      }
    }
  }
}

inline void draw_marker(View& d, double x, double y, char kind, int r = 4) {
  if (kind == 's') {
    d.rectangle(x - r, y - r, x + r, y + r, BLACK);
  } else if (kind == 'c') {
    d.ellipse(x - r - 1, y - r - 1, x + r + 1, y + r + 1, WHITE, BLACK, 2);
  } else if (kind == 't') {
    const Pt p[] = {{x, y - r - 1}, {x + r + 1, y + r}, {x - r - 1, y + r}};
    d.polygon(p, 3, BLACK);
  } else {
    const Pt p[] = {{x, y - r - 2}, {x + r + 2, y}, {x, y + r + 2}, {x - r - 2, y}};
    d.polygon(p, 4, WHITE, BLACK, 2);
  }
}

inline int marker_xs(double x0, double x1, int k, int n, double* out, int cap, double step = MARKER_STEP) {
  int count = 0;
  for (double x = x0 + step * (k + 0.5) / n; x < x1 - 4 && count < cap; x += step) out[count++] = x;
  return count;
}

inline double y_at(const Pt* pts, int n, double x) {
  if (x <= pts[0].x) return pts[0].y;
  for (int i = 0; i + 1 < n; ++i) {
    const Pt a = pts[i], b = pts[i + 1];
    if (a.x <= x && x <= b.x) return b.x == a.x ? a.y : a.y + (b.y - a.y) * (x - a.x) / (b.x - a.x);
  }
  return pts[n - 1].y;
}

inline void line_with_markers(View& d, const Pt* pts, int n, const LineStyle& s, int k, int count) {
  styled_line(d, pts, n, s);
  if (n == 0) return;
  double xs[32];
  const int m = marker_xs(pts[0].x, pts[n - 1].x, k, count, xs, 32);
  for (int i = 0; i < m; ++i) draw_marker(d, xs[i], y_at(pts, n, xs[i]), s.marker);
  draw_marker(d, pts[n - 1].x, pts[n - 1].y, s.marker);
}

inline void legend_sample(View& d, double x, double y, const LineStyle& s, int length = 24) {
  const Pt p[] = {{x, y}, {x + length, y}};
  styled_line(d, p, 2, s);
  draw_marker(d, x + length / 2.0, y, s.marker);
}

}  // namespace ink
```

`32` markers covers the widest chart (800 px / 56 px step ≈ 14).

- [ ] **Step 4: Implement `include/render/calendar.h`** (port of `draw/calendar.py`)

```cpp
#pragma once
// Month grid, Sunday first, today inverted (spec §3.3). Port of draw/calendar.py.
#include <stdio.h>

#include "civil.h"
#include "render/canvas.h"
#include "render/text.h"

namespace ink {

// calendar.Calendar(firstweekday=6).monthdayscalendar(y, m)
inline int month_weeks(int y, int m, int weeks[6][7]) {
  const int lead = (weekday(days_from_civil(y, m, 1)) + 1) % 7;
  const int n = days_in_month(y, m);
  const int rows = (lead + n + 6) / 7;
  for (int r = 0; r < rows; ++r)
    for (int c = 0; c < 7; ++c) {
      const int day = r * 7 + c - lead + 1;
      weeks[r][c] = day >= 1 && day <= n ? day : 0;
    }
  return rows;
}

inline int month_grid(View& d, double x, double y, double w, int32_t today, bool big = false) {
  const Ymd t = civil_from_days(today);
  int weeks[6][7];
  const int rows = month_weeks(t.y, t.m, weeks);
  const double cw = w / 7, ch = big ? 26 : 22;
  const Font& head = font(big ? 12 : 11, true);
  const Font& num = font(big ? 15 : 13);
  const Font& num_bold = font(big ? 15 : 13, true);
  const char* letters[] = {"S", "M", "T", "W", "T", "F", "S"};
  for (int i = 0; i < 7; ++i) draw_text(d, x + cw * i + cw / 2, y + ch / 2, letters[i], head, BLACK, "mm");
  d.line(x + 4, y + ch, x + w - 4, y + ch, BLACK);
  char s[4];
  for (int r = 0; r < rows; ++r)
    for (int c = 0; c < 7; ++c) {
      const int day = weeks[r][c];
      if (!day) continue;
      const double cx = x + cw * c + cw / 2, cy = y + ch * (r + 1) + ch / 2 + 2;
      snprintf(s, sizeof s, "%d", day);
      if (day == t.d) {
        d.rounded_rectangle(cx - cw / 2 + 3, cy - ch / 2 + 1, cx + cw / 2 - 3, cy + ch / 2 - 1, 4, BLACK);
        draw_text(d, cx, cy, s, num_bold, WHITE, "mm");
      } else {
        draw_text(d, cx, cy, s, num, BLACK, "mm");
      }
    }
  return static_cast<int>(ch * (rows + 1) + 4);
}

}  // namespace ink
```

- [ ] **Step 5: Run**

Run: `pio test -e native -f test_widgets`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add include/render/icons.h include/render/lines.h include/render/calendar.h test/test_widgets/test_main.cpp
git commit -m "Render: weather icons, chart line styles and the month grid, ported from the server

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

### Task 13: calendar_weather widget and the server reference renders

**Files:**
- Create: `tools/ref_render.py`, `include/widgets/calendar_weather.h`
- Create (generated, committed): `test/reference/widgets/*.pbm`
- Create (goldens, committed after review): `test/goldens/calendar_weather_{third,two_thirds,full}.png`
- Test: `test/test_widgets/test_main.cpp`

**Interfaces:**
- Consumes: Task 9 (`Weather`, `DayForecast`, fixtures), Task 12 (icons, calendar), Task 5 (text), Task 3 (`fmt_deg`, names), `Size` (Task 7).
- Produces (`namespace ink`):

```cpp
constexpr int WIDGET_H = 464, FOOTER_H = 16;
struct WeatherPayload { double temp; int16_t code; bool has_today; DayForecast today;
                        uint8_t n_upcoming; DayForecast upcoming[FORECAST_DAYS]; };
WeatherPayload build_weather_payload(const Weather& w, int32_t today);   // rows picked by date
bool weather_payload_ok(const WeatherPayload& p);                          // false -> "error: calendar_weather"
void render_calendar_weather(View& d, Size size, const WeatherPayload& p, int32_t today);
// test helper in test/test_widgets: render one widget into its own w×464 bitmap, compare with the server's PBM
```

- [ ] **Step 1: Write the reference renderer**

Create `tools/ref_render.py`:

```python
"""Reference renders of every golden scenario by the Python server, from test/fixtures
(plan Task 13). test/test_widgets compares the C++ output with these.

Needs the server package (removed in Task 20; the PBMs stay committed). To rerun after that,
check out tag server-render-final into a scratch worktree.
Run from the repo root:  (cd server && uv run python ../tools/ref_render.py)
"""
import json
import re
import sys
from dataclasses import replace
from datetime import date, datetime, timedelta, timezone
from pathlib import Path
from zoneinfo import ZoneInfo

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "server"))

from PIL import Image  # noqa: E402

from inkboard_server.compositor import FetchFailure, render_frame  # noqa: E402
from inkboard_server.frame import THRESHOLD, calibration_pattern  # noqa: E402
from inkboard_server.market_data import summarize  # noqa: E402
from inkboard_server.series import CATALOG, DEFAULT_SERIES  # noqa: E402
from inkboard_server.sources import openmeteo  # noqa: E402
from inkboard_server.sources.base import SourceResult  # noqa: E402
from inkboard_server.widgets.base import WIDGET_H, Box, RenderContext, Size, WidgetData  # noqa: E402
from inkboard_server.widgets.calendar_weather import CalendarWeather, build_payload  # noqa: E402
from inkboard_server.widgets.market_trends import MarketPayload, MarketTrends  # noqa: E402

FIX = ROOT / "test" / "fixtures"
OUT = ROOT / "test" / "reference" / "widgets"
LA = ZoneInfo("America/Los_Angeles")
EPOCH = date(1970, 1, 1)
SIZES = [(Size.THIRD, "third"), (Size.TWO_THIRDS, "two_thirds"), (Size.FULL, "full")]
WEATHER_OPTS = {"lat": 34.1, "lon": -118.2, "units": "imperial"}
VERSION = "fw 2.0.0"


def fixture(name: str) -> int:
    return int(re.search(rf"#define {name} (\d+)", (FIX / "fixtures.h").read_text()).group(1))


TODAY = EPOCH + timedelta(days=fixture("FIXTURE_TODAY"))
NOW = datetime.fromtimestamp(fixture("FIXTURE_NOW"), timezone.utc)
CTX = RenderContext(TODAY, LA)


def save(name: str, img: Image.Image) -> None:
    img.save(OUT / f"{name}.pbm")


def widget_image(w, data) -> Image.Image:
    img = Image.new("L", (w.size.width, WIDGET_H), 255)
    w.render(img, Box(0, 0, w.size.width, WIDGET_H), data, CTX)
    return img.point(lambda p: 255 if p > THRESHOLD else 0).convert("1")


def weather_data(stale=False) -> WidgetData:
    raw = openmeteo.parse_forecast(json.loads((FIX / "weather_la.json").read_text()))
    return WidgetData(build_payload(raw, TODAY), [SourceResult("weather", raw, NOW, stale)])


def fred_obs(fred_id: str, since: date | None = None) -> list[tuple[date, float]]:
    obs = json.loads((FIX / f"fred_{fred_id}_full.json").read_text())["observations"]
    out = [(date.fromisoformat(o["date"]), float(o["value"])) for o in obs if o["value"] not in (".", "")]
    return [x for x in out if since is None or x[0] >= since]


def market_data(series, years=5, stale=False, override=None) -> WidgetData:
    sums, srcs = [], []
    for sid in series:
        obs = (override or {}).get(sid) or fred_obs(CATALOG[sid].fred_id)
        sums.append(summarize(sid, obs, TODAY, years))
        srcs.append(SourceResult(f"fred:{sid}", obs, NOW, stale))
    return WidgetData(MarketPayload(tuple(sums), years, TODAY), srcs)


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    for size, name in SIZES:
        save(f"calendar_weather_{name}", widget_image(CalendarWeather(size, WEATHER_OPTS), weather_data()))
        save(f"market_trends_{name}", widget_image(MarketTrends(size, {"series": DEFAULT_SERIES, "years": 5}),
                                                  market_data(DEFAULT_SERIES)))
    short = market_data(("sp500", "home_la"), override={"home_la": fred_obs("MEDLISPRI31080", date(2024, 1, 1))})
    save("market_trends_short_series",
         widget_image(MarketTrends(Size.TWO_THIRDS, {"series": ("sp500", "home_la"), "years": 5}), short))
    ws = [MarketTrends(Size.TWO_THIRDS, {"series": DEFAULT_SERIES, "years": 5}),
          CalendarWeather(Size.THIRD, WEATHER_OPTS)]
    save("screen_default", render_frame(ws, [market_data(DEFAULT_SERIES), weather_data()], CTX, VERSION))
    save("screen_stale", render_frame(ws, [market_data(DEFAULT_SERIES, stale=True), weather_data(stale=True)],
                                      CTX, VERSION))
    save("screen_nodata", render_frame(ws, [FetchFailure("fred"), FetchFailure("weather")], CTX, VERSION))

    class Exploding(CalendarWeather):
        def render(self, img, box, data, ctx):
            raise RuntimeError("boom")

    save("screen_render_error", render_frame([ws[0], Exploding(Size.THIRD, WEATHER_OPTS)],
                                             [market_data(DEFAULT_SERIES), weather_data()], CTX, VERSION))
    save("calibration", calibration_pattern())
    print("wrote", sorted(p.name for p in OUT.glob("*.pbm")))


if __name__ == "__main__":
    main()
```

Run: `(cd server && uv run python ../tools/ref_render.py)`
Expected: `wrote [...]` listing 12 files.

- [ ] **Step 2: Write the failing tests**

Add to `test/test_widgets/test_main.cpp`:

```cpp
#include <string>
#include <vector>

#include "../fixtures/fixtures.h"
#include "json_stream.h"
#include "widgets/calendar_weather.h"

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
```

`RUN_TEST` both. Run → Expected: compile error.

- [ ] **Step 3: Implement `include/widgets/calendar_weather.h`** (port of `widgets/calendar_weather.py`)

```cpp
#pragma once
// Weather-first calendar: current conditions, forecast rows, date and month grid
// (spec §3.3). Port of server/inkboard_server/widgets/calendar_weather.py.
#include <math.h>
#include <stdio.h>

#include "civil.h"
#include "format.h"
#include "query.h"
#include "render/calendar.h"
#include "render/icons.h"
#include "render/text.h"
#include "sources/openmeteo.h"

namespace ink {

constexpr int WIDGET_H = 464, FOOTER_H = 16;

struct WeatherPayload {
  double temp = 0;
  int16_t code = 0;
  bool has_today = false;
  DayForecast today = {};
  uint8_t n_upcoming = 0;
  DayForecast upcoming[FORECAST_DAYS] = {};
};

// Pick days by date, not position: the cache may still hold yesterday's forecast.
inline WeatherPayload build_weather_payload(const Weather& w, int32_t today) {
  WeatherPayload p;
  p.temp = w.temp;
  p.code = w.code;
  for (int i = 0; i < w.n_daily; ++i) {
    if (w.daily[i].day == today && !p.has_today) {
      p.has_today = true;
      p.today = w.daily[i];
    } else if (w.daily[i].day > today) {
      p.upcoming[p.n_upcoming++] = w.daily[i];
    }
  }
  return p;
}

inline bool weather_payload_ok(const WeatherPayload& p) { return isfinite(p.temp); }

namespace cw_detail {

inline int now_block(View& d, double x, double y, const WeatherPayload& p) {
  const WmoInfo info = wmo_info(p.code);
  const int r = 34;
  draw_icon(d, info.icon, x + r + 4, y + r + 4, r);
  const double tx = x + 2 * r + 18;
  char s[48], a[16], b[16];
  fmt_deg(s, sizeof s, p.temp);
  draw_text(d, tx, y + 2, s, font(46, true), BLACK);
  draw_text(d, tx, y + 54, info.label, font(14), BLACK);
  if (p.has_today) {
    fmt_deg(a, sizeof a, p.today.hi);
    fmt_deg(b, sizeof b, p.today.lo);
    snprintf(s, sizeof s, "H %s  L %s", a, b);
  } else {
    snprintf(s, sizeof s, "H \xE2\x80\x94  L \xE2\x80\x94");
  }
  draw_text(d, tx, y + 72, s, font(14, true), BLACK);
  return 2 * r + 20;
}

inline int rows(View& d, double x, double y, double w, const DayForecast* days, int n) {
  const int rh = 30;
  char s[16];
  for (int k = 0; k < n; ++k) {
    const double yy = y + k * rh + rh / 2.0;
    draw_text(d, x + 4, yy, DAY_ABBR[weekday(days[k].day)], font(15, true), BLACK, "lm");
    draw_icon(d, wmo_info(days[k].code).icon, x + 70, yy, 12);
    fmt_deg(s, sizeof s, days[k].hi);
    draw_text(d, x + w - 50, yy, s, font(15, true), BLACK, "rm");
    fmt_deg(s, sizeof s, days[k].lo);
    draw_text(d, x + w - 4, yy, s, font(15), BLACK, "rm");
    if (k)
      for (int xx = static_cast<int>(x + 4); xx < static_cast<int>(x + w - 4); xx += 4) d.point(xx, yy - rh / 2.0, BLACK);
  }
  return n * rh;
}

}  // namespace cw_detail

inline void render_calendar_weather(View& d, Size size, const WeatherPayload& p, int32_t today) {
  using namespace cw_detail;
  const int pad = 14, bw = d.w(), bh = d.h();
  const Ymd t = civil_from_days(today);
  const int wd = weekday(today);
  char s[48];
  if (size == Size::Third) {
    const int iw = bw - 2 * pad;
    int y = pad;
    y += now_block(d, pad, y, p) + 4;
    y += rows(d, pad, y, iw, p.upcoming, p.n_upcoming < 5 ? p.n_upcoming : 5) + 8;
    d.line(pad, y, bw - pad, y, BLACK);
    y += 8;
    snprintf(s, sizeof s, "%s, %s %d", DAY_ABBR[wd], MONTH_NAME[t.m], t.d);
    draw_text(d, pad, y, s, font(20, true), BLACK);
    month_grid(d, pad, y + 30, iw, today);
  } else {
    const bool full = size == Size::Full;
    const int split = static_cast<int>(bw * (full ? 0.36 : 0.45));
    const int lx = pad, lw = split - 2 * pad;
    const int rx = split + pad, rw = bw - split - 2 * pad;
    d.line(split, pad, split, bh - pad, BLACK);
    int y = pad + 6;
    y += now_block(d, lx, y, p) + 14;
    const int max_rows = full ? 7 : 5;
    rows(d, lx, y, lw, p.upcoming, p.n_upcoming < max_rows ? p.n_upcoming : max_rows);
    y = pad + 6;
    draw_text(d, rx, y, DAY_NAME[wd], font(18, true), BLACK);
    snprintf(s, sizeof s, "%s %d, %d", full ? MONTH_NAME[t.m] : MONTH_ABBR[t.m], t.d, t.y);
    draw_text(d, rx, y + 24, s, font(26, true), BLACK);
    month_grid(d, rx, y + 70, rw, today, true);
  }
}

}  // namespace ink
```

- [ ] **Step 4: Run, compare, create goldens**

Run: `INKBOARD_UPDATE_GOLDENS=1 pio test -e native -f test_widgets`, then `pio test -e native -f test_widgets`.
Expected: PASS; the printed differing-pixel counts are ideally 0. If a count is non-zero, open `test/reference/widgets/<name>.pbm.cpp.png` next to the `.pbm` and fix the port before committing. Look at the three new goldens in `test/goldens/`.

- [ ] **Step 5: Commit**

```bash
git add tools/ref_render.py include/widgets/calendar_weather.h test/reference/widgets test/goldens/calendar_weather_*.png test/test_widgets/test_main.cpp
git commit -m "Widgets: calendar_weather ported, checked against the server's render of the same data

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

### Task 14: market_trends widget

**Files:**
- Create: `include/widgets/market_trends.h`
- Create (goldens): `test/goldens/market_trends_{third,two_thirds,full,short_series}.png`
- Test: `test/test_widgets/test_main.cpp`

**Interfaces:**
- Consumes: `Summary`, `summarize`, `y_range`, `log_ticks` (Task 10); `CATALOG`, `format_value` (Task 7); lines (Task 12); `WIDGET_H` (Task 13).
- Produces (`namespace ink`):

```cpp
struct MarketPayload { int n; const Summary* series[4]; int years; int32_t today; };
bool market_payload_ok(const MarketPayload& p);    // n >= 1 and every series has points
void render_market_trends(View& d, Size size, const MarketPayload& p);
```

- [ ] **Step 1: Write the failing tests**

Add to `test/test_widgets/test_main.cpp`:

```cpp
#include "sources/fred.h"
#include "widgets/market_trends.h"

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
```

`RUN_TEST` both. Run → Expected: compile error.

- [ ] **Step 2: Implement `include/widgets/market_trends.h`** (port of `widgets/market_trends.py`)

```cpp
#pragma once
// Market trends: several series on one log chart, each divided by its own window mean
// (spec §3.3). Port of server/inkboard_server/widgets/market_trends.py.
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "civil.h"
#include "format.h"
#include "market_data.h"
#include "query.h"
#include "render/lines.h"
#include "render/text.h"
#include "series.h"

namespace ink {

struct MarketPayload {
  int n;
  const Summary* series[4];
  int years;
  int32_t today;
};

inline bool market_payload_ok(const MarketPayload& p) {
  if (p.n < 1 || p.n > 4) return false;
  for (int i = 0; i < p.n; ++i)
    if (p.series[i] == nullptr || p.series[i]->n < 1) return false;
  return true;
}

namespace mt_detail {

inline Pt g_pts[MAX_POINTS + 1];  // one series at a time; static so the board's stack stays small

inline void chart(View& d, const MarketPayload& p, double left, double top, double right, double bottom, bool narrow) {
  double lo, hi;
  y_range(p.series, p.n, lo, hi);
  const double llo = log(lo), lhi = log(hi);
  int32_t start = p.series[0]->dates[0];
  for (int k = 1; k < p.n; ++k)
    if (p.series[k]->dates[0] < start) start = p.series[k]->dates[0];
  const int32_t span = p.today - start > 1 ? p.today - start : 1;
  auto X = [&](int32_t day) { return left + static_cast<double>(day - start) / span * (right - left); };
  auto Y = [&](double v) { return bottom - (log(v) - llo) / (lhi - llo) * (bottom - top); };
  const Font& axis = font(11);
  double ticks[6];
  char s[16];
  const int nt = log_ticks(lo, hi, ticks);
  for (int i = 0; i < nt; ++i) {
    const double y = Y(ticks[i]);
    if (ticks[i] == 1) d.line(left, y, right, y, BLACK, 2);
    else
      for (int x = static_cast<int>(left); x < static_cast<int>(right); x += 6) d.point(x, y, BLACK);
    snprintf(s, sizeof s, "%g\xC3\x97", ticks[i]);
    draw_text(d, left - 4, y, s, axis, BLACK, "rm");
  }
  d.line(left, bottom, right, bottom, BLACK);
  const int y0 = civil_from_days(start).y, y1 = civil_from_days(p.today).y;
  for (int yr = y0 + 1; yr <= y1; ++yr) {
    const double x = X(days_from_civil(yr, 1, 1));
    d.line(x, bottom, x, bottom + 4, BLACK);
    if (narrow) snprintf(s, sizeof s, "'%02d", yr % 100);
    else snprintf(s, sizeof s, "%d", yr);
    draw_text(d, x, bottom + 6, s, axis, BLACK, "mt");
  }
  for (int k = 0; k < p.n; ++k) {
    const Summary& sm = *p.series[k];
    for (int i = 0; i < sm.n; ++i) g_pts[i] = Pt{X(sm.dates[i]), Y(sm.norm[i])};
    line_with_markers(d, g_pts, sm.n, STYLES[k], k, p.n);
  }
}

inline void table(View& d, const MarketPayload& p, double x0, double ty, double x1, int row_h, bool narrow) {
  const Font& value_font = font(narrow ? 12 : 13);
  const Font& name_font = font(narrow ? 12 : 13, true);
  const Font& head_font = font(11);
  const double cols3[] = {x1 - 150, x1 - 70, x1}, cols2[] = {x1 - 60, x1};
  const double* cols = narrow ? cols2 : cols3;
  const int ncol = narrow ? 2 : 3;
  const char* heads[] = {"now", "\xC3\x97" "avg", "1 yr"};
  for (int i = 0; i < ncol; ++i) draw_text(d, cols[i], ty - 4, heads[i], head_font, BLACK, "rb");
  d.line(x0, ty, x1, ty, BLACK);
  char name[64], v[3][24];
  for (int k = 0; k < p.n; ++k) {
    const Summary& s = *p.series[k];
    const SeriesDef& sd = CATALOG[s.series];
    const double y = ty + 12 + k * row_h;
    legend_sample(d, x0, y, STYLES[k]);
    snprintf(name, sizeof name, "%s", narrow ? sd.short_label : sd.label);
    if (s.short_history && !narrow)
      snprintf(name + strlen(name), sizeof name - strlen(name), " (since %d)", civil_from_days(s.window_start).y);
    draw_text(d, x0 + 32, y, name, name_font, BLACK, "lm");
    format_value(sd.fmt, s.last, v[0], sizeof v[0]);
    snprintf(v[1], sizeof v[1], "%.2f\xC3\x97", s.ratio);
    if (s.has_yoy) snprintf(v[2], sizeof v[2], "%+.0f%%", s.yoy * 100);
    else snprintf(v[2], sizeof v[2], "\xE2\x80\x94");
    for (int i = 0; i < ncol; ++i) draw_text(d, cols[i], y, v[i], value_font, BLACK, "rm");
  }
}

}  // namespace mt_detail

inline void render_market_trends(View& d, Size size, const MarketPayload& p) {
  const bool narrow = size == Size::Third;
  const int pad = 12, bw = d.w(), bh = d.h();
  draw_text(d, pad, pad, "Markets", font(narrow ? 18 : 22, true), BLACK);
  char s[64];
  if (narrow) snprintf(s, sizeof s, "%d-yr \xC2\xB7 \xC3\x97" "avg \xC2\xB7 log", p.years);
  else snprintf(s, sizeof s, "%d-yr, \xC3\x97 own average, log", p.years);
  draw_text(d, pad, pad + (narrow ? 23 : 28), s, font(12), BLACK);
  int32_t asof = p.series[0]->last_date;
  for (int k = 1; k < p.n; ++k)
    if (p.series[k]->last_date < asof) asof = p.series[k]->last_date;
  const Ymd a = civil_from_days(asof);
  snprintf(s, sizeof s, "as of %s %d", MONTH_ABBR[a.m], a.d);
  draw_text(d, bw - pad, pad + 4, s, font(11), BLACK, "ra");
  const int row_h = narrow ? 20 : 22;
  const int table_h = p.n * row_h + 36;
  const int top = pad + 50, bottom = bh - pad - 18 - table_h;
  mt_detail::chart(d, p, pad + 30, top, bw - pad - 6, bottom, narrow);
  mt_detail::table(d, p, pad, bottom + 40, bw - pad, row_h, narrow);
}

}  // namespace ink
```

`"\xC3\x97" "avg"` is split into two literals on purpose: `\xC3\x97avg` would be read as one long hex escape.

- [ ] **Step 3: Run, compare, create goldens**

Run: `INKBOARD_UPDATE_GOLDENS=1 pio test -e native -f test_widgets`, then `pio test -e native -f test_widgets`.
Expected: PASS, differing-pixel counts printed (ideally 0; investigate any non-zero count with the `.pbm.cpp.png`). Look at the four new goldens.

- [ ] **Step 4: Commit**

```bash
git add include/widgets/market_trends.h test/goldens/market_trends_*.png test/test_widgets/test_main.cpp
git commit -m "Widgets: market_trends ported, checked against the server's render of the same data

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

### Task 15: Compositor, model, error and calibration screens

**Files:**
- Create: `include/compositor.h`, `include/model.h`
- Create (goldens): `test/goldens/screen_{default,stale,nodata,render_error,auth_rejected,config_error}.png`, `test/goldens/calibration.png`
- Test: `test/test_widgets/test_main.cpp`

**Interfaces:**
- Consumes: everything in Phase 2–3; `WeatherCache`, `SeriesCache`, `is_stale`, `weather_usable` (Task 11).
- Produces (`namespace ink`):

```cpp
// compositor.h
enum class ColumnState : uint8_t { Ok, NoData, AuthRejected, RenderError };
struct ColumnData {
  ColumnState state; const char* nodata_source;      // "fred", "weather", "market_trends"
  const WeatherPayload* weather; const MarketPayload* market; int32_t today;
  bool has_fetched_at; int64_t fetched_at; bool stale; // newest successful fetch among its sources
};
void compose(Bitmap& frame, const Layout& l, const ColumnData* cols, const tz::Rule& zone, const char* version);
void render_config_error(Bitmap& frame, const char* message, const char* query);
void render_calibration(Bitmap& frame);
// model.h
struct Model {
  Layout layout;
  int64_t now;                 // one clock read, after the network step
  WeatherCache weather;
  SeriesCache series[4];       // series[i] belongs to layout.series[i]
};
WeatherKey weather_key(const Layout& l);
void build_frame(Bitmap& frame, const Model& m, const char* version);
```

- [ ] **Step 1: Write the failing tests**

Add to `test/test_widgets/test_main.cpp`:

```cpp
#include "model.h"

static ink::Model g_model;
static uint8_t g_frame[ink::FRAME_BYTES];

static void default_model(bool stale) {
  g_model = ink::Model{};
  char e[192];
  TEST_ASSERT_TRUE(ink::parse_query("w=market_trends:2/3,calendar_weather:1/3&lat=34.1&lon=-118.2"
                                    "&tz=America/Los_Angeles&units=imperial", g_model.layout, e, sizeof e));
  g_model.now = FIXTURE_NOW + (stale ? 7 * 3600 : 0);
  g_model.weather.valid = true;
  g_model.weather.key = ink::weather_key(g_model.layout);
  g_model.weather.data = fixture_weather();
  g_model.weather.status.fetched_at = FIXTURE_NOW;
  g_model.weather.status.last_attempt_failed = stale;
  const char* ids[] = {"SP500", "CBBTCUSD", "MORTGAGE30US", "MEDLISPRI31080"};
  for (int i = 0; i < 4; ++i) {
    load_series(i, ids[i]);
    ink::SeriesCache& c = g_model.series[i];
    c.valid = true;
    snprintf(c.fred_id, sizeof c.fred_id, "%s", ids[i]);
    c.data = g_series[i];
    c.status.fetched_at = FIXTURE_NOW;
    c.status.last_attempt_failed = stale;
  }
}

static void check_screen(const char* name, bool has_reference) {
  ink::Bitmap bm(g_frame, ink::FRAME_W, ink::FRAME_H);
  ink::build_frame(bm, g_model, "fw 2.0.0");
  if (has_reference) check_reference(name, g_frame, ink::FRAME_W, ink::FRAME_H);
  else ink_test::golden(name, g_frame, ink::FRAME_W, ink::FRAME_H);
}

void test_screen_default() { default_model(false); check_screen("screen_default", true); }
void test_screen_stale() { default_model(true); check_screen("screen_stale", true); }

void test_screen_nodata() {
  default_model(false);
  g_model.weather = ink::WeatherCache{};
  for (auto& s : g_model.series) s = ink::SeriesCache{};
  check_screen("screen_nodata", true);
}

void test_screen_render_error() {
  default_model(false);
  g_model.weather.data.temp = NAN;  // fails weather_payload_ok -> "error: calendar_weather"
  check_screen("screen_render_error", true);
}

void test_screen_auth_rejected() {
  default_model(false);
  for (auto& s : g_model.series) {
    s = ink::SeriesCache{};
    s.status.auth_rejected = true;
    s.status.last_attempt_failed = true;
  }
  check_screen("screen_auth_rejected", false);
}

void test_weather_from_other_location_not_shown() {
  default_model(false);
  g_model.weather.key.lat = 40.7;  // cache from before a reflash with another location
  ink::Bitmap bm(g_frame, ink::FRAME_W, ink::FRAME_H);
  ink::build_frame(bm, g_model, "fw 2.0.0");
  static uint8_t nodata[ink::FRAME_BYTES];
  g_model.weather = ink::WeatherCache{};
  ink::Bitmap bm2(nodata, ink::FRAME_W, ink::FRAME_H);
  ink::build_frame(bm2, g_model, "fw 2.0.0");
  TEST_ASSERT_EQUAL_MEMORY(nodata, g_frame, ink::FRAME_BYTES);
}

void test_config_error_and_calibration() {
  ink::Bitmap bm(g_frame, ink::FRAME_W, ink::FRAME_H);
  ink::render_config_error(bm, "w: sizes add up to 2/3, need 3/3", "w=market_trends:2/3&tz=UTC");
  ink_test::golden("screen_config_error", g_frame, ink::FRAME_W, ink::FRAME_H);
  ink::render_calibration(bm);
  // frame.py drew this pattern without fontmode "1" (anti-aliased, then thresholded), so its
  // text differs slightly from ours; the shapes are what bring-up checks. Golden only.
  ink_test::golden("calibration", g_frame, ink::FRAME_W, ink::FRAME_H);
}
```

`RUN_TEST` all seven. Run → Expected: compile error.

- [ ] **Step 2: Implement `include/compositor.h`** (port of `compositor.py` and `frame.py` `calibration_pattern`)

```cpp
#pragma once
// Columns, dividers, footer and per-column failure messages (spec §3.3), plus the config
// error screen and the calibration pattern. Port of compositor.py and frame.py.
#include <stdio.h>
#include <string.h>

#include "format.h"
#include "query.h"
#include "render/canvas.h"
#include "render/text.h"
#include "series.h"
#include "tz.h"
#include "widgets/calendar_weather.h"
#include "widgets/market_trends.h"

namespace ink {

enum class ColumnState : uint8_t { Ok, NoData, AuthRejected, RenderError };

struct ColumnData {
  ColumnState state = ColumnState::NoData;
  const char* nodata_source = "";
  const WeatherPayload* weather = nullptr;
  const MarketPayload* market = nullptr;
  int32_t today = 0;
  bool has_fetched_at = false;
  int64_t fetched_at = 0;
  bool stale = false;
};

namespace comp_detail {

inline void message(View& full, const Box& box, const char* text, double dy = 0) {
  draw_text(full, box.x + box.w / 2.0, box.y + box.h / 2.0 + dy, text, font(14, true), BLACK, "mm");
}

inline void add_unique(const char** list, int& n, const char* a) {
  for (int i = 0; i < n; ++i)
    if (strcmp(list[i], a) == 0) return;
  list[n++] = a;
}

}  // namespace comp_detail

inline void compose(Bitmap& frame, const Layout& l, const ColumnData* cols, const tz::Rule& zone, const char* version) {
  using namespace comp_detail;
  frame.fill(WHITE);
  View full(frame, Box{0, 0, FRAME_W, FRAME_H});
  int x = 0, dividers[3], nd = 0;
  char text[64];
  for (int i = 0; i < l.n_columns; ++i) {
    const Box box{x, 0, size_width(l.columns[i].size), WIDGET_H};
    const ColumnData& c = cols[i];
    if (c.state == ColumnState::NoData) {
      snprintf(text, sizeof text, "No data yet: %s", c.nodata_source);
      message(full, box, text);
    } else if (c.state == ColumnState::AuthRejected) {
      message(full, box, "FRED key rejected:", -10);
      message(full, box, "check secrets.h", 10);
    } else if (c.state == ColumnState::RenderError) {
      snprintf(text, sizeof text, "error: %s", widget_name(l.columns[i].type));
      message(full, box, text);
    } else {
      View v(frame, box);
      if (l.columns[i].type == WidgetType::CalendarWeather)
        render_calendar_weather(v, l.columns[i].size, *c.weather, c.today);
      else
        render_market_trends(v, l.columns[i].size, *c.market);
    }
    if (x) dividers[nd++] = x;
    x += box.w;
  }
  for (int i = 0; i < nd; ++i) full.line(dividers[i], 8, dividers[i], WIDGET_H - 8, BLACK);

  // Footer (compositor.py _footer)
  full.line(0, WIDGET_H, FRAME_W, WIDGET_H, BLACK);
  bool any_time = false, stale = false;
  int64_t newest = 0;
  for (int i = 0; i < l.n_columns; ++i) {
    // A column whose render failed still fetched its data (compositor.py counts it too).
    if (cols[i].state != ColumnState::Ok && cols[i].state != ColumnState::RenderError) continue;
    stale = stale || cols[i].stale;
    if (cols[i].has_fetched_at && (!any_time || cols[i].fetched_at > newest)) newest = cols[i].fetched_at;
    any_time = any_time || cols[i].has_fetched_at;
  }
  char left[128] = "updated \xE2\x80\x94";
  if (any_time) {
    const tz::Local t = tz::to_local(zone, newest);
    char clock[16];
    fmt_clock(clock, sizeof clock, t.hh, t.mm);
    snprintf(left, sizeof left, "updated %s", clock);
  }
  if (stale) strncat(left, "   \xE2\x9A\xA0 stale", sizeof left - strlen(left) - 1);
  if (version && *version) {
    strncat(left, "   ", sizeof left - strlen(left) - 1);
    strncat(left, version, sizeof left - strlen(left) - 1);
  }
  const int y = WIDGET_H + FOOTER_H / 2;
  draw_text(full, 8, y, left, font(10), BLACK, "lm");
  const char* attrs[16];
  int na = 0;
  for (int i = 0; i < l.n_columns; ++i) {
    if (l.columns[i].type == WidgetType::MarketTrends) {
      for (int k = 0; k < l.n_series; ++k)
        for (const char* a : CATALOG[l.series[k]].attribution)
          if (a) add_unique(attrs, na, a);
    } else {
      add_unique(attrs, na, "Open-Meteo");
    }
  }
  char right[160] = "";
  for (int i = 0; i < na; ++i) {
    if (i) strncat(right, " \xC2\xB7 ", sizeof right - strlen(right) - 1);
    strncat(right, attrs[i], sizeof right - strlen(right) - 1);
  }
  draw_text(full, FRAME_W - 8, y, right, font(10), BLACK, "rm");
}
```

```cpp
// Word-wrapped text block; returns the y after the last line.
inline int wrapped(View& d, int x, int y, int width, const char* text, const Font& f, int line_h) {
  char line[256] = "";
  const char* p = text;
  while (*p) {
    const char* sp = strchr(p, ' ');
    const size_t n = sp ? static_cast<size_t>(sp - p) : strlen(p);
    char trial[256];
    snprintf(trial, sizeof trial, "%s%s%.*s", line, line[0] ? " " : "", static_cast<int>(n), p);
    if (line[0] && text_length(f, trial) > width) {
      draw_text(d, x, y, line, f, BLACK);
      y += line_h;
      snprintf(line, sizeof line, "%.*s", static_cast<int>(n), p);
    } else {
      snprintf(line, sizeof line, "%s", trial);
    }
    p += n;
    while (*p == ' ') ++p;
  }
  if (line[0]) {
    draw_text(d, x, y, line, f, BLACK);
    y += line_h;
  }
  return y;
}

inline void render_config_error(Bitmap& frame, const char* message, const char* query) {
  frame.fill(WHITE);
  View d(frame, Box{0, 0, FRAME_W, FRAME_H});
  draw_text(d, 24, 36, "inkboard config error", font(22, true), BLACK);
  wrapped(d, 24, 90, FRAME_W - 48, message, font(15), 22);
  draw_text(d, 24, 360, "FRAME_QUERY in include/config.h:", font(13, true), BLACK);
  // Queries have no spaces: wrap by characters at 100 per line.
  char chunk[101];
  int y = 384;
  for (size_t off = 0; off < strlen(query) && y < FRAME_H - 16; off += 100, y += 18) {
    snprintf(chunk, sizeof chunk, "%.100s", query + off);
    draw_text(d, 24, y, chunk, font(12), BLACK);
  }
}

// frame.py calibration_pattern(): asymmetric marks so rotation, mirroring and inversion show.
inline void render_calibration(Bitmap& frame) {
  frame.fill(WHITE);
  View d(frame, Box{0, 0, FRAME_W, FRAME_H});
  d.rectangle(0, 0, 39, 39, BLACK);
  const Pt tr[] = {{FRAME_W - 40.0, 1}, {FRAME_W - 2.0, 1}, {FRAME_W - 2.0, 39}};
  d.line(tr, 3, BLACK, 4);
  const Pt bl[] = {{1, FRAME_H - 40.0}, {1, FRAME_H - 2.0}, {39, FRAME_H - 2.0}};
  d.line(bl, 3, BLACK, 4);
  d.ellipse(FRAME_W - 40, FRAME_H - 40, FRAME_W - 1, FRAME_H - 1, BLACK);
  draw_text(d, 52, 10, "TOP LEFT (solid square)", font(20, true), BLACK);
  draw_text(d, FRAME_W - 52, FRAME_H - 10, "BOTTOM RIGHT (dot)", font(20, true), BLACK, "rb");
  draw_text(d, FRAME_W / 2, 110, "inkboard calibration", font(36, true), BLACK, "mm");
  draw_text(d, FRAME_W / 2, 155, "Black text on white means the invert setting is right.", font(18), BLACK, "mm");
  for (int y = 200; y < 400; y += 8)
    for (int x = 200; x < 600; x += 8)
      if (((x - 200) / 8 + (y - 200) / 8) % 2 == 0) d.rectangle(x, y, x + 7, y + 7, BLACK);
}

}  // namespace ink
```

- [ ] **Step 3: Implement `include/model.h`**

```cpp
#pragma once
// Everything one render needs, and the step from cached data to a frame (spec §4.2 step 6):
// the per-column states of compositor.py's fetch_widgets, computed from the caches.
#include <stdio.h>

#include "compositor.h"
#include "data_cache.h"
#include "market_data.h"
#include "query.h"
#include "tz.h"

namespace ink {

struct Model {
  Layout layout;
  int64_t now = 0;
  WeatherCache weather;
  SeriesCache series[4];
};

inline WeatherKey weather_key(const Layout& l) {
  WeatherKey k{l.lat, l.lon, l.metric, ""};
  snprintf(k.tz, sizeof k.tz, "%s", l.tz);
  return k;
}

namespace model_detail {
inline Summary g_summaries[4];   // ~6 KB each: static, never on the board's stack
inline WeatherPayload g_weather;
inline MarketPayload g_market;
}  // namespace model_detail

inline void build_frame(Bitmap& frame, const Model& m, const char* version) {
  using namespace model_detail;
  const Layout& l = m.layout;
  const int32_t today = tz::to_local(l.zone, m.now).day;

  ColumnData weather_col, market_col;
  if (l.has_weather) {
    if (!weather_usable(m.weather, weather_key(l))) {
      weather_col.state = ColumnState::NoData;
      weather_col.nodata_source = "weather";
    } else {
      g_weather = build_weather_payload(m.weather.data, today);
      weather_col.state = weather_payload_ok(g_weather) ? ColumnState::Ok : ColumnState::RenderError;
      weather_col.weather = &g_weather;
      weather_col.today = today;
      weather_col.has_fetched_at = true;
      weather_col.fetched_at = m.weather.status.fetched_at;
      weather_col.stale = is_stale(m.weather.status, m.now, WEATHER_TTL_S);
    }
  }
  if (l.has_market) {
    market_col.state = ColumnState::Ok;
    g_market = MarketPayload{l.n_series, {}, l.years, today};
    for (int i = 0; i < l.n_series && market_col.state == ColumnState::Ok; ++i) {
      const SeriesCache& c = m.series[i];
      if (!c.valid || c.data.latest_date == INT32_MIN) {
        market_col.state = c.status.auth_rejected ? ColumnState::AuthRejected : ColumnState::NoData;
        market_col.nodata_source = "fred";
      } else if (!summarize(l.series[i], c.data, today, l.years, g_summaries[i])) {
        market_col.state = ColumnState::NoData;      // summarize() raised in Python: FetchFailure(type_name)
        market_col.nodata_source = "market_trends";
      } else {
        g_market.series[i] = &g_summaries[i];
        if (!market_col.has_fetched_at || c.status.fetched_at > market_col.fetched_at)
          market_col.fetched_at = c.status.fetched_at;
        market_col.has_fetched_at = true;
        market_col.stale = market_col.stale || is_stale(c.status, m.now, FRED_TTL_S);
      }
    }
    if (market_col.state == ColumnState::Ok) {
      market_col.market = &g_market;
      if (!market_payload_ok(g_market)) market_col.state = ColumnState::RenderError;
    }
  }
  ColumnData cols[3];
  for (int i = 0; i < l.n_columns; ++i)
    cols[i] = l.columns[i].type == WidgetType::CalendarWeather ? weather_col : market_col;
  compose(frame, l, cols, l.zone, version);
}

}  // namespace ink
```

- [ ] **Step 4: Run, compare, create goldens**

Run: `INKBOARD_UPDATE_GOLDENS=1 pio test -e native -f test_widgets`, then `pio test -e native -f test_widgets`.
Expected: PASS. `screen_default`, `screen_stale`, `screen_nodata` and `screen_render_error` print their differing-pixel counts against the server (ideally 0). Look at all new goldens, especially `screen_auth_rejected` and `screen_config_error` (no server equivalent).

- [ ] **Step 5: Commit**

```bash
git add include/compositor.h include/model.h test/goldens/screen_*.png test/test_widgets/test_main.cpp
git commit -m "Render: compositor, footer, config error and calibration screens

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

### Task 16: `just preview`

**Files:**
- Create: `tools/preview/main.cpp`
- Modify: `platformio.ini` (add `[env:preview]`), `justfile` (add `preview`), `.gitignore` (`.pio/` is already ignored; nothing to add)

**Interfaces:**
- Consumes: `parse_query`, `Model`, `build_frame`, `openmeteo_path`, `OpenMeteoParser`, `fred_path`, `FredParser`, `SundayResampler`, `plan_fred`, `png_encode`; `FRAME_QUERY`, `INKBOARD_VERSION` from `include/config.h`.
- Produces: `.pio/build/preview/program [--query Q] [--fixtures] [--out PATH]`; `just preview ["<query>"] [flags]`.

- [ ] **Step 1: Add the environment and recipe**

`platformio.ini`, after `[env:native]`:

```ini
; Host preview of a layout (spec §3.4): just preview ["<query>"]
[env:preview]
platform = native
build_src_filter = -<*> +<../tools/preview/>
build_flags = -std=gnu++17 -Wall -Wextra -DINK_TEST_DIR=\"$PROJECT_DIR/test\"
```

(Use the same `INK_TEST_DIR` form that Task 2 settled on.)

`justfile`, after `test`:

```just
# Render a layout to .pio/preview.png on this machine (default: FRAME_QUERY in include/config.h).
# Live data needs FRED_API_KEY (env or include/secrets.h); --fixtures renders offline.
preview QUERY="" *FLAGS:
    pio run -s -e preview
    .pio/build/preview/program {{ if QUERY == "" { "" } else { "--query '" + QUERY + "'" } }} {{FLAGS}}
```

- [ ] **Step 2: Write the program**

Create `tools/preview/main.cpp`:

```cpp
// Host preview (spec §3.4): renders FRAME_QUERY (or --query) to a PNG with the board's own
// parsers and renderer. Live data comes through curl; --fixtures uses test/fixtures.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <fstream>
#include <regex>
#include <sstream>
#include <string>

#include "config.h"
#include "data_cache.h"
#include "model.h"
#include "render/png.h"
#include "sources/fred.h"
#include "sources/openmeteo.h"
#include "../../test/fixtures/fixtures.h"

static ink::Model g_model;
static uint8_t g_frame[ink::FRAME_BYTES];
static ink::SeriesData g_scratch;

static std::string slurp(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  std::stringstream s;
  s << f.rdbuf();
  return s.str();
}

// Streams `url` (or a fixture file) into a JSON handler; true if the document was complete.
static bool fetch(const std::string& url, const std::string& fixture, ink::json::Handler& h) {
  ink::json::Parser p(h);
  if (!fixture.empty()) {
    const std::string doc = slurp(fixture);
    p.feed(doc.data(), doc.size());
    return !doc.empty() && p.finish();
  }
  const std::string cmd = "curl -sS --fail-with-body --max-time 60 '" + url + "'";
  FILE* f = popen(cmd.c_str(), "r");
  if (!f) return false;
  char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) p.feed(buf, n);
  return pclose(f) == 0 && p.finish();
}

static std::string fred_key() {
  if (const char* k = getenv("FRED_API_KEY")) return k;
  std::smatch m;
  const std::string s = slurp(std::string(INK_TEST_DIR) + "/../include/secrets.h");
  if (std::regex_search(s, m, std::regex("#define\\s+FRED_API_KEY\\s+\"([^\"]+)\""))) return m[1];
  return "";
}

int main(int argc, char** argv) {
  std::string query = FRAME_QUERY, out = std::string(INK_TEST_DIR) + "/../.pio/preview.png";
  bool fixtures = false;
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--query") && i + 1 < argc) query = argv[++i];
    else if (!strcmp(argv[i], "--fixtures")) fixtures = true;
    else if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
    else {
      fprintf(stderr, "usage: %s [--query Q] [--fixtures] [--out PATH]\n", argv[0]);
      return 2;
    }
  }
  ink::Bitmap frame(g_frame, ink::FRAME_W, ink::FRAME_H);
  char error[192];
  if (!ink::parse_query(query.c_str(), g_model.layout, error, sizeof error)) {
    fprintf(stderr, "config error: %s\n", error);
    ink::render_config_error(frame, error, query.c_str());
  } else {
    const ink::Layout& l = g_model.layout;
    g_model.now = fixtures ? FIXTURE_NOW : static_cast<int64_t>(time(nullptr));
    const int32_t today = ink::tz::to_local(l.zone, g_model.now).day;
    const std::string fix = std::string(INK_TEST_DIR) + "/fixtures/";
    if (l.has_weather) {
      char path[400];
      ink::openmeteo_path(path, sizeof path, l.lat, l.lon, l.metric, l.tz);
      ink::OpenMeteoParser h;
      ink::Weather w;
      if (fetch(std::string("https://") + ink::OPENMETEO_HOST + path, fixtures ? fix + "weather_la.json" : "", h) &&
          h.result(w)) {
        g_model.weather = ink::WeatherCache{true, ink::weather_key(l), w, {}};
        g_model.weather.status.fetched_at = g_model.now;
      } else {
        fprintf(stderr, "weather: fetch failed\n");
      }
    }
    const std::string key = fixtures ? "" : fred_key();
    if (l.has_market && !fixtures && key.empty()) fprintf(stderr, "FRED_API_KEY not set: market data skipped\n");
    for (int i = 0; l.has_market && i < l.n_series && (fixtures || !key.empty()); ++i) {
      const ink::SeriesDef& sd = ink::CATALOG[l.series[i]];
      const ink::FredPlan plan = ink::plan_fred(ink::SeriesCache{}, g_model.now, today, l.years);
      char path[400];
      ink::fred_path(path, sizeof path, sd.fred_id, key.c_str(), plan.observation_start);
      ink::SundayResampler r;
      r.begin_full(g_scratch, plan.keep_from, today);
      ink::FredParser fp(r);
      const bool ok = fetch(std::string("https://") + ink::FRED_HOST + path,
                            fixtures ? fix + "fred_" + sd.fred_id + "_full.json" : "", fp) &&
                      fp.saw_observations() && r.finish(plan.keep_from);
      if (!ok) {
        fprintf(stderr, "%s: fetch failed%s\n", sd.fred_id, fp.api_key_error() ? " (API key rejected)" : "");
        g_model.series[i].status.auth_rejected = fp.api_key_error();
        continue;
      }
      ink::SeriesCache& c = g_model.series[i];
      c.valid = true;
      snprintf(c.fred_id, sizeof c.fred_id, "%s", sd.fred_id);
      c.data = g_scratch;
      c.status.fetched_at = g_model.now;
    }
    ink::build_frame(frame, g_model, "fw " INKBOARD_VERSION " preview");
  }
  const std::vector<uint8_t> png = ink::png_encode(g_frame, ink::FRAME_W, ink::FRAME_H);
  FILE* f = fopen(out.c_str(), "wb");
  if (!f || fwrite(png.data(), 1, png.size(), f) != png.size()) {
    fprintf(stderr, "cannot write %s\n", out.c_str());
    return 1;
  }
  fclose(f);
  printf("%s\n", out.c_str());
  return 0;
}
```

- [ ] **Step 3: Run it**

Run: `just preview "" --fixtures`
Expected: prints `.../.pio/preview.png`; the image looks like `test/goldens/screen_default.png` except for the footer version text.
Run: `just preview` (live; needs the FRED key)
Expected: today's dashboard. Run `just preview "w=market_trends:1&years=2"` and `just preview "w=bad"` (config error screen, and `config error: w: expected type:size, got 'bad'` on stderr).

- [ ] **Step 4: Commit**

```bash
git add tools/preview/main.cpp platformio.ini justfile
git commit -m "just preview: render a layout on this machine with the board's own code

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

## Phase 4: Firmware

### Task 17: The wake cycle (host-tested)

**Files:**
- Rewrite: `include/wake_logic.h`, `include/cycle.h`
- Modify: `include/http_time.h` (use `civil.h`; delete `format_clock` and `badge_text`)
- Rewrite: `test/test_logic/test_main.cpp`

**Interfaces:**
- Consumes: everything from Phases 2–3.
- Produces:

```cpp
// wake_logic.h (namespace wake)
constexpr uint32_t RTC_MAGIC = 0x1B0A4D03;      // new layout: old RTC contents read as a cold boot
constexpr int32_t MIN_SLEEP_S = 300, MAX_SLEEP_S = 21600;
struct Rtc { uint32_t magic; bool clock_valid; bool panel_dirty; uint32_t shown_crc; uint8_t fail_count; };
void rtc_begin(Rtc& r, bool clock_survived_reset);
int32_t parse_seconds(const char* s);           // unchanged
int32_t clamp_sleep(int32_t s);                 // unchanged
int32_t backoff_seconds(uint8_t fail_count, int32_t retry_after_s);   // unchanged
enum class Idle : uint8_t { Wait, RunNow, DeepSleep };
Idle idle_step(bool computer_attached, int32_t ms_left);              // unchanged
int32_t remaining_sleep_s(int32_t ms_left);                           // unchanged
// cycle.h (namespace wake)
constexpr uint32_t NETWORK_BUDGET_MS = 45000, WIFI_TIMEOUT_MS = 15000, SNTP_TIMEOUT_MS = 5000, FRED_MIN_LEFT_MS = 8000;
struct Work { uint8_t frame[ink::FRAME_BYTES]; ink::Model model; ink::SeriesData scratch;
              uint8_t file[sizeof(ink::SeriesCache) + ink::CACHE_HEADER_BYTES]; };
template <class Ops> bool show_if_changed(Ops& ops, Rtc& rtc, const uint8_t* frame);
template <class Ops> int32_t run_cycle(Ops& ops, Rtc& rtc, Work& w, const char* frame_query, const char* version,
                                       bool calibration, int32_t fallback_s);
// Ops: store_begin, load, save, prune, wifi_up, wifi_off, sntp, set_clock, now, ms, get, close_connections,
//      show, fred_api_key, log (signatures in the FakeOps below; src/main.cpp mirrors them)
```

- [ ] **Step 1: Rewrite `include/wake_logic.h`**

```cpp
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
```

- [ ] **Step 2: Trim `include/http_time.h`**

Replace its body with:

```cpp
#pragma once
// HTTP Date parsing: the clock fallback when SNTP fails (spec §2.1). Pure; host-tested.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "civil.h"

namespace httptime {

// IMF-fixdate, e.g. "Sun, 27 Sep 2026 19:39:05 GMT" -> unix seconds; -1 if invalid.
inline int64_t parse_http_date(const char* s) {
  if (s == nullptr) return -1;
  int d, y, hh, mm, ss;
  char mon[4] = {0};
  if (sscanf(s, "%*3s, %d %3s %d %d:%d:%d GMT", &d, mon, &y, &hh, &mm, &ss) != 6) return -1;
  static const char* kMonths = "JanFebMarAprMayJunJulAugSepOctNovDec";
  const char* at = strstr(kMonths, mon);
  if (at == nullptr || strlen(mon) != 3 || (at - kMonths) % 3 != 0) return -1;
  const int m = static_cast<int>((at - kMonths) / 3 + 1);
  if (d < 1 || d > 31 || hh < 0 || hh > 23 || mm < 0 || mm > 59 || ss < 0 || ss > 60) return -1;
  return int64_t(ink::days_from_civil(y, m, d)) * 86400 + hh * 3600 + mm * 60 + ss;
}

}  // namespace httptime
```

- [ ] **Step 3: Write the failing tests**

Replace `test/test_logic/test_main.cpp` with:

```cpp
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

void setUp() {}
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

static Work g_work;

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
```

Notes on the expectations:
- `parse_http_date("Sun, 27 Sep 2026 19:39:05 GMT")`: day 20723 × 86400 + 70745 = 1,790,537,945.
- `test_offline_keeps_frame_until_stale`: the first offline wake is at 11:00 (age 3600 s ≥ the 1800 s TTL but < 7200 s, and Wi-Fi failures do not mark sources failed, spec §4.4), so the frame is unchanged; the second at 12:01:40 (age 7300 s ≥ 7200 s) is stale.
- `test_cached_data_survives_reboot` needs `Work` value-initializable: it is (POD members and default-initialized structs).

Run: `pio test -e native -f test_logic` → Expected: compile errors (`cycle.h` still has the old API).

- [ ] **Step 4: Rewrite `include/cycle.h`**

```cpp
#pragma once
// One wake (spec §4.2): config, caches, network for what is due, render, show if changed,
// sleep. Generic over the hardware (`Ops`) so the ordering and failure paths are host-tested;
// src/main.cpp supplies the board's Ops.
#include <stdint.h>
#include <stdio.h>

#include "compositor.h"
#include "crc32.h"
#include "data_cache.h"
#include "model.h"
#include "query.h"
#include "sources/fred.h"
#include "sources/openmeteo.h"
#include "wake_logic.h"

namespace wake {

constexpr uint32_t NETWORK_BUDGET_MS = 45000;
constexpr uint32_t WIFI_TIMEOUT_MS = 15000;
constexpr uint32_t SNTP_TIMEOUT_MS = 5000;
constexpr uint32_t FRED_MIN_LEFT_MS = 8000;

// Everything one cycle needs that is too big for the stack; main.cpp keeps one static instance.
struct Work {
  uint8_t frame[ink::FRAME_BYTES];
  ink::Model model;
  ink::SeriesData scratch;  // a FRED response is resampled into this, then committed
  uint8_t file[sizeof(ink::SeriesCache) + ink::CACHE_HEADER_BYTES];
};

// Mark dirty, draw, then record what is shown: a reset mid-refresh leaves the panel dirty
// and the next wake redraws (spec §4.3).
template <class Ops>
bool show_if_changed(Ops& ops, Rtc& rtc, const uint8_t* frame) {
  const uint32_t crc = ink::crc32(frame, ink::FRAME_BYTES);
  if (!rtc.panel_dirty && crc == rtc.shown_crc) return false;
  rtc.panel_dirty = true;
  ops.show(frame);
  rtc.shown_crc = crc;
  rtc.panel_dirty = false;
  return true;
}

namespace cycle_detail {

constexpr const char* WEATHER_FILE = "weather.bin";

inline void series_file(char* out, size_t n, const char* fred_id) { snprintf(out, n, "fred_%s.bin", fred_id); }

template <class Ops>
void save_weather(Ops& ops, Work& w, bool fs) {
  if (!fs) return;
  const size_t n = ink::encode_cache(ink::CacheKind::Weather, w.model.weather, w.file, sizeof w.file);
  if (n == 0 || !ops.save(WEATHER_FILE, w.file, n)) ops.log("store: weather not saved");
}

template <class Ops>
void save_series(Ops& ops, Work& w, int i, bool fs) {
  if (!fs) return;
  char name[32];
  series_file(name, sizeof name, ink::CATALOG[w.model.layout.series[i]].fred_id);
  const size_t n = ink::encode_cache(ink::CacheKind::Series, w.model.series[i], w.file, sizeof w.file);
  if (n == 0 || !ops.save(name, w.file, n)) ops.log("store: series not saved");
}

template <class Ops>
void load_caches(Ops& ops, Work& w, bool fs) {
  ink::Model& m = w.model;
  m.weather = ink::WeatherCache{};
  for (auto& s : m.series) s = ink::SeriesCache{};
  if (!fs) return;
  size_t len = 0;
  if (m.layout.has_weather && ops.load(WEATHER_FILE, w.file, sizeof w.file, len) &&
      !ink::decode_cache(ink::CacheKind::Weather, w.file, len, m.weather))
    m.weather = ink::WeatherCache{};
  for (int i = 0; m.layout.has_market && i < m.layout.n_series; ++i) {
    const char* id = ink::CATALOG[m.layout.series[i]].fred_id;
    char name[32];
    series_file(name, sizeof name, id);
    if (!ops.load(name, w.file, sizeof w.file, len) ||
        !ink::decode_cache(ink::CacheKind::Series, w.file, len, m.series[i]) || strcmp(m.series[i].fred_id, id) != 0)
      m.series[i] = ink::SeriesCache{};
  }
}

// Keep only the files this layout uses: removes the old frame store and unconfigured series.
template <class Ops>
void prune_files(Ops& ops, const ink::Layout& l) {
  char names[5][32];
  const char* keep[5];
  int n = 0;
  if (l.has_weather) snprintf(names[n++], sizeof names[0], "%s", WEATHER_FILE);
  for (int i = 0; l.has_market && i < l.n_series; ++i) series_file(names[n++], sizeof names[0], ink::CATALOG[l.series[i]].fred_id);
  for (int i = 0; i < n; ++i) keep[i] = names[i];
  ops.prune(keep, n);
}

inline bool anything_due(const Work& w, int64_t now) {
  const ink::Model& m = w.model;
  const ink::Layout& l = m.layout;
  if (l.has_weather && ink::weather_due(m.weather, ink::weather_key(l), now)) return true;
  const int32_t today = ink::tz::to_local(l.zone, now).day;
  for (int i = 0; l.has_market && i < l.n_series; ++i)
    if (ink::plan_fred(m.series[i], now, today, l.years).kind != ink::FredFetch::None) return true;
  return false;
}

struct NullHandler : ink::json::Handler {
  void scalar(const ink::json::Parser&, ink::json::Type, const char*) override {}
};

}  // namespace cycle_detail

template <class Ops>
int32_t run_cycle(Ops& ops, Rtc& rtc, Work& w, const char* frame_query, const char* version, bool calibration,
                  int32_t fallback_s) {
  using namespace cycle_detail;
  ink::Bitmap frame(w.frame, ink::FRAME_W, ink::FRAME_H);
  ink::Model& m = w.model;
  char msg[192];

  if (calibration) {
    ink::render_calibration(frame);
    show_if_changed(ops, rtc, w.frame);
    return clamp_sleep(fallback_s);
  }
  if (!ink::parse_query(frame_query, m.layout, msg, sizeof msg)) {
    ops.log(msg);
    ink::render_config_error(frame, msg, frame_query);
    show_if_changed(ops, rtc, w.frame);
    return clamp_sleep(fallback_s);
  }
  const ink::Layout& l = m.layout;

  const bool fs = ops.store_begin();
  if (!fs) ops.log("store: unavailable, running from RAM");
  load_caches(ops, w, fs);
  if (fs) prune_files(ops, l);

  bool network_failed = false;
  int32_t retry_after = -1;
  if (!rtc.clock_valid || anything_due(w, ops.now())) {
    const uint32_t t0 = ops.ms();
    auto left = [&]() -> uint32_t {
      const uint32_t used = ops.ms() - t0;
      return used < NETWORK_BUDGET_MS ? NETWORK_BUDGET_MS - used : 0;
    };
    if (!ops.wifi_up(WIFI_TIMEOUT_MS)) {
      ops.log("wifi: not connected");
      network_failed = true;  // sources are not marked failed: they were never asked (spec §4.4)
    } else {
      if (ops.sntp(SNTP_TIMEOUT_MS)) rtc.clock_valid = true;
      if (!rtc.clock_valid) {  // SNTP blocked: any HTTPS response's Date header will do
        NullHandler null;
        const ink::FetchResult r =
            ops.get(ink::OPENMETEO_HOST, "/v1/forecast?latitude=0&longitude=0&current=temperature_2m", null, left());
        if (r.date_epoch > 0) {
          ops.set_clock(r.date_epoch);
          rtc.clock_valid = true;
        }
      }
      bool attempted = false, any_ok = false;
      if (rtc.clock_valid) {
        const int64_t now = ops.now();
        const int32_t today = ink::tz::to_local(l.zone, now).day;
        const ink::WeatherKey key = ink::weather_key(l);
        if (l.has_weather && ink::weather_due(m.weather, key, now)) {
          attempted = true;
          char path[320];
          ink::openmeteo_path(path, sizeof path, l.lat, l.lon, l.metric, l.tz);
          ink::OpenMeteoParser h;
          const ink::FetchResult r = ops.get(ink::OPENMETEO_HOST, path, h, left());
          if (!ink::weather_usable(m.weather, key)) {  // another location or unit: start over
            m.weather = ink::WeatherCache{};
            m.weather.key = key;
          }
          ink::Weather data;
          if (ink::fetch_ok(r) && h.result(data)) {
            m.weather.valid = true;
            m.weather.data = data;
            ink::record_success(m.weather.status, now);
            any_ok = true;
          } else {
            ink::record_failure(m.weather.status, r, now);
            if (r.retry_after_s > retry_after) retry_after = r.retry_after_s;
          }
          snprintf(msg, sizeof msg, "weather: HTTP %d%s", r.http_status, r.complete ? "" : " (incomplete)");
          ops.log(msg);
          save_weather(ops, w, fs);
        }
        bool auth_failed = false;
        for (int i = 0; l.has_market && i < l.n_series; ++i) {
          ink::SeriesCache& c = m.series[i];
          const ink::SeriesDef& sd = ink::CATALOG[l.series[i]];
          const ink::FredPlan p = ink::plan_fred(c, now, today, l.years);
          if (p.kind == ink::FredFetch::None) continue;
          attempted = true;
          snprintf(c.fred_id, sizeof c.fred_id, "%s", sd.fred_id);
          if (auth_failed || left() < FRED_MIN_LEFT_MS) {  // a bad key fails every series; no time left
            ink::FetchResult skipped;
            skipped.auth_error = auth_failed;
            ink::record_failure(c.status, skipped, now);
            save_series(ops, w, i, fs);
            continue;
          }
          ink::SundayResampler rs;
          const bool tail = p.kind == ink::FredFetch::Tail && rs.begin_tail(w.scratch, c.data, p.s0, today);
          if (!tail) rs.begin_full(w.scratch, p.keep_from, today);
          const int32_t start = tail ? p.observation_start : ink::window_start_day(today, l.years) - 62;
          char path[256];
          ink::fred_path(path, sizeof path, sd.fred_id, ops.fred_api_key(), start);
          ink::FredParser fp(rs);
          ink::FetchResult r = ops.get(ink::FRED_HOST, path, fp, left());
          if (ink::fetch_ok(r) && fp.saw_observations() && rs.finish(p.keep_from)) {
            c.valid = true;
            c.data = w.scratch;
            if (!tail) c.full_fetched_at = now;
            ink::record_success(c.status, now);
            any_ok = true;
          } else {
            r.auth_error = (r.http_status == 400 || r.http_status == 403) && fp.api_key_error();
            auth_failed = r.auth_error;
            if (tail && ink::fetch_ok(r)) c.full_fetched_at = 0;  // the tail did not fit the cache: refetch in full
            ink::record_failure(c.status, r, now);
            if (r.retry_after_s > retry_after) retry_after = r.retry_after_s;
          }
          snprintf(msg, sizeof msg, "fred %s: HTTP %d%s%s", sd.fred_id, r.http_status, tail ? " tail" : " full",
                   r.auth_error ? " (API key rejected)" : "");
          ops.log(msg);  // never the path: it contains the API key
          save_series(ops, w, i, fs);
        }
      }
      ops.close_connections();
      network_failed = attempted && !any_ok;
    }
    ops.wifi_off();
  }

  if (!rtc.clock_valid) {  // no idea what day it is: leave the panel as it is
    if (rtc.fail_count < 255) ++rtc.fail_count;
    return backoff_seconds(rtc.fail_count, retry_after);
  }
  m.now = ops.now();  // one read after the network step: date, footer, staleness and next wake agree
  ink::build_frame(frame, m, version);
  show_if_changed(ops, rtc, w.frame);
  if (network_failed) {
    if (rtc.fail_count < 255) ++rtc.fail_count;
    return backoff_seconds(rtc.fail_count, retry_after);
  }
  rtc.fail_count = 0;
  return clamp_sleep(ink::tz::next_refresh_seconds(l.zone, m.now));
}

}  // namespace wake
```

In `test_fred_auth_rejected_stops_after_first`, the weather succeeded so `any_ok` is true and the wake is not a network failure; the four series all end `auth_rejected` (the first from the response, the rest from `skipped.auth_error`).

- [ ] **Step 5: Run**

Run: `pio test -e native`
Expected: all suites PASS (`test_logic` now has 17 tests).

- [ ] **Step 6: Commit**

```bash
git add include/wake_logic.h include/cycle.h include/http_time.h test/test_logic/test_main.cpp
git commit -m "Firmware: wake cycle that fetches what is due, renders on the board and shows only changes

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

### Task 18: Board glue: Wi-Fi, SNTP, HTTPS streaming, LittleFS, main

**Files:**
- Create: `src/net.h`, `src/net.cpp`, `src/data_store.h`, `src/data_store.cpp`
- Rewrite: `src/main.cpp`, `src/panel.h`, `src/panel.cpp`, `include/config.h`
- Modify: `include/secrets.h.example`, `tools/gen_ca_certs.sh`, `include/ca_certs.h` (regenerated), `extras/smoke/main.cpp` (network step), `justfile` (`flash-dev`)
- Delete: `src/fetch.h`, `src/fetch.cpp`, `src/frame_store.h`, `src/frame_store.cpp`, `include/badge.h`, `include/body_reader.h`

**Interfaces:**
- Consumes: `wake::run_cycle`, `wake::Work`, `wake::Rtc`, `wake::rtc_begin` (Task 17); `ink::FetchResult`, `ink::json::Handler`/`Parser`.
- Produces: the board firmware (`pio run -e supermini-c6`).

- [ ] **Step 1: Regenerate the CA bundle for the two APIs**

Edit `tools/gen_ca_certs.sh`: replace the comment and `certs=(...)` with the root file names found in Task 1 Step 2, e.g.:

```bash
# Regenerate include/ca_certs.h from the system trust store (spec §2.6): the roots that
# api.open-meteo.com and api.stlouisfed.org chain to. Check them when regenerating:
#   openssl s_client -connect <host>:443 -servername <host> -showcerts </dev/null | grep -E '^ *i:'
certs=(<root file names from Task 1 Step 2, without .pem>)
```
and the generated comment line `// Root CAs for api.open-meteo.com and api.stlouisfed.org: ${certs[*]}`.
Run: `tools/gen_ca_certs.sh`
Expected: `wrote include/ca_certs.h (N roots)`.

- [ ] **Step 2: Config and secrets**

Replace `include/config.h` with:

```cpp
#pragma once
// Board configuration (tracked). Wi-Fi credentials and the FRED key live in secrets.h (gitignored).

// Firmware version, shown in the dashboard footer and sent as the User-Agent. Set by
// tools/version.py from the repo's VERSION file; `just flash-dev` adds -<git hash>.
#ifndef INKBOARD_VERSION
#define INKBOARD_VERSION "dev"
#endif

// This board's layout and options (README "Change the dashboard layout"). Preview it on a
// computer with: just preview
#define FRAME_QUERY \
  "w=market_trends:2/3,calendar_weather:1/3&lat=34.05&lon=-118.24&tz=America/Los_Angeles&units=imperial"

// For bring-up (docs/dashboard-bringup.md step 1): 1 draws the calibration pattern instead of the dashboard.
#define USE_CALIBRATION_PATTERN 0

// 1 (prod): deep-sleep between updates unless a computer is on USB. `just flash-dev` builds
// with -DINKBOARD_DEEP_SLEEP=0: the board never deep-sleeps, so USB and logs stay available.
#ifndef INKBOARD_DEEP_SLEEP
#define INKBOARD_DEEP_SLEEP 1
#endif

// Sleep after a config error or while showing the calibration pattern.
#define FALLBACK_SLEEP_S 3600

// The renderer uses bit 1 = white, the same as GxEPD2's own buffer, so no inversion is
// expected. Confirmed with the calibration pattern in docs/dashboard-bringup.md.
#define FRAME_INVERT false
```

`include/secrets.h.example` (FRED key was added in Task 1; confirm the file reads):

```cpp
#pragma once
// Copy to include/secrets.h (gitignored) and fill in.
#define WIFI_SSID "your-network"
#define WIFI_PASSWORD "your-password"
// Free key from https://fredaccount.stlouisfed.org/apikeys (needed for market_trends).
#define FRED_API_KEY "your-fred-api-key"
```

- [ ] **Step 3: Write `src/data_store.h/.cpp`**

`src/data_store.h`:

```cpp
#pragma once
#include <stddef.h>
#include <stdint.h>

// Cache files on LittleFS (spec §2.5): names are bare ("weather.bin"), stored at the root.
bool store_begin();  // mounts, formatting on failure: the partition only holds caches
bool store_load(const char* name, uint8_t* buf, size_t cap, size_t& len);
bool store_save(const char* name, const uint8_t* data, size_t len);  // temp file + rename
void store_prune(const char* const* keep, int n);                    // deletes every other file
```

`src/data_store.cpp`:

```cpp
#include "data_store.h"

#include <LittleFS.h>
#include <string.h>

static String path_of(const char* name) { return String("/") + name; }

bool store_begin() { return LittleFS.begin(true); }

bool store_load(const char* name, uint8_t* buf, size_t cap, size_t& len) {
  File f = LittleFS.open(path_of(name), "r");
  if (!f) return false;
  len = f.size();
  const bool ok = len <= cap && f.read(buf, len) == len;
  f.close();
  return ok;
}

// Write to <name>.tmp, then rename over <name>: a power cut never leaves a torn file.
bool store_save(const char* name, const uint8_t* data, size_t len) {
  const String path = path_of(name), tmp = path + ".tmp";
  File f = LittleFS.open(tmp, "w");
  if (!f) return false;
  const size_t n = f.write(data, len);
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

void store_prune(const char* const* keep, int n) {
  String doomed[16];
  int nd = 0;
  File root = LittleFS.open("/");
  for (File f = root.openNextFile(); f && nd < 16; f = root.openNextFile()) {
    const String name = f.name();
    f.close();
    bool k = false;
    for (int i = 0; i < n; ++i) k = k || name == keep[i];
    if (!k) doomed[nd++] = name;  // collect first: removing while iterating skips entries
  }
  root.close();
  for (int i = 0; i < nd; ++i) LittleFS.remove(path_of(doomed[i].c_str()));
}
```

- [ ] **Step 4: Write `src/net.h/.cpp`**

`src/net.h`:

```cpp
#pragma once
#include <stdint.h>

#include "data_cache.h"
#include "json_stream.h"

bool net_wifi_up(uint32_t timeout_ms);
void net_wifi_off();                    // also closes the keep-alive connection
bool net_sntp(uint32_t timeout_ms);     // true once the system clock is set
void net_set_clock(int64_t epoch);
// HTTPS GET with the body streamed into a JSON handler; one keep-alive connection per host.
ink::FetchResult net_get(const char* host, const char* path, ink::json::Handler& h, uint32_t timeout_ms);
void net_close();
```

`src/net.cpp`:

```cpp
#include "net.h"

#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include <sys/time.h>

#include "ca_certs.h"
#include "config.h"
#include "http_time.h"
#include "secrets.h"
#include "wake_logic.h"

// Feeds the body straight into the JSON tokenizer, so nothing is buffered (spec §2.6).
// writeToStream() wants a Stream; only the write side is used.
class ParserSink : public Stream {
 public:
  explicit ParserSink(ink::json::Parser& p) : p_(p) {}
  size_t write(uint8_t c) override { return p_.feed(reinterpret_cast<const char*>(&c), 1) ? 1 : 0; }
  size_t write(const uint8_t* b, size_t n) override { return p_.feed(reinterpret_cast<const char*>(b), n) ? n : 0; }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}

 private:
  ink::json::Parser& p_;
};

static NetworkClientSecure* g_client = nullptr;
static String g_host;

bool net_wifi_up(uint32_t timeout_ms) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  const uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > timeout_ms) return false;
    delay(100);
  }
  return true;
}

void net_close() {
  if (g_client) {
    g_client->stop();
    delete g_client;
    g_client = nullptr;
  }
  g_host = "";
}

void net_wifi_off() {
  net_close();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

bool net_sntp(uint32_t timeout_ms) {
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  const uint32_t start = millis();
  while (sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED) {
    if (millis() - start > timeout_ms) return false;
    delay(20);
  }
  return true;
}

void net_set_clock(int64_t epoch) {
  struct timeval tv = {static_cast<time_t>(epoch), 0};
  settimeofday(&tv, nullptr);
}

ink::FetchResult net_get(const char* host, const char* path, ink::json::Handler& h, uint32_t timeout_ms) {
  ink::FetchResult r;
  if (g_client == nullptr || g_host != host) {  // the FRED series share one TLS session
    net_close();
    g_client = new NetworkClientSecure;
    g_client->setCACert(CA_BUNDLE_PEM);
    g_client->setHandshakeTimeout(10);
    g_host = host;
  }
  HTTPClient http;
  http.setReuse(true);
  http.setConnectTimeout(8000);
  http.setTimeout(timeout_ms < 10000 ? timeout_ms : 10000);
  http.setUserAgent("inkboard/" INKBOARD_VERSION);
  static const char* keys[] = {"Date", "Retry-After"};
  http.collectHeaders(keys, 2);
  if (!http.begin(*g_client, host, 443, path, true)) return r;
  r.http_status = http.GET();
  r.date_epoch = httptime::parse_http_date(http.header("Date").c_str());
  r.retry_after_s = wake::parse_seconds(http.header("Retry-After").c_str());
  if (r.http_status == 200 || r.http_status == 400 || r.http_status == 403) {  // 4xx bodies explain a bad key
    ink::json::Parser p(h);
    ParserSink sink(p);
    const int size = http.getSize();
    const int written = http.writeToStream(&sink);
    r.complete = written >= 0 && (size < 0 || written == size) && p.finish();
  }
  http.end();
  if (r.http_status < 0) net_close();  // a broken session must not be reused
  return r;
}
```

- [ ] **Step 5: Rewrite the panel**

`src/panel.h`:

```cpp
#pragma once
#include <stdint.h>

// Powers the HAT, draws a 48,000-byte frame with one full refresh, hibernates, powers off.
void panel_show(const uint8_t* frame);
```

`src/panel.cpp`: keep the file as it is but delete `panel_show_error()`, the two `#include <Fonts/...>` lines and the comment about the paged error screen; change the page-buffer comment to "A 1/8-height page buffer (6 KB) is GxEPD2's minimum; frames go straight to the controller with writeImage and never touch it."

- [ ] **Step 6: Rewrite `src/main.cpp`**

```cpp
// inkboard (spec §4): wake, refresh the data that is due, render the dashboard on the board,
// show it if it changed, sleep. The cycle lives in include/cycle.h (host-tested); this file
// only supplies the hardware operations.
#include <Arduino.h>
#include <driver/gpio.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <soc/soc_caps.h>
#include <sys/time.h>

#include "config.h"
#include "cycle.h"
#include "data_store.h"
#include "net.h"
#include "panel.h"
#include "pins.h"
#if !__has_include("secrets.h")
#error "Copy include/secrets.h.example to include/secrets.h and fill it in."
#endif
#include "secrets.h"

#ifndef FRED_API_KEY
#define FRED_API_KEY ""  // older secrets.h: fine for layouts without market_trends
#endif

static constexpr bool same_text(const char* a, const char* b) {
  return *a == *b && (*a == '\0' || same_text(a + 1, b + 1));
}
static constexpr bool starts_with(const char* s, const char* p) { return *p == '\0' || (*s == *p && starts_with(s + 1, p + 1)); }
static constexpr bool contains(const char* s, const char* p) { return *s != '\0' && (starts_with(s, p) || contains(s + 1, p)); }
static_assert(!same_text(WIFI_SSID, "your-network"), "Put your Wi-Fi credentials in include/secrets.h");
static_assert(!contains(FRAME_QUERY, "market_trends") ||
                  !(same_text(FRED_API_KEY, "") || same_text(FRED_API_KEY, "your-fred-api-key")),
              "FRAME_QUERY uses market_trends: put your FRED_API_KEY in include/secrets.h");

RTC_NOINIT_ATTR static wake::Rtc g_rtc;
static wake::Work g_work;  // frame + caches + scratch (~75 KB): static, never on the stack
static char g_version[40];

struct BoardOps {
  bool store_begin() { return ::store_begin(); }
  bool load(const char* n, uint8_t* b, size_t cap, size_t& len) { return store_load(n, b, cap, len); }
  bool save(const char* n, const uint8_t* d, size_t len) { return store_save(n, d, len); }
  void prune(const char* const* keep, int n) { store_prune(keep, n); }
  bool wifi_up(uint32_t ms) { return net_wifi_up(ms); }
  void wifi_off() { net_wifi_off(); }
  bool sntp(uint32_t ms) { return net_sntp(ms); }
  void set_clock(int64_t e) { net_set_clock(e); }
  int64_t now() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return tv.tv_sec;
  }
  uint32_t ms() { return millis(); }
  ink::FetchResult get(const char* host, const char* path, ink::json::Handler& h, uint32_t timeout_ms) {
    return net_get(host, path, h, timeout_ms);
  }
  void close_connections() { net_close(); }
  void show(const uint8_t* frame) { panel_show(frame); }
  const char* fred_api_key() { return FRED_API_KEY; }
  void log(const char* line) { Serial.println(line); }
};

static BoardOps g_ops;

// Which resets keep the RTC-backed system time (measured in plan Task 1, spec §8).
static bool clock_survives(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_DEEPSLEEP:
    case ESP_RST_SW:
    case ESP_RST_PANIC:
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
      return true;
    default:
      return false;
  }
}

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
  Serial.printf("inkboard %s\n", INKBOARD_VERSION);
  // Keep the HAT off while awake: drive PWR LOW before releasing the deep-sleep hold,
  // otherwise GPIO1 floats (no pull-down on the switch) during Wi-Fi and the fetch.
  pinMode(PIN_EPD_PWR, OUTPUT);
  digitalWrite(PIN_EPD_PWR, LOW);
  gpio_hold_dis(static_cast<gpio_num_t>(PIN_EPD_PWR));
  pinMode(PIN_LED_STATUS, OUTPUT);  // heartbeat
  digitalWrite(PIN_LED_STATUS, LOW);
  delay(30);
  digitalWrite(PIN_LED_STATUS, HIGH);
  const esp_reset_reason_t reason = esp_reset_reason();
  wake::rtc_begin(g_rtc, clock_survives(reason));
  snprintf(g_version, sizeof g_version, "fw %s", INKBOARD_VERSION);
  Serial.printf("reset reason %d, clock %s\n", static_cast<int>(reason), g_rtc.clock_valid ? "valid" : "unknown");
#if !INKBOARD_DEEP_SLEEP
  Serial.println("dev build: deep sleep disabled");
#endif
}

// One cycle, then wait for the next. With a computer on USB (it sends SOF frames; a charger
// doesn't) the board stays awake so its USB port is always there for flashing and logs;
// otherwise, or once unplugged, it deep-sleeps the rest. Deep sleep never returns: the next
// timer wake starts again at setup().
void loop() {
  const int32_t sleep_s =
      wake::run_cycle(g_ops, g_rtc, g_work, FRAME_QUERY, g_version, USE_CALIBRATION_PATTERN, FALLBACK_SLEEP_S);
  Serial.printf("heap: free %u, min %u\n", static_cast<unsigned>(ESP.getFreeHeap()),
                static_cast<unsigned>(ESP.getMinFreeHeap()));
  const uint32_t start = millis();
  const int64_t total_ms = static_cast<int64_t>(sleep_s) * 1000;
  bool announced = false;
  for (;;) {
    const int32_t left = static_cast<int32_t>(total_ms - static_cast<int64_t>(millis() - start));
    switch (wake::idle_step(!INKBOARD_DEEP_SLEEP || HWCDC::isPlugged(), left)) {
      case wake::Idle::RunNow:
        return;
      case wake::Idle::DeepSleep:
        deep_sleep(wake::remaining_sleep_s(left));
        return;
      case wake::Idle::Wait:
        if (!announced) {
          Serial.printf("%s: staying awake, next update in %ld s\n",
                        INKBOARD_DEEP_SLEEP ? "computer on USB" : "deep sleep off (dev build)", static_cast<long>(sleep_s));
          announced = true;
        }
        delay(1000);
        break;
    }
  }
}
```

Replace the `clock_survives` list with exactly the reset reasons Task 1 measured as keeping time (spec §8).

- [ ] **Step 7: Delete the server-era firmware files and update the smoke test and flash-dev**

```bash
git rm src/fetch.h src/fetch.cpp src/frame_store.h src/frame_store.cpp include/badge.h include/body_reader.h
```

In `extras/smoke/main.cpp`, `checkNetwork()`: replace the `String url = String(SERVER_URL) + "/v1/test.bin";` block with a GET of `https://api.open-meteo.com/v1/forecast?latitude=0&longitude=0&current=temperature_2m` (always TLS; drop the `plain` client), and report `PASS` on HTTP 200 (any size) with the elapsed ms; update the header comment lines 11–12 to "fetches a tiny Open-Meteo forecast over HTTPS (separates Wi-Fi, firewall and TLS problems)" and the comment above `checkNetwork()` likewise.

In `justfile` `flash-dev`: delete the `url=` line, the `echo "dev firmware ... -> $url (keep 'just dev' running here)"` line becomes `echo "dev firmware $INKBOARD_VERSION (deep sleep off)"`, and `PLATFORMIO_BUILD_FLAGS` becomes `"-DINKBOARD_DEEP_SLEEP=0"`. Update its doc comment to `# Dev build: deep sleep off, version with the git hash (Linux). ENV: supermini-c6, smoke, minimal.`

- [ ] **Step 8: Build everything**

Run:
```bash
pio run -e supermini-c6 && pio run -e smoke && pio run -e minimal && pio test -e native
```
Expected: all `SUCCESS`, all tests PASS. Note `RAM:` and `Flash:` of `supermini-c6` (Flash must stay under the 1.9 MB slot; RAM shows the static `.bss`, about 130 KB is expected).

- [ ] **Step 9: Commit**

```bash
git add -A src include extras/smoke/main.cpp tools/gen_ca_certs.sh justfile
git commit -m "Firmware: render on the board (Wi-Fi, SNTP, streamed HTTPS into the parsers, LittleFS caches)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

### Task 19: Check it on the board

Manual, with the user. Ask before every upload.

**Files:**
- Modify: `docs/superpowers/specs/2026-09-30-ondevice-render-design.md` (append results to §8)

- [ ] **Step 1: Flash the dev build and watch one cycle**

Ask the user to connect the board. Run `just flash-dev`.
Expected serial: `reset reason …`, `weather: HTTP 200`, four `fred <ID>: HTTP 200 full`, `heap: free …`, then `deep sleep off (dev build): staying awake, next update in … s`; the panel shows the dashboard. Compare it with `just preview` taken at the same time.

- [ ] **Step 2: Walk the failure table (spec §4.4)**

With the dev build (no deep sleep, so each next cycle can be forced by tapping RESET and the clock stays valid only for the resets measured in Task 1):

| Check | How | Expected |
|---|---|---|
| Next wake fetches weather only | wait for the next cycle | `weather: HTTP 200`, no `fred` lines |
| Wi-Fi off | switch the AP off (or set a wrong SSID build) and wait one cycle | `wifi: not connected`, panel unchanged, sleep 300 |
| Bad FRED key | build with `FRED_API_KEY "0000…"` (32 zeros), cold boot | one `fred SP500: HTTP 400 full (API key rejected)`, market column "FRED key rejected: / check secrets.h" |
| Bad query | `FRAME_QUERY "w=bad"` | config error screen, once |
| Cold boot offline | power-cycle with the AP off | panel unchanged; retries every 5 → 15 → 60 min |
| Calibration | `USE_CALIBRATION_PATTERN 1` | the calibration pattern, as in `docs/dashboard-bringup.md` |

Restore `config.h` and `secrets.h` afterwards (`git diff include/config.h` must be empty).

- [ ] **Step 3: Production build, battery wake**

Run `just flash`, unplug USB, run on battery for at least two hours. Ask the user to note the panel at each hour; then reconnect and read the log of one wake (`just monitor` right after tapping RESET).
Expected: hourly refreshes; measure the awake time of a weather-only wake and of a 6-hourly FRED wake from the serial timestamps.

- [ ] **Step 4: Record and commit**

Append to spec §8: awake time for weather-only and FRED wakes, free heap after a cycle, and the result of each row of the table. Then:

```bash
git add docs/superpowers/specs/2026-09-30-ondevice-render-design.md
git commit -m "Docs: on-board results of on-device rendering

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

## Phase 5: Sign-off and removal

### Task 20: Side-by-side sign-off, delete the server, update docs

**Files:**
- Delete: `server/`, `docs/cloudflare-tunnel.md`, `extras/spike/`, `tools/ref_render.py` (its PBMs stay)
- Modify: `justfile`, `platformio.ini` (remove `[env:spike]`), `README.md`, `CLAUDE.md`, `docs/dashboard-bringup.md`, `docs/superpowers/specs/2026-09-27-dashboard-design.md` (header note), `VERSION`, `.gitignore`, `tools/setup.ps1` and `justfile` `setup` (FRED key hint)

- [ ] **Step 1: Prepare the side-by-side review**

Run `pio test -e native -f test_widgets` and list each reference with its differing-pixel count. For every scenario write a side-by-side PNG (server left, board right) to `/tmp/claude-1000/sbs/`:

```bash
mkdir -p /tmp/claude-1000/sbs && uv run --with pillow==12.3.0 python - <<'EOF'
from pathlib import Path
from PIL import Image
for ref in sorted(Path("test/reference/widgets").glob("*.pbm")):
    gold = Path("test/goldens") / (ref.stem + ".png")
    if not gold.exists():
        continue
    a, b = Image.open(ref).convert("L"), Image.open(gold).convert("L")
    out = Image.new("L", (a.width + b.width + 10, max(a.height, b.height)), 128)
    out.paste(a, (0, 0)); out.paste(b, (a.width + 10, 0))
    out.save(f"/tmp/claude-1000/sbs/{ref.stem}.png")
    print(ref.stem)
EOF
```

Show the user the images (Read each PNG) together with the pixel counts, and ask for explicit sign-off that the board's rendering is equivalent. **Do not continue without it.**

- [ ] **Step 2: Delete the server and its tooling**

```bash
git rm -r server docs/cloudflare-tunnel.md extras/spike tools/ref_render.py
```

`platformio.ini`: delete the `[env:spike]` block.

`justfile`: delete the variables `public_url`, `local_url`, `query`, and the recipes `setup-server`, `dev`, `up`, `down`, `logs`, `check`. Replace `test` with:

```just
# Host tests (no network, no board); INKBOARD_UPDATE_GOLDENS=1 rewrites the golden PNGs.
test *ARGS:
    pio test -e native {{ARGS}}
```

Add generator recipes:

```just
# Regenerate the bitmap fonts, time-zone table, Pillow reference images, or recorded fixtures.
gen-fonts:
    uv run tools/gen_fonts.py
gen-tz:
    uv run tools/gen_tz_table.py
gen-primitives:
    uv run tools/ref_primitives.py
record-fixtures:
    uv run tools/record_fixtures.py
```

Change the header comment to `# inkboard: e-paper dashboard firmware (PlatformIO). \`just\` lists the recipes.` and the Windows note to `# On Windows only \`setup\`, \`flash\` and \`monitor\` are supported (PowerShell).`

In `setup` (Linux) change the `secrets.h` messages to mention the FRED key: `"created include/secrets.h; put your Wi-Fi name and password and your FRED API key in it"`. Make the same change in `tools/setup.ps1`.

- [ ] **Step 3: Update the docs**

- `README.md`:
  - Intro: the board fetches weather and market data itself and renders the screen; no server.
  - Board-owner setup: step 3 becomes "Set your Wi-Fi credentials and FRED key" — fill in `WIFI_SSID`, `WIFI_PASSWORD` and `FRED_API_KEY` (free at https://fredaccount.stlouisfed.org/apikeys; only needed when the layout uses `market_trends`).
  - Delete the "Server provider" section.
  - "Change the dashboard layout": same `FRAME_QUERY` syntax and option table; step 3 becomes "Preview it on this computer: `just preview` (or `just preview "<query>"`), which writes `.pio/preview.png`."
  - Repository layout: drop `server/`; add `include/render/`, `include/widgets/`, `include/sources/`, `tools/` (generators), `test/` (suites, fixtures, goldens, reference images).
  - Roadmap: remove server items.
- `CLAUDE.md`:
  - Intro line: "ESP32-C6 firmware (C++ / Arduino-ESP32 3.x / PlatformIO + pioarduino) that fetches Open-Meteo and FRED and renders 800×480 1-bit frames on the board. Design: `docs/superpowers/specs/2026-09-30-ondevice-render-design.md` (the widget designs are in `2026-09-27-dashboard-design.md` §4–§5)."
  - Commands: `just test` → `pio test -e native` (with `INKBOARD_UPDATE_GOLDENS=1`); add `just preview`; `just flash-dev` = deep sleep off; delete `just dev`, `just up/down/check/logs`.
  - Rules: delete the production-containers rule; secrets now `include/secrets.h` (Wi-Fi + `FRED_API_KEY`) only; add "Never log a FRED URL: it contains the API key."
  - Portable-code rule: list the new header folders.
- `docs/dashboard-bringup.md`: remove server steps; step 1 uses `USE_CALIBRATION_PATTERN 1`; add the FRED key to the prerequisites.
- `docs/superpowers/specs/2026-09-27-dashboard-design.md`: under the title add `> **Superseded in part (2026-09-30):** rendering moved onto the board; see 2026-09-30-ondevice-render-design.md. §1–§3 and §6 describe the removed server; §4–§5 still define the widgets.`
- `VERSION`: `2.0.0`.
- `.gitignore`: delete entries that only applied to `server/` (e.g. `server/.env`, `server/.cache`); keep `include/secrets.h`.

Check for leftovers: `grep -rn "server/\|SERVER_URL\|just dev\|inkboard.signalwave.dev\|frame.bin" --include=*.md --include=*.h --include=*.cpp --include=justfile --include=*.ini . | grep -v docs/superpowers/` → Expected: no output.

- [ ] **Step 4: Full verification**

Run:
```bash
pio test -e native && pio run -e supermini-c6 && pio run -e smoke && pio run -e minimal && pio run -e preview && just preview "" --fixtures
```
Expected: all PASS / SUCCESS, preview written.

- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "Remove the render server: the board renders the dashboard itself (v2.0.0)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

- [ ] **Step 6: Hand back**

Per the user's global workflow: run the regular code review (`superpowers:requesting-code-review`) on `main..feat/ondevice-render`, fix valid Critical/Important findings, then one read-only Codex review (`codex:codex-rescue`) of the same range. Merging and removing the worktree are the user's call.
