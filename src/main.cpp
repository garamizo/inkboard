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

// Which resets keep the RTC-backed system time (measured in plan Task 1, spec §8: the C6
// reports the RESET pin as power-on). Resets that were not measured count as losing it: an
// SNTP sync is cheaper than a wrong date.
static bool clock_survives(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_DEEPSLEEP:
    case ESP_RST_SW:
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
