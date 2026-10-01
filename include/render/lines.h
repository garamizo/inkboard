#pragma once
// Chart line styles for 1-bit output: width + dash + marker shape (spec §3.3). Port of draw/lines.py.
// Solid lines skip Pillow's joint="curve": Pillow only draws joints for width > 4.
#include <math.h>

#include "render/canvas.h"

namespace ink {

struct LineStyle {
  int width;
  int dash_on, dash_off;  // dash_on == 0: solid
  char marker;            // 's' square, 'c' circle, 't' triangle, 'd' diamond
};

inline constexpr LineStyle STYLES[4] = {{3, 0, 0, 's'}, {2, 7, 4, 'c'}, {1, 0, 0, 't'}, {2, 2, 3, 'd'}};
constexpr double MARKER_STEP = 56;

inline void styled_line(View& d, const Pt* pts, int n, const LineStyle& s) {
  if (n < 2) return;
  if (s.dash_on == 0) return d.line(pts, n, BLACK, s.width);
  const double on = s.dash_on, off = s.dash_off;
  double acc = 0;
  bool drawing = true;
  for (int i = 0; i + 1 < n; ++i) {
    const double x0 = pts[i].x, y0 = pts[i].y, x1 = pts[i + 1].x, y1 = pts[i + 1].y;
    const double seg = hypot(x1 - x0, y1 - y0);
    double t = 0;
    while (t < seg) {
      const double limit = drawing ? on : off;
      const double step = limit - acc < seg - t ? limit - acc : seg - t;
      if (drawing) {
        const double a = t / seg, b = (t + step) / seg;
        d.line(x0 + (x1 - x0) * a, y0 + (y1 - y0) * a, x0 + (x1 - x0) * b, y0 + (y1 - y0) * b, BLACK, s.width);
      }
      t += step;
      acc += step;
      if (acc >= limit) {
        acc = 0;
        drawing = !drawing;
      }
    }
  }
}

inline void draw_marker(View& d, double x, double y, char kind, int r = 4) {
  if (kind == 's') {
    d.rectangle(x - r, y - r, x + r, y + r, BLACK);
  } else if (kind == 'c') {
    d.ellipse(x - r - 1, y - r - 1, x + r + 1, y + r + 1, WHITE, BLACK, 2);
  } else if (kind == 't') {
    const Pt p[] = {{x, y - r - 1}, {x + r + 1, y + r}, {x - r - 1, y + r}};
    d.polygon(p, 3, BLACK);
  } else {
    const Pt p[] = {{x, y - r - 2}, {x + r + 2, y}, {x, y + r + 2}, {x - r - 2, y}};
    d.polygon(p, 4, WHITE, BLACK, 2);
  }
}

inline int marker_xs(double x0, double x1, int k, int n, double* out, int cap, double step = MARKER_STEP) {
  int count = 0;
  for (double x = x0 + step * (k + 0.5) / n; x < x1 - 4 && count < cap; x += step) out[count++] = x;
  return count;
}

inline double y_at(const Pt* pts, int n, double x) {
  if (x <= pts[0].x) return pts[0].y;
  for (int i = 0; i + 1 < n; ++i) {
    const Pt a = pts[i], b = pts[i + 1];
    if (a.x <= x && x <= b.x) return b.x == a.x ? a.y : a.y + (b.y - a.y) * (x - a.x) / (b.x - a.x);
  }
  return pts[n - 1].y;
}

inline void line_with_markers(View& d, const Pt* pts, int n, const LineStyle& s, int k, int count) {
  styled_line(d, pts, n, s);
  if (n == 0) return;
  double xs[32];
  const int m = marker_xs(pts[0].x, pts[n - 1].x, k, count, xs, 32);
  for (int i = 0; i < m; ++i) draw_marker(d, xs[i], y_at(pts, n, xs[i]), s.marker);
  draw_marker(d, pts[n - 1].x, pts[n - 1].y, s.marker);
}

inline void legend_sample(View& d, double x, double y, const LineStyle& s, int length = 24) {
  const Pt p[] = {{x, y}, {x + length, y}};
  styled_line(d, p, 2, s);
  draw_marker(d, x + length / 2.0, y, s.marker);
}

}  // namespace ink
