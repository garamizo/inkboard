#pragma once
// Weekly grid, normalization and chart range for market_trends (spec §2.5).
// Port of server/inkboard_server/market_data.py on top of the Sunday cache.
#include <math.h>
#include <stdint.h>

#include "civil.h"
#include "sources/fred.h"

namespace ink {

inline constexpr double TICKS[6] = {0.25, 0.5, 1, 1.5, 2, 3};

inline int32_t window_start_day(int32_t today, int years) {
  return today - static_cast<int32_t>(nearbyint(365.25 * years));  // round() is half-to-even
}

struct Summary {
  int series = 0;
  int n = 0;
  int32_t dates[MAX_POINTS + 1] = {};
  double norm[MAX_POINTS + 1] = {};
  double last = 0, ratio = 0, yoy = 0;
  bool has_yoy = false;
  int32_t last_date = 0, window_start = 0;
  bool short_history = false;
};

// Last observation on or before g, from the Sunday cache plus the latest observation.
inline bool value_on_or_before(const SeriesData& d, int32_t g, double& v) {
  if (d.latest_date == INT32_MIN) return false;
  if (g >= d.latest_date) {
    v = d.latest_value;
    return true;
  }
  if (d.n == 0 || g < d.first_sunday) return false;
  int32_t k = (g - d.first_sunday) / 7;
  if (k >= d.n) k = d.n - 1;
  v = d.values[k];
  return true;
}

inline bool summarize(int series, const SeriesData& d, int32_t today, int years, Summary& out) {
  // Reset in place: a Summary{} temporary is ~8 KB of stack.
  out.n = 0;
  out.has_yoy = false;
  out.short_history = false;
  out.yoy = 0;
  out.series = series;
  const int32_t g0 = first_sunday_on_or_after(window_start_day(today, years));
  double v;
  for (int32_t g = g0; g <= today; g += 7)
    if (value_on_or_before(d, g, v)) {
      out.dates[out.n] = g;
      out.norm[out.n++] = v;
    }
  if (weekday(today) != 6 && value_on_or_before(d, today, v)) {  // weekly_grid appends a non-Sunday today
    out.dates[out.n] = today;
    out.norm[out.n++] = v;
  }
  if (out.n == 0) return false;
  // Same algorithm as Python 3.12's sum() of floats (Neumaier-compensated), same order.
  double sum = 0, comp = 0;
  for (int i = 0; i < out.n; ++i) {
    const double x = out.norm[i], t = sum + x;
    if (fabs(sum) >= fabs(x)) comp += (sum - t) + x;
    else comp += (x - t) + sum;
    sum = t;
  }
  if (comp != 0 && isfinite(comp)) sum += comp;
  const double mean = sum / out.n;
  out.last = out.norm[out.n - 1];
  out.ratio = out.last / mean;
  const int32_t year_ago = today - 364;
  for (int i = 0; i < out.n; ++i)
    if (out.dates[i] <= year_ago) {
      out.has_yoy = true;
      out.yoy = out.last / out.norm[i] - 1;  // overwritten until the last such point
    }
  out.last_date = d.latest_date;
  out.window_start = out.dates[0];
  out.short_history = out.dates[0] > g0 + 31;
  for (int i = 0; i < out.n; ++i) out.norm[i] /= mean;
  return true;
}

inline void y_range(const Summary* const* s, int n, double& lo, double& hi) {
  lo = INFINITY;
  hi = -INFINITY;
  for (int k = 0; k < n; ++k)
    for (int i = 0; i < s[k]->n; ++i) {
      if (s[k]->norm[i] < lo) lo = s[k]->norm[i];
      if (s[k]->norm[i] > hi) hi = s[k]->norm[i];
    }
  lo *= 0.92;
  hi *= 1.08;
}

inline int log_ticks(double lo, double hi, double* out) {
  int n = 0;
  for (double g : TICKS)
    if (lo <= g && g <= hi) out[n++] = g;
  return n;
}

}  // namespace ink
