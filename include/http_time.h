#pragma once
// HTTP Date parsing: the clock fallback when SNTP fails (spec §2.1). Pure; host-tested.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "civil.h"

namespace httptime {

// IMF-fixdate, e.g. "Sun, 27 Sep 2026 19:39:05 GMT" -> unix seconds; -1 if invalid.
inline int64_t parse_http_date(const char* s) {
  if (s == nullptr) return -1;
  int d, y, hh, mm, ss;
  char mon[4] = {0};
  if (sscanf(s, "%*3s, %d %3s %d %d:%d:%d GMT", &d, mon, &y, &hh, &mm, &ss) != 6) return -1;
  static const char* kMonths = "JanFebMarAprMayJunJulAugSepOctNovDec";
  const char* at = strstr(kMonths, mon);
  if (at == nullptr || strlen(mon) != 3 || (at - kMonths) % 3 != 0) return -1;
  const int m = static_cast<int>((at - kMonths) / 3 + 1);
  if (d < 1 || d > 31 || hh < 0 || hh > 23 || mm < 0 || mm > 59 || ss < 0 || ss > 60) return -1;
  return int64_t(ink::days_from_civil(y, m, d)) * 86400 + hh * 3600 + mm * 60 + ss;
}

}  // namespace httptime
