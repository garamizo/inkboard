#pragma once
// Market trends: several series on one log chart, each divided by its own window mean
// (spec §3.3). Port of server/inkboard_server/widgets/market_trends.py.
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "civil.h"
#include "format.h"
#include "market_data.h"
#include "query.h"
#include "render/lines.h"
#include "render/text.h"
#include "series.h"

namespace ink {

struct MarketPayload {
  int n;
  const Summary* series[4];
  int years;
  int32_t today;
};

inline bool market_payload_ok(const MarketPayload& p) {
  if (p.n < 1 || p.n > 4) return false;
  for (int i = 0; i < p.n; ++i)
    if (p.series[i] == nullptr || p.series[i]->n < 1) return false;
  // A bad cache must show "error: market_trends", not feed NaN/inf to the integer casts in the chart.
  for (int i = 0; i < p.n; ++i)
    for (int k = 0; k < p.series[i]->n; ++k)
      if (!isfinite(p.series[i]->norm[k]) || p.series[i]->norm[k] <= 0) return false;
  double lo, hi;
  y_range(p.series, p.n, lo, hi);
  return isfinite(lo) && isfinite(hi) && lo > 0 && hi > lo;
}

namespace mt_detail {

inline Pt g_pts[MAX_POINTS + 1];  // one series at a time; static so the board's stack stays small

inline void chart(View& d, const MarketPayload& p, double left, double top, double right, double bottom, bool narrow) {
  double lo, hi;
  y_range(p.series, p.n, lo, hi);
  const double llo = log(lo), lhi = log(hi);
  int32_t start = p.series[0]->dates[0];
  for (int k = 1; k < p.n; ++k)
    if (p.series[k]->dates[0] < start) start = p.series[k]->dates[0];
  const int32_t span = p.today - start > 1 ? p.today - start : 1;
  auto X = [&](int32_t day) { return left + static_cast<double>(day - start) / span * (right - left); };
  auto Y = [&](double v) { return bottom - (log(v) - llo) / (lhi - llo) * (bottom - top); };
  const Font& axis = font(11);
  double ticks[6];
  char s[16];
  const int nt = log_ticks(lo, hi, ticks);
  for (int i = 0; i < nt; ++i) {
    const double y = Y(ticks[i]);
    if (ticks[i] == 1) d.line(left, y, right, y, BLACK, 2);
    else
      for (int x = static_cast<int>(left); x < static_cast<int>(right); x += 6) d.point(x, y, BLACK);
    snprintf(s, sizeof s, "%g\xC3\x97", ticks[i]);
    draw_text(d, left - 4, y, s, axis, BLACK, "rm");
  }
  d.line(left, bottom, right, bottom, BLACK);
  const int y0 = civil_from_days(start).y, y1 = civil_from_days(p.today).y;
  for (int yr = y0 + 1; yr <= y1; ++yr) {
    const double x = X(days_from_civil(yr, 1, 1));
    d.line(x, bottom, x, bottom + 4, BLACK);
    if (narrow) snprintf(s, sizeof s, "'%02d", yr % 100);
    else snprintf(s, sizeof s, "%d", yr);
    draw_text(d, x, bottom + 6, s, axis, BLACK, "mt");
  }
  for (int k = 0; k < p.n; ++k) {
    const Summary& sm = *p.series[k];
    for (int i = 0; i < sm.n; ++i) g_pts[i] = Pt{X(sm.dates[i]), Y(sm.norm[i])};
    line_with_markers(d, g_pts, sm.n, STYLES[k], k, p.n);
  }
}

inline void table(View& d, const MarketPayload& p, double x0, double ty, double x1, int row_h, bool narrow) {
  const Font& value_font = font(narrow ? 12 : 13);
  const Font& name_font = font(narrow ? 12 : 13, true);
  const Font& head_font = font(11);
  const double cols3[] = {x1 - 150, x1 - 70, x1}, cols2[] = {x1 - 60, x1};
  const double* cols = narrow ? cols2 : cols3;
  const int ncol = narrow ? 2 : 3;
  const char* heads[] = {"now", "\xC3\x97" "avg", "1 yr"};
  for (int i = 0; i < ncol; ++i) draw_text(d, cols[i], ty - 4, heads[i], head_font, BLACK, "rb");
  d.line(x0, ty, x1, ty, BLACK);
  char name[64], v[3][24];
  for (int k = 0; k < p.n; ++k) {
    const Summary& s = *p.series[k];
    const SeriesDef& sd = CATALOG[s.series];
    const double y = ty + 12 + k * row_h;
    legend_sample(d, x0, y, STYLES[k]);
    snprintf(name, sizeof name, "%s", narrow ? sd.short_label : sd.label);
    if (s.short_history && !narrow)
      snprintf(name + strlen(name), sizeof name - strlen(name), " (since %d)", civil_from_days(s.window_start).y);
    draw_text(d, x0 + 32, y, name, name_font, BLACK, "lm");
    format_value(sd.fmt, s.last, v[0], sizeof v[0]);
    snprintf(v[1], sizeof v[1], "%.2f\xC3\x97", s.ratio);
    if (s.has_yoy) snprintf(v[2], sizeof v[2], "%+.0f%%", s.yoy * 100);
    else snprintf(v[2], sizeof v[2], "\xE2\x80\x94");
    for (int i = 0; i < ncol; ++i) draw_text(d, cols[i], y, v[i], value_font, BLACK, "rm");
  }
}

}  // namespace mt_detail

inline void render_market_trends(View& d, Size size, const MarketPayload& p) {
  const bool narrow = size == Size::Third;
  const int pad = 12, bw = d.w(), bh = d.h();
  draw_text(d, pad, pad, "Markets", font(narrow ? 18 : 22, true), BLACK);
  char s[64];
  if (narrow) snprintf(s, sizeof s, "%d-yr \xC2\xB7 \xC3\x97" "avg \xC2\xB7 log", p.years);
  else snprintf(s, sizeof s, "%d-yr, \xC3\x97 own average, log", p.years);
  draw_text(d, pad, pad + (narrow ? 23 : 28), s, font(12), BLACK);
  int32_t asof = p.series[0]->last_date;
  for (int k = 1; k < p.n; ++k)
    if (p.series[k]->last_date < asof) asof = p.series[k]->last_date;
  const Ymd a = civil_from_days(asof);
  snprintf(s, sizeof s, "as of %s %d", MONTH_ABBR[a.m], a.d);
  draw_text(d, bw - pad, pad + 4, s, font(11), BLACK, "ra");
  const int row_h = narrow ? 20 : 22;
  const int table_h = p.n * row_h + 36;
  const int top = pad + 50, bottom = bh - pad - 18 - table_h;
  mt_detail::chart(d, p, pad + 30, top, bw - pad - 6, bottom, narrow);
  mt_detail::table(d, p, pad, bottom + 40, bw - pad, row_h, narrow);
}

}  // namespace ink
