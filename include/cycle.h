#pragma once
// One wake (spec §4.2): config, caches, network for what is due, render, show if changed,
// sleep. Generic over the hardware (`Ops`) so the ordering and failure paths are host-tested;
// src/main.cpp supplies the board's Ops.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

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
  bool loaded;  // caches read from flash since boot
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

// In place: `x = SeriesCache{}` builds a ~4.3 KB temporary, too much for the board's 8 KB stack.
inline void reset(ink::SeriesCache& c) {
  c.valid = false;
  c.fred_id[0] = '\0';
  c.data.first_sunday = INT32_MIN;
  c.data.n = 0;
  c.data.latest_date = INT32_MIN;
  c.data.latest_value = 0;
  c.data.covered_from = INT32_MIN;
  c.full_fetched_at = 0;
  c.status = ink::SourceStatus{};
}

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

// Flash is read once per boot: while the board stays awake (USB, dev builds) RAM is newer,
// e.g. after a fetch whose save failed.
template <class Ops>
void load_caches(Ops& ops, Work& w, bool fs) {
  if (w.loaded) return;
  w.loaded = true;
  ink::Model& m = w.model;
  m.weather = ink::WeatherCache{};
  for (auto& s : m.series) reset(s);
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
      reset(m.series[i]);
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
