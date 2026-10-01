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
  snprintf(out, n, "/fred/series/observations?series_id=%s&api_key=%s&file_type=json&sort_order=asc&observation_start=%04d-%02d-%02d",
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
