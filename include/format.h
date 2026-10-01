#pragma once
// Python-compatible text for the widgets (spec §3.3): round-half-even like round(),
// "{:,.0f}" thousands separators, English names (never the C locale).
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace ink {

inline const char* const DAY_ABBR[7] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
inline const char* const DAY_NAME[7] = {"Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"};
inline const char* const MONTH_ABBR[13] = {"", "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                           "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
inline const char* const MONTH_NAME[13] = {"", "January", "February", "March", "April", "May", "June",
                                           "July", "August", "September", "October", "November", "December"};

// deg() in calendar_weather.py: int(round(v)) with banker's rounding, never "-0°".
inline void fmt_deg(char* out, size_t n, double v) {
  long r = lrint(v);  // default rounding mode is to-nearest-even, like Python's round()
  snprintf(out, n, "%ld\xC2\xB0", r);
}

// f"{v:,.0f}"
inline void fmt_thousands0(char* out, size_t n, double v) {
  char raw[48];
  snprintf(raw, sizeof raw, "%.0f", v);
  const char* digits = raw[0] == '-' ? raw + 1 : raw;
  size_t len = strlen(digits);
  char tmp[64];
  size_t k = 0;
  if (raw[0] == '-') tmp[k++] = '-';
  for (size_t i = 0; i < len; ++i) {
    if (i > 0 && (len - i) % 3 == 0) tmp[k++] = ',';
    tmp[k++] = digits[i];
  }
  tmp[k] = '\0';
  snprintf(out, n, "%s", tmp);
}

// compositor.py footer_time_text: "10:00 AM"
inline void fmt_clock(char* out, size_t n, int hh, int mm) {
  snprintf(out, n, "%d:%02d %s", hh % 12 == 0 ? 12 : hh % 12, mm, hh < 12 ? "AM" : "PM");
}

}  // namespace ink
