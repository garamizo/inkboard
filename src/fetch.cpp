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
#if !__has_include("secrets.h")
#error "Copy include/secrets.h.example to include/secrets.h and put your Wi-Fi credentials in it."
#endif
#include "secrets.h"

// Refuse to build firmware that could never join Wi-Fi (works for every build path: just, pio, IDE).
static constexpr bool same_text(const char* a, const char* b) {
  return *a == *b && (*a == '\0' || same_text(a + 1, b + 1));
}
static_assert(!same_text(WIFI_SSID, "your-network"), "Put your Wi-Fi credentials in include/secrets.h");

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
