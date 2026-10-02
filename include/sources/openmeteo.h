#pragma once
// Open-Meteo forecast: request path and streaming parser (spec §2.4).
// Port of the removed Python server's sources/openmeteo.py.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ctype.h>
#include <math.h>

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
    const unsigned char u = static_cast<unsigned char>(*c);
    if (isalnum(u) || u == '_' || u == '-' || u == '.') tzq[k++] = *c;
    else k += static_cast<size_t>(snprintf(tzq + k, sizeof tzq - k, "%%%02X", u));  // '/', and '+' in Etc/GMT+5
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
      temp_ = num ? strtod(s, nullptr) : NAN;
      has_temp_ = isfinite(temp_);
      return;
    }
    if (p.at({"current", "weather_code"})) {
      const double c = num ? strtod(s, nullptr) : -1;
      has_code_ = c >= 0 && c <= 32767;  // WMO codes are 0-99; never cast garbage
      code_ = static_cast<int16_t>(has_code_ ? c : 0);
      return;
    }
    if (p.depth() != 3 || p.is_array(1) || !p.is_array(2) || strcmp(p.key(0), "daily") != 0) return;
    const int col = column(p.key(1));
    const int32_t i = p.index(2);
    if (col < 0 || i >= FORECAST_DAYS) return;
    if (i + 1 > len_[col]) len_[col] = i + 1;
    if (col == 0) {
      ok_[0][i] = t == json::Type::String;
      date_[i] = ok_[0][i] ? parse_iso_date(s) : INT32_MIN;
    } else {
      val_[col][i] = num ? strtod(s, nullptr) : NAN;
      ok_[col][i] = isfinite(val_[col][i]) && (col != 1 || (val_[col][i] >= 0 && val_[col][i] <= 32767));
    }
  }

  void end_container(const json::Parser& p) override {
    if (p.depth() == 2 && strcmp(p.key(0), "daily") == 0 && !p.is_array(1)) {
      const int col = column(p.key(1));
      if (col >= 0 && p.closed_array()) seen_ |= 1 << col;
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
