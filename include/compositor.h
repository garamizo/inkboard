#pragma once
// Columns, dividers, footer and per-column failure messages (spec §3.3), plus the config
// error screen and the calibration pattern. Port of compositor.py and frame.py.
#include <stdio.h>
#include <string.h>

#include "format.h"
#include "query.h"
#include "render/canvas.h"
#include "render/text.h"
#include "series.h"
#include "tz.h"
#include "widgets/calendar_weather.h"
#include "widgets/market_trends.h"

namespace ink {

enum class ColumnState : uint8_t { Ok, NoData, AuthRejected, RenderError };

struct ColumnData {
  ColumnState state = ColumnState::NoData;
  const char* nodata_source = "";
  const WeatherPayload* weather = nullptr;
  const MarketPayload* market = nullptr;
  int32_t today = 0;
  bool has_fetched_at = false;
  int64_t fetched_at = 0;
  bool stale = false;
};

namespace comp_detail {

inline void message(View& full, const Box& box, const char* text, double dy = 0) {
  draw_text(full, box.x + box.w / 2.0, box.y + box.h / 2.0 + dy, text, font(14, true), BLACK, "mm");
}

inline void add_unique(const char** list, int& n, const char* a) {
  for (int i = 0; i < n; ++i)
    if (strcmp(list[i], a) == 0) return;
  list[n++] = a;
}

}  // namespace comp_detail

inline void compose(Bitmap& frame, const Layout& l, const ColumnData* cols, const tz::Rule& zone, const char* version) {
  using namespace comp_detail;
  frame.fill(WHITE);
  View full(frame, Box{0, 0, FRAME_W, FRAME_H});
  int x = 0, dividers[3], nd = 0;
  char text[64];
  for (int i = 0; i < l.n_columns; ++i) {
    const Box box{x, 0, size_width(l.columns[i].size), WIDGET_H};
    const ColumnData& c = cols[i];
    if (c.state == ColumnState::NoData) {
      snprintf(text, sizeof text, "No data yet: %s", c.nodata_source);
      message(full, box, text);
    } else if (c.state == ColumnState::AuthRejected) {
      message(full, box, "FRED key rejected:", -10);
      message(full, box, "check secrets.h", 10);
    } else if (c.state == ColumnState::RenderError) {
      snprintf(text, sizeof text, "error: %s", widget_name(l.columns[i].type));
      message(full, box, text);
    } else {
      View v(frame, box);
      if (l.columns[i].type == WidgetType::CalendarWeather)
        render_calendar_weather(v, l.columns[i].size, *c.weather, c.today);
      else
        render_market_trends(v, l.columns[i].size, *c.market);
    }
    if (x) dividers[nd++] = x;
    x += box.w;
  }
  for (int i = 0; i < nd; ++i) full.line(dividers[i], 8, dividers[i], WIDGET_H - 8, BLACK);

  // Footer (compositor.py _footer)
  full.line(0, WIDGET_H, FRAME_W, WIDGET_H, BLACK);
  bool any_time = false, stale = false;
  int64_t newest = 0;
  for (int i = 0; i < l.n_columns; ++i) {
    // A column whose render failed still fetched its data (compositor.py counts it too).
    if (cols[i].state != ColumnState::Ok && cols[i].state != ColumnState::RenderError) continue;
    stale = stale || cols[i].stale;
    if (cols[i].has_fetched_at && (!any_time || cols[i].fetched_at > newest)) newest = cols[i].fetched_at;
    any_time = any_time || cols[i].has_fetched_at;
  }
  char left[128] = "updated \xE2\x80\x94";
  if (any_time) {
    const tz::Local t = tz::to_local(zone, newest);
    char clock[16];
    fmt_clock(clock, sizeof clock, t.hh, t.mm);
    snprintf(left, sizeof left, "updated %s", clock);
  }
  if (stale) strncat(left, "   \xE2\x9A\xA0 stale", sizeof left - strlen(left) - 1);
  if (version && *version) {
    strncat(left, "   ", sizeof left - strlen(left) - 1);
    strncat(left, version, sizeof left - strlen(left) - 1);
  }
  const int y = WIDGET_H + FOOTER_H / 2;
  draw_text(full, 8, y, left, font(10), BLACK, "lm");
  const char* attrs[16];
  int na = 0;
  for (int i = 0; i < l.n_columns; ++i) {
    if (l.columns[i].type == WidgetType::MarketTrends) {
      for (int k = 0; k < l.n_series; ++k)
        for (const char* a : CATALOG[l.series[k]].attribution)
          if (a) add_unique(attrs, na, a);
    } else {
      add_unique(attrs, na, "Open-Meteo");
    }
  }
  char right[160] = "";
  for (int i = 0; i < na; ++i) {
    if (i) strncat(right, " \xC2\xB7 ", sizeof right - strlen(right) - 1);
    strncat(right, attrs[i], sizeof right - strlen(right) - 1);
  }
  draw_text(full, FRAME_W - 8, y, right, font(10), BLACK, "rm");
}

// Word-wrapped text block; returns the y after the last line.
inline int wrapped(View& d, int x, int y, int width, const char* text, const Font& f, int line_h) {
  char line[512] = "";
  const char* p = text;
  while (*p) {
    const char* sp = strchr(p, ' ');
    const size_t n = sp ? static_cast<size_t>(sp - p) : strlen(p);
    char trial[512];
    snprintf(trial, sizeof trial, "%s%s%.*s", line, line[0] ? " " : "", static_cast<int>(n), p);
    if (line[0] && text_length(f, trial) > width) {
      draw_text(d, x, y, line, f, BLACK);
      y += line_h;
      snprintf(line, sizeof line, "%.*s", static_cast<int>(n), p);
    } else {
      snprintf(line, sizeof line, "%s", trial);
    }
    p += n;
    while (*p == ' ') ++p;
  }
  if (line[0]) {
    draw_text(d, x, y, line, f, BLACK);
    y += line_h;
  }
  return y;
}

inline void render_config_error(Bitmap& frame, const char* message, const char* query) {
  frame.fill(WHITE);
  View d(frame, Box{0, 0, FRAME_W, FRAME_H});
  draw_text(d, 24, 36, "inkboard config error", font(22, true), BLACK);
  wrapped(d, 24, 90, FRAME_W - 48, message, font(15), 22);
  draw_text(d, 24, 360, "FRAME_QUERY in include/config.h:", font(13, true), BLACK);
  // Queries have no spaces: wrap by characters at 100 per line.
  char chunk[101];
  int y = 384;
  for (size_t off = 0; off < strlen(query) && y < FRAME_H - 16; off += 100, y += 18) {
    snprintf(chunk, sizeof chunk, "%.100s", query + off);
    draw_text(d, 24, y, chunk, font(12), BLACK);
  }
}

// frame.py calibration_pattern(): asymmetric marks so rotation, mirroring and inversion show.
inline void render_calibration(Bitmap& frame) {
  frame.fill(WHITE);
  View d(frame, Box{0, 0, FRAME_W, FRAME_H});
  d.rectangle(0, 0, 39, 39, BLACK);
  const Pt tr[] = {{FRAME_W - 40.0, 1}, {FRAME_W - 2.0, 1}, {FRAME_W - 2.0, 39}};
  d.line(tr, 3, BLACK, 4);
  const Pt bl[] = {{1, FRAME_H - 40.0}, {1, FRAME_H - 2.0}, {39, FRAME_H - 2.0}};
  d.line(bl, 3, BLACK, 4);
  d.ellipse(FRAME_W - 40, FRAME_H - 40, FRAME_W - 1, FRAME_H - 1, BLACK);
  draw_text(d, 52, 10, "TOP LEFT (solid square)", font(20, true), BLACK);
  draw_text(d, FRAME_W - 52, FRAME_H - 10, "BOTTOM RIGHT (dot)", font(20, true), BLACK, "rb");
  draw_text(d, FRAME_W / 2, 110, "inkboard calibration", font(36, true), BLACK, "mm");
  draw_text(d, FRAME_W / 2, 155, "Black text on white means the invert setting is right.", font(18), BLACK, "mm");
  for (int y = 200; y < 400; y += 8)
    for (int x = 200; x < 600; x += 8)
      if (((x - 200) / 8 + (y - 200) / 8) % 2 == 0) d.rectangle(x, y, x + 7, y + 7, BLACK);
}

}  // namespace ink
