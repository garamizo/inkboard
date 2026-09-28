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
