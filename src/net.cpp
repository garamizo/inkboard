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

// Feeds the body straight into the JSON tokenizer, so nothing is buffered (spec §2.6), and
// enforces the wake's deadline: a short write makes writeToStream() stop with an error.
// writeToStream() wants a Stream; only the write side is used.
class ParserSink : public Stream {
 public:
  ParserSink(ink::json::Parser& p, uint32_t deadline_ms) : p_(p), deadline_(deadline_ms) {}
  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t* b, size_t n) override {
    if (static_cast<int32_t>(millis() - deadline_) >= 0) return 0;
    return p_.feed(reinterpret_cast<const char*>(b), n) ? n : 0;
  }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}

 private:
  ink::json::Parser& p_;
  uint32_t deadline_;
};

// One TLS client and one HTTPClient per host, kept across requests so the FRED series share a
// keep-alive session. HTTPClient's destructor calls stop() on its client, so the HTTPClient
// must outlive every request and be destroyed before the client.
static NetworkClientSecure* g_client = nullptr;
static HTTPClient* g_http = nullptr;
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
  delete g_http;  // stops the connection through g_client, which is still alive here
  g_http = nullptr;
  delete g_client;
  g_client = nullptr;
  g_host = "";
}

void net_wifi_off() {
  net_close();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

// sntp_get_sync_status() reports COMPLETED once and then resets, so it is read once per check.
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
  // The cycle passes what is left of its budget; a 0 timeout could mean "no timeout" below.
  if (timeout_ms < 1000) return r;
  const uint32_t deadline = millis() + timeout_ms;
  if (g_client == nullptr || g_host != host) {
    net_close();
    g_client = new NetworkClientSecure;
    g_client->setCACert(CA_BUNDLE_PEM);
    g_http = new HTTPClient;
    g_http->setReuse(true);
    g_http->setUserAgent("inkboard/" INKBOARD_VERSION);
    g_host = host;
  }
  // Connect, TLS and each wait for data are bounded by what is left of the budget (at most
  // 8/10/10 s); the body as a whole by the deadline in ParserSink.
  const uint32_t step = timeout_ms < 10000 ? timeout_ms : 10000;
  g_client->setHandshakeTimeout(step / 1000);
  g_http->setConnectTimeout(step < 8000 ? step : 8000);
  g_http->setTimeout(step);
  static const char* keys[] = {"Date", "Retry-After"};
  g_http->collectHeaders(keys, 2);
  if (!g_http->begin(*g_client, host, 443, path, true)) return r;
  r.http_status = g_http->GET();
  r.date_epoch = httptime::parse_http_date(g_http->header("Date").c_str());
  r.retry_after_s = wake::parse_seconds(g_http->header("Retry-After").c_str());
  if (r.http_status == 200 || r.http_status == 400 || r.http_status == 403) {  // 4xx bodies explain a bad key
    ink::json::Parser p(h);
    ParserSink sink(p, deadline);
    const int size = g_http->getSize();
    const int written = g_http->writeToStream(&sink);
    r.complete = written >= 0 && (size < 0 || written == size) && p.finish();
  }
  g_http->end();
  if (!r.complete) net_close();  // a broken or half-read session must not be reused
  return r;
}
