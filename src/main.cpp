// inkboard thin client (spec §6): wake, fetch the frame, draw it if it changed, sleep.
// The cycle and all decisions live in include/cycle.h and wake_logic.h (host-tested);
// this file only supplies the hardware operations.
#include <Arduino.h>
#include <WiFi.h>
#include <driver/gpio.h>
#include <esp_sleep.h>
#include <soc/soc_caps.h>

#include "config.h"
#include "cycle.h"
#include "fetch.h"
#include "frame_store.h"
#include "panel.h"
#include "pins.h"

// RTC_NOINIT_ATTR survives deep sleep AND reset/panic (RTC_DATA_ATTR would be zeroed on
// RESET); only power loss leaves garbage, which the magic check turns into a cold boot.
RTC_NOINIT_ATTR static wake::Rtc g_rtc;
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
    if (wifi_connect(15000)) {
      Serial.printf("wifi: connected, ip %s\n", WiFi.localIP().toString().c_str());
      r = fetch_frame(etag, buf);
    } else {
      Serial.printf("wifi: not connected (status %d)\n", static_cast<int>(WiFi.status()));
    }
    wifi_off();  // before drawing: the refresh takes seconds
    Serial.printf("GET -> %d (sent etag: %s)\n", r.http_status, etag[0] ? etag : "none");
    return r;
  }
  void show(const uint8_t* frame) { panel_show(frame); }
  void show_error(const char* message) { panel_show_error(message, FRAME_QUERY); }
};

static BoardOps g_ops{false};

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

#if !INKBOARD_DEEP_SLEEP
  Serial.println("dev build: deep sleep disabled, server " SERVER_URL);
#endif
  g_ops.fs = store_begin();
  Serial.printf("flash store: %s\n", g_ops.fs ? "mounted" : "UNAVAILABLE");
}

// One cycle, then wait for the next. With a computer on USB (it sends SOF frames; a charger
// doesn't) the board stays awake so its USB port is always there for flashing and logs;
// otherwise, or once unplugged, it deep-sleeps the rest. Deep sleep never returns: the next
// timer wake starts again at setup().
void loop() {
  const int32_t sleep_s = wake::run_cycle(g_ops, g_rtc, g_frame, FALLBACK_SLEEP_S);
  const uint32_t start = millis();
  const int64_t total_ms = static_cast<int64_t>(sleep_s) * 1000;
  bool announced = false;
  for (;;) {
    int32_t left = static_cast<int32_t>(total_ms - static_cast<int64_t>(millis() - start));
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
