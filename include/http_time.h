#pragma once
// HTTP Date parsing and the offline badge's clock text. Pure; host-tested.
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace httptime {

// Days since 1970-01-01 for a proleptic Gregorian date (Howard Hinnant's algorithm).
inline int64_t days_from_civil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

// IMF-fixdate, e.g. "Sun, 27 Sep 2026 19:39:05 GMT" -> unix seconds; -1 if invalid.
inline int64_t parse_http_date(const char* s) {
  if (s == nullptr) return -1;
  int d, y, hh, mm, ss;
  char mon[4] = {0};
  if (sscanf(s, "%*3s, %d %3s %d %d:%d:%d GMT", &d, mon, &y, &hh, &mm, &ss) != 6) return -1;
  static const char* kMonths = "JanFebMarAprMayJunJulAugSepOctNovDec";
  const char* at = strstr(kMonths, mon);
  if (at == nullptr || strlen(mon) != 3 || (at - kMonths) % 3 != 0) return -1;
  unsigned m = static_cast<unsigned>((at - kMonths) / 3 + 1);
  if (d < 1 || d > 31 || hh < 0 || hh > 23 || mm < 0 || mm > 59 || ss < 0 || ss > 60) return -1;
  return days_from_civil(y, m, static_cast<unsigned>(d)) * 86400 + hh * 3600 + mm * 60 + ss;
}

// "7:39 PM" in local time (fixed UTC offset; the board has no tz database).
inline void format_clock(int64_t epoch, int32_t utc_offset_s, char out[9]) {
  int64_t local = epoch + utc_offset_s;
  int64_t sec_of_day = ((local % 86400) + 86400) % 86400;
  int h = static_cast<int>(sec_of_day / 3600);
  int m = static_cast<int>((sec_of_day % 3600) / 60);
  snprintf(out, 9, "%d:%02d %s", h % 12 == 0 ? 12 : h % 12, m, h < 12 ? "AM" : "PM");
}

inline void badge_text(char* out, size_t n, int64_t last_ok_epoch, int32_t utc_offset_s) {
  if (last_ok_epoch <= 0) {
    snprintf(out, n, "offline");
    return;
  }
  char clock[9];
  format_clock(last_ok_epoch, utc_offset_s, clock);
  snprintf(out, n, "offline since %s", clock);
}

}  // namespace httptime
