#pragma once
// Weather-first calendar: current conditions, forecast rows, date and month grid
// (spec §3.3). Port of the removed Python server's widgets/calendar_weather.py.
#include <math.h>
#include <stdio.h>

#include "civil.h"
#include "format.h"
#include "query.h"
#include "render/calendar.h"
#include "render/icons.h"
#include "render/text.h"
#include "sources/openmeteo.h"

namespace ink {

constexpr int WIDGET_H = 464, FOOTER_H = 16;

struct WeatherPayload {
  double temp = 0;
  int16_t code = 0;
  bool has_today = false;
  DayForecast today = {};
  uint8_t n_upcoming = 0;
  DayForecast upcoming[FORECAST_DAYS] = {};
};

// Pick days by date, not position: the cache may still hold yesterday's forecast.
inline WeatherPayload build_weather_payload(const Weather& w, int32_t today) {
  WeatherPayload p;
  p.temp = w.temp;
  p.code = w.code;
  for (int i = 0; i < w.n_daily; ++i) {
    if (w.daily[i].day == today && !p.has_today) {
      p.has_today = true;
      p.today = w.daily[i];
    } else if (w.daily[i].day > today) {
      p.upcoming[p.n_upcoming++] = w.daily[i];
    }
  }
  return p;
}

inline bool weather_payload_ok(const WeatherPayload& p) { return isfinite(p.temp); }

namespace cw_detail {

inline int now_block(View& d, double x, double y, const WeatherPayload& p) {
  const WmoInfo info = wmo_info(p.code);
  const int r = 34;
  draw_icon(d, info.icon, x + r + 4, y + r + 4, r);
  const double tx = x + 2 * r + 18;
  char s[48], a[16], b[16];
  fmt_deg(s, sizeof s, p.temp);
  draw_text(d, tx, y + 2, s, font(46, true), BLACK);
  draw_text(d, tx, y + 54, info.label, font(14), BLACK);
  if (p.has_today) {
    fmt_deg(a, sizeof a, p.today.hi);
    fmt_deg(b, sizeof b, p.today.lo);
    snprintf(s, sizeof s, "H %s  L %s", a, b);
  } else {
    snprintf(s, sizeof s, "H \xE2\x80\x94  L \xE2\x80\x94");
  }
  draw_text(d, tx, y + 72, s, font(14, true), BLACK);
  return 2 * r + 20;
}

inline int rows(View& d, double x, double y, double w, const DayForecast* days, int n) {
  const int rh = 30;
  char s[16];
  for (int k = 0; k < n; ++k) {
    const double yy = y + k * rh + rh / 2.0;
    draw_text(d, x + 4, yy, DAY_ABBR[weekday(days[k].day)], font(15, true), BLACK, "lm");
    draw_icon(d, wmo_info(days[k].code).icon, x + 70, yy, 12);
    fmt_deg(s, sizeof s, days[k].hi);
    draw_text(d, x + w - 50, yy, s, font(15, true), BLACK, "rm");
    fmt_deg(s, sizeof s, days[k].lo);
    draw_text(d, x + w - 4, yy, s, font(15), BLACK, "rm");
    if (k)
      for (int xx = static_cast<int>(x + 4); xx < static_cast<int>(x + w - 4); xx += 4) d.point(xx, yy - rh / 2.0, BLACK);
  }
  return n * rh;
}

}  // namespace cw_detail

inline void render_calendar_weather(View& d, Size size, const WeatherPayload& p, int32_t today) {
  using namespace cw_detail;
  const int pad = 14, bw = d.w(), bh = d.h();
  const Ymd t = civil_from_days(today);
  const int wd = weekday(today);
  char s[48];
  if (size == Size::Third) {
    const int iw = bw - 2 * pad;
    int y = pad;
    y += now_block(d, pad, y, p) + 4;
    y += rows(d, pad, y, iw, p.upcoming, p.n_upcoming < 5 ? p.n_upcoming : 5) + 8;
    d.line(pad, y, bw - pad, y, BLACK);
    y += 8;
    snprintf(s, sizeof s, "%s, %s %d", DAY_ABBR[wd], MONTH_NAME[t.m], t.d);
    draw_text(d, pad, y, s, font(20, true), BLACK);
    month_grid(d, pad, y + 30, iw, today);
  } else {
    const bool full = size == Size::Full;
    const int split = static_cast<int>(bw * (full ? 0.36 : 0.45));
    const int lx = pad, lw = split - 2 * pad;
    const int rx = split + pad, rw = bw - split - 2 * pad;
    d.line(split, pad, split, bh - pad, BLACK);
    int y = pad + 6;
    y += now_block(d, lx, y, p) + 14;
    const int max_rows = full ? 7 : 5;
    rows(d, lx, y, lw, p.upcoming, p.n_upcoming < max_rows ? p.n_upcoming : max_rows);
    y = pad + 6;
    draw_text(d, rx, y, DAY_NAME[wd], font(18, true), BLACK);
    snprintf(s, sizeof s, "%s %d, %d", full ? MONTH_NAME[t.m] : MONTH_ABBR[t.m], t.d, t.y);
    draw_text(d, rx, y + 24, s, font(26, true), BLACK);
    month_grid(d, rx, y + 70, rw, today, true);
  }
}

}  // namespace ink
