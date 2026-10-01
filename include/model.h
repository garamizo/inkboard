#pragma once
// Everything one render needs, and the step from cached data to a frame (spec §4.2 step 6):
// the per-column states of compositor.py's fetch_widgets, computed from the caches.
#include <stdio.h>

#include "compositor.h"
#include "data_cache.h"
#include "market_data.h"
#include "query.h"
#include "tz.h"

namespace ink {

struct Model {
  Layout layout;
  int64_t now = 0;
  WeatherCache weather;
  SeriesCache series[4];
};

inline WeatherKey weather_key(const Layout& l) {
  WeatherKey k{l.lat, l.lon, l.metric, ""};
  snprintf(k.tz, sizeof k.tz, "%s", l.tz);
  return k;
}

namespace model_detail {
inline Summary g_summaries[4];   // ~6 KB each: static, never on the board's stack
inline WeatherPayload g_weather;
inline MarketPayload g_market;
}  // namespace model_detail

inline void build_frame(Bitmap& frame, const Model& m, const char* version) {
  using namespace model_detail;
  const Layout& l = m.layout;
  const int32_t today = tz::to_local(l.zone, m.now).day;

  ColumnData weather_col, market_col;
  if (l.has_weather) {
    if (!weather_usable(m.weather, weather_key(l))) {
      weather_col.state = ColumnState::NoData;
      weather_col.nodata_source = "weather";
    } else {
      g_weather = build_weather_payload(m.weather.data, today);
      weather_col.state = weather_payload_ok(g_weather) ? ColumnState::Ok : ColumnState::RenderError;
      weather_col.weather = &g_weather;
      weather_col.today = today;
      weather_col.has_fetched_at = true;
      weather_col.fetched_at = m.weather.status.fetched_at;
      weather_col.stale = is_stale(m.weather.status, m.now, WEATHER_TTL_S);
    }
  }
  if (l.has_market) {
    market_col.state = ColumnState::Ok;
    g_market = MarketPayload{l.n_series, {}, l.years, today};
    for (int i = 0; i < l.n_series && market_col.state == ColumnState::Ok; ++i) {
      const SeriesCache& c = m.series[i];
      if (!c.valid || c.data.latest_date == INT32_MIN) {
        market_col.state = c.status.auth_rejected ? ColumnState::AuthRejected : ColumnState::NoData;
        market_col.nodata_source = "fred";
      } else if (!summarize(l.series[i], c.data, today, l.years, g_summaries[i])) {
        market_col.state = ColumnState::NoData;      // summarize() raised in Python: FetchFailure(type_name)
        market_col.nodata_source = "market_trends";
      } else {
        g_market.series[i] = &g_summaries[i];
        if (!market_col.has_fetched_at || c.status.fetched_at > market_col.fetched_at)
          market_col.fetched_at = c.status.fetched_at;
        market_col.has_fetched_at = true;
        market_col.stale = market_col.stale || is_stale(c.status, m.now, FRED_TTL_S);
      }
    }
    if (market_col.state == ColumnState::Ok) {
      market_col.market = &g_market;
      if (!market_payload_ok(g_market)) market_col.state = ColumnState::RenderError;
    }
  }
  ColumnData cols[3];
  for (int i = 0; i < l.n_columns; ++i)
    cols[i] = l.columns[i].type == WidgetType::CalendarWeather ? weather_col : market_col;
  compose(frame, l, cols, l.zone, version);
}

}  // namespace ink
