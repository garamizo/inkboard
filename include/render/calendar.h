#pragma once
// Month grid, Sunday first, today inverted (spec §3.3). Port of draw/calendar.py.
#include <stdio.h>

#include "civil.h"
#include "render/canvas.h"
#include "render/text.h"

namespace ink {

// calendar.Calendar(firstweekday=6).monthdayscalendar(y, m)
inline int month_weeks(int y, int m, int weeks[6][7]) {
  const int lead = (weekday(days_from_civil(y, m, 1)) + 1) % 7;
  const int n = days_in_month(y, m);
  const int rows = (lead + n + 6) / 7;
  for (int r = 0; r < rows; ++r)
    for (int c = 0; c < 7; ++c) {
      const int day = r * 7 + c - lead + 1;
      weeks[r][c] = day >= 1 && day <= n ? day : 0;
    }
  return rows;
}

inline int month_grid(View& d, double x, double y, double w, int32_t today, bool big = false) {
  const Ymd t = civil_from_days(today);
  int weeks[6][7];
  const int rows = month_weeks(t.y, t.m, weeks);
  const double cw = w / 7, ch = big ? 26 : 22;
  const Font& head = font(big ? 12 : 11, true);
  const Font& num = font(big ? 15 : 13);
  const Font& num_bold = font(big ? 15 : 13, true);
  const char* letters[] = {"S", "M", "T", "W", "T", "F", "S"};
  for (int i = 0; i < 7; ++i) draw_text(d, x + cw * i + cw / 2, y + ch / 2, letters[i], head, BLACK, "mm");
  d.line(x + 4, y + ch, x + w - 4, y + ch, BLACK);
  char s[4];
  for (int r = 0; r < rows; ++r)
    for (int c = 0; c < 7; ++c) {
      const int day = weeks[r][c];
      if (!day) continue;
      const double cx = x + cw * c + cw / 2, cy = y + ch * (r + 1) + ch / 2 + 2;
      snprintf(s, sizeof s, "%d", day);
      if (day == t.d) {
        d.rounded_rectangle(cx - cw / 2 + 3, cy - ch / 2 + 1, cx + cw / 2 - 3, cy + ch / 2 - 1, 4, BLACK);
        draw_text(d, cx, cy, s, num_bold, WHITE, "mm");
      } else {
        draw_text(d, cx, cy, s, num, BLACK, "mm");
      }
    }
  return static_cast<int>(ch * (rows + 1) + 4);
}

}  // namespace ink
