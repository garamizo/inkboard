#pragma once
// Geometric weather icons and the WMO code mapping (spec §3.3). Port of draw/icons.py.
#include <math.h>
#include <stdint.h>

#include "render/canvas.h"
#include "render/text.h"

namespace ink {

enum class Icon : uint8_t { Sun, Part, Cloud, Fog, Rain, Snow, Storm };

struct WmoInfo {
  const char* label;
  Icon icon;
};

inline WmoInfo wmo_info(int code) {
  if (code == 0) return {"Clear", Icon::Sun};
  if (code == 1) return {"Mostly clear", Icon::Sun};
  if (code == 2) return {"Partly cloudy", Icon::Part};
  if (code == 3) return {"Overcast", Icon::Cloud};
  if (code == 45 || code == 48) return {"Fog", Icon::Fog};
  if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) return {"Rain", Icon::Rain};
  if ((code >= 71 && code <= 77) || code == 85 || code == 86) return {"Snow", Icon::Snow};
  if (code >= 95 && code <= 99) return {"Storms", Icon::Storm};
  return {"\xE2\x80\x94", Icon::Cloud};
}

namespace icon_detail {

inline int max2(int a, int b) { return a > b ? a : b; }

inline void sun(View& d, double cx, double cy, double r) {
  const int w = max2(2, static_cast<int>(r * 0.12));
  d.ellipse(cx - r * .45, cy - r * .45, cx + r * .45, cy + r * .45, NONE, BLACK, w);
  for (int k = 0; k < 8; ++k) {
    const double a = k * M_PI / 4;
    d.line(cx + cos(a) * r * .65, cy + sin(a) * r * .65, cx + cos(a) * r * .95, cy + sin(a) * r * .95, BLACK,
           max2(2, static_cast<int>(r * .1)));
  }
}

inline void cloud(View& d, double cx, double cy, double r) {
  const int w = max2(2, static_cast<int>(r * .1));
  const double parts[3][3] = {{cx - r * .45, cy + r * .1, r * .38}, {cx, cy - r * .15, r * .5},
                              {cx + r * .45, cy + r * .12, r * .36}};
  for (const auto& p : parts) d.ellipse(p[0] - p[2], p[1] - p[2], p[0] + p[2], p[1] + p[2], NONE, BLACK, w);
  for (const auto& p : parts) d.ellipse(p[0] - p[2] + w, p[1] - p[2] + w, p[0] + p[2] - w, p[1] + p[2] - w, WHITE);
  d.rectangle(cx - r * .45, cy + r * .1, cx + r * .45, cy + r * .48 - w, WHITE);
  d.line(cx - r * .45, cy + r * .48, cx + r * .45, cy + r * .48, BLACK, w);
}

}  // namespace icon_detail

inline void draw_icon(View& d, Icon kind, double cx, double cy, double r) {
  using namespace icon_detail;
  const int lw = max2(2, static_cast<int>(r * .1));
  switch (kind) {
    case Icon::Sun: return sun(d, cx, cy, r);
    case Icon::Part:
      sun(d, cx - r * .3, cy - r * .3, r * .7);
      return cloud(d, cx + r * .1, cy + r * .15, r * .8);
    case Icon::Cloud: return cloud(d, cx, cy, r);
    case Icon::Rain:
    case Icon::Snow:
    case Icon::Storm:
      cloud(d, cx, cy - r * .2, r * .85);
      for (int k = 0; k < 3; ++k) {
        const double x = cx - r * .35 + k * r * .35;
        if (kind == Icon::Snow) {
          draw_text(d, x, cy + r * .55, "*", font(max2(8, static_cast<int>(r * .5)), true), BLACK, "mm");
        } else if (kind == Icon::Storm && k == 1) {
          const Pt bolt[] = {{x + r * .1, cy + r * .3}, {x - r * .1, cy + r * .55}, {x + r * .1, cy + r * .55},
                             {x - r * .1, cy + r * .85}};
          d.line(bolt, 4, BLACK, lw);
        } else {
          d.line(x, cy + r * .35, x - r * .12, cy + r * .75, BLACK, lw);
        }
      }
      return;
    case Icon::Fog:
      for (int k = 0; k < 4; ++k) {
        const double y = cy - r * .4 + k * r * .27;
        d.line(cx - r * .7, y, cx + r * .7, y, BLACK, lw);
      }
      return;
  }
}

}  // namespace ink
