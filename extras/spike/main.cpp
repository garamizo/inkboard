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
    // ISRG Root X1 (api.open-meteo.com)
    "-----BEGIN CERTIFICATE-----\n"
    "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\n"
    "-----END CERTIFICATE-----\n"
    // DigiCert Global Root G3 (api.stlouisfed.org)
    "-----BEGIN CERTIFICATE-----\n"
    "MIICPzCCAcWgAwIBAgIQBVVWvPJepDU1w6QP1atFcjAKBggqhkjOPQQDAzBhMQsw\n"
    "CQYDVQQGEwJVUzEVMBMGA1UEChMMRGlnaUNlcnQgSW5jMRkwFwYDVQQLExB3d3cu\n"
    "ZGlnaWNlcnQuY29tMSAwHgYDVQQDExdEaWdpQ2VydCBHbG9iYWwgUm9vdCBHMzAe\n"
    "Fw0xMzA4MDExMjAwMDBaFw0zODAxMTUxMjAwMDBaMGExCzAJBgNVBAYTAlVTMRUw\n"
    "EwYDVQQKEwxEaWdpQ2VydCBJbmMxGTAXBgNVBAsTEHd3dy5kaWdpY2VydC5jb20x\n"
    "IDAeBgNVBAMTF0RpZ2lDZXJ0IEdsb2JhbCBSb290IEczMHYwEAYHKoZIzj0CAQYF\n"
    "K4EEACIDYgAE3afZu4q4C/sLfyHS8L6+c/MzXRq8NOrexpu80JX28MzQC7phW1FG\n"
    "fp4tn+6OYwwX7Adw9c+ELkCDnOg/QW07rdOkFFk2eJ0DQ+4QE2xy3q6Ip6FrtUPO\n"
    "Z9wj/wMco+I+o0IwQDAPBgNVHRMBAf8EBTADAQH/MA4GA1UdDwEB/wQEAwIBhjAd\n"
    "BgNVHQ4EFgQUs9tIpPmhxdiuNkHMEWNpYim8S8YwCgYIKoZIzj0EAwMDaAAwZQIx\n"
    "AK288mw/EkrRLTnDCgmXc/SINoyIJ7vmiI1Qhadj+Z4y3maTD/HMsQmP3Wyr+mt/\n"
    "oAIwOWZbwmSNuJ5Q3KjVSaLtx9zRSX8XAbjIho9OjIgrqJqpisXRAL34VOKa5Vt8\n"
    "sycX\n"
    "-----END CERTIFICATE-----\n";

// ≈ the dashboard's static RAM: Work (frame, model, scratch, file buffer), four Summary, the
// chart points and GxEPD2's page buffer (Tasks 15-18, ~115 KB). Touched in setup() so the
// linker keeps it.
static uint8_t g_resident[120 * 1024];
RTC_NOINIT_ATTR static uint32_t g_magic;
RTC_NOINIT_ATTR static uint32_t g_runs;        // boots since the spike was flashed

static void heap(const char* tag) {
  Serial.printf("SPIKE heap_%s free=%u min=%u largest=%u stack_hwm=%u\n", tag,
                (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size(),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                (unsigned)uxTaskGetStackHighWaterMark(nullptr));
}

static int64_t now_s() { struct timeval tv; gettimeofday(&tv, nullptr); return tv.tv_sec; }

// Counts body bytes without storing them (the parser will stream); writeToStream decodes chunking.
struct CountingSink : public Stream {
  size_t bytes = 0;
  size_t write(uint8_t) override { ++bytes; return 1; }
  size_t write(const uint8_t*, size_t n) override { bytes += n; return n; }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}
};

// GET over one client; reports writeToStream's return value (bytes, or a negative HTTPC error).
static void get(NetworkClientSecure& c, HTTPClient& http, const String& url, const char* tag) {
  uint32_t t0 = millis();
  http.setReuse(true);
  http.begin(c, url);
  const char* keys[] = {"Date", "Content-Length", "Transfer-Encoding"};
  http.collectHeaders(keys, 3);
  int code = http.GET();
  int written = 0;
  size_t counted = 0;
  if (code > 0) {
    CountingSink sink;
    written = http.writeToStream(&sink);
    counted = sink.bytes;
  }
  heap(tag);
  Serial.printf("SPIKE get_%s code=%d written=%d counted=%u ms=%lu te=%s date=\"%s\"\n", tag, code, written,
                (unsigned)counted, millis() - t0, http.header("Transfer-Encoding").c_str(), http.header("Date").c_str());
  http.end();  // with setReuse(true) the TLS session stays open if the server allows keep-alive
}

void setup() {
  Serial.begin(115200);
  delay(2000);
  for (size_t i = 0; i < sizeof g_resident; ++i) g_resident[i] = static_cast<uint8_t>(i);
  volatile uint32_t sum = 0;
  for (size_t i = 0; i < sizeof g_resident; i += 512) sum += g_resident[i];
  const bool rtc_ok = g_magic == 0x5B1CE001;
  g_runs = rtc_ok ? g_runs + 1 : 0;
  const int64_t boot_now = now_s();  // what the RTC-backed clock says before SNTP
  const uint32_t boot_ms = millis();
  Serial.printf("SPIKE run=%u reset_reason=%d rtc_magic_ok=%d now_before_sntp=%lld resident_sum=%u\n",
                (unsigned)g_runs, (int)esp_reset_reason(), rtc_ok, (long long)boot_now, (unsigned)sum);
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
  // Drift = SNTP time minus what the clock said at boot (plus the time since boot). On a run
  // whose clock was set before, |drift| < 5 s means this kind of reset kept system time.
  const bool sntp_ok = sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED;
  const int64_t synced = now_s();
  Serial.printf("SPIKE sntp_ms=%lu sntp_ok=%d now=%lld", millis() - t0, sntp_ok, (long long)synced);
  if (sntp_ok) Serial.printf(" drift_s=%lld", (long long)(synced - (boot_now + (millis() - boot_ms) / 1000)));
  Serial.println();
  if (sntp_ok) g_magic = 0x5B1CE001;  // never trust a later run's clock after a failed sync

  NetworkClientSecure c;
  c.setCACert(kRoots);
  c.setHandshakeTimeout(10);
  HTTPClient http;
  get(c, http, "https://api.open-meteo.com/v1/forecast?latitude=34.1&longitude=-118.2&current=temperature_2m,weather_code"
               "&daily=weather_code,temperature_2m_max,temperature_2m_min&temperature_unit=fahrenheit"
               "&timezone=America%2FLos_Angeles&forecast_days=8", "openmeteo");

  http.end();
  c.stop();  // keep-alive left c's TLS session resident; FRED must be measured alone
  heap("after_openmeteo_close");

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
  Serial.flush();
#ifdef SPIKE_STAY_AWAKE
  // Stay up so USB (flashing, logs) is always there; the next reset is chosen over serial.
  Serial.println("SPIKE awake: send 'r' = software reset, 's' = deep sleep 60 s; or tap RESET / power-cycle");
#else
  if (g_runs == 1) {  // the run after the first deep sleep: measure a software reset next
    Serial.println("SPIKE software reset");
    Serial.flush();
    esp_restart();
  }
  Serial.println("SPIKE done: sleeping 60 s (then tap RESET once, then power-cycle once)");
  Serial.flush();
  esp_sleep_enable_timer_wakeup(60ULL * 1000000ULL);
  esp_deep_sleep_start();
#endif
}

void loop() {
#ifdef SPIKE_STAY_AWAKE
  const int c = Serial.read();
  if (c == 'r') {
    Serial.println("SPIKE software reset");
    Serial.flush();
    esp_restart();
  } else if (c == 's') {
    Serial.println("SPIKE deep sleep 60 s");
    Serial.flush();
    esp_sleep_enable_timer_wakeup(60ULL * 1000000ULL);
    esp_deep_sleep_start();
  }
  delay(20);
#endif
}
