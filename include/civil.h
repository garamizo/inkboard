#pragma once
// Calendar arithmetic on day numbers (days since 1970-01-01), matching Python's date.
// Pure; host-tested in test/test_data.
#include <stdint.h>

namespace ink {

inline int64_t floor_div(int64_t a, int64_t b) {
  int64_t q = a / b;
  return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}
inline int64_t floor_mod(int64_t a, int64_t b) { return a - floor_div(a, b) * b; }

// Howard Hinnant's algorithms (proleptic Gregorian).
inline int32_t days_from_civil(int y, int m, int d) {
  y -= m <= 2;
  const int32_t era = (y >= 0 ? y : y - 399) / 400;
  const uint32_t yoe = static_cast<uint32_t>(y - era * 400);
  const uint32_t doy = (153 * static_cast<uint32_t>(m + (m > 2 ? -3 : 9)) + 2) / 5 + static_cast<uint32_t>(d) - 1;
  const uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int32_t>(doe) - 719468;
}

struct Ymd {
  int y, m, d;
};

inline Ymd civil_from_days(int32_t z) {
  z += 719468;
  const int32_t era = (z >= 0 ? z : z - 146096) / 146097;
  const uint32_t doe = static_cast<uint32_t>(z - era * 146097);
  const uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const uint32_t mp = (5 * doy + 2) / 153;
  const int d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
  const int m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
  return Ymd{static_cast<int>(yoe) + era * 400 + (m <= 2), m, d};
}

inline int weekday(int32_t day) { return static_cast<int>(floor_mod(day + 3, 7)); }  // Monday = 0

inline bool is_leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

inline int days_in_month(int y, int m) {
  static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  return m == 2 && is_leap(y) ? 29 : kDays[m - 1];
}

// Exactly "YYYY-MM-DD" (FRED and Open-Meteo dates); INT32_MIN otherwise.
inline int32_t parse_iso_date(const char* s) {
  int v[3] = {0, 0, 0};
  const int widths[3] = {4, 2, 2};
  for (int part = 0; part < 3; ++part) {
    for (int i = 0; i < widths[part]; ++i, ++s) {
      if (*s < '0' || *s > '9') return INT32_MIN;
      v[part] = v[part] * 10 + (*s - '0');
    }
    if (part < 2 && *s++ != '-') return INT32_MIN;
  }
  if (*s != '\0' || v[1] < 1 || v[1] > 12 || v[2] < 1 || v[2] > days_in_month(v[0], v[1])) return INT32_MIN;
  return days_from_civil(v[0], v[1], v[2]);
}

inline int32_t first_sunday_on_or_after(int32_t day) { return day + (6 - weekday(day) + 7) % 7; }

}  // namespace ink
