#pragma once
// FRAME_QUERY -> Layout (spec §1.1). Port of server/inkboard_server/query.py and
// widgets/params.py with the same one-line messages (they reach the config error screen).
// Deviations: Python's float()/int() also accept "_" digit separators; these parsers don't.
// A decoded NUL byte, a key over 31 bytes or a value over 255 bytes is "malformed query string".
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
// false if the result holds a NUL byte or does not fit: a shortened value must never validate.
inline bool unquote(const char* s, size_t n, char* out, size_t cap) {
  size_t k = 0;
  for (size_t i = 0; i < n; ++i) {
    if (k + 1 >= cap) return false;
    char c = s[i];
    if (c == '+') {
      c = ' ';
    } else if (c == '%' && i + 2 < n && hexval(s[i + 1]) >= 0 && hexval(s[i + 2]) >= 0) {
      c = static_cast<char>(hexval(s[i + 1]) * 16 + hexval(s[i + 2]));
      i += 2;
    }
    if (c == '\0') return false;
    out[k++] = c;
  }
  out[k] = '\0';
  return true;
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
      if (!unquote(p, static_cast<size_t>(eq - p), pairs[n_pairs].key, sizeof pairs[n_pairs].key) ||
          !unquote(eq + 1, flen - static_cast<size_t>(eq + 1 - p), pairs[n_pairs].value, sizeof pairs[n_pairs].value))
        return err(error, error_n, "malformed query string"), false;
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
      const char* p = s;
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
