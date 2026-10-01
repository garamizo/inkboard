#pragma once
// Market series a board can show (spec §1.1). Port of server/inkboard_server/series.py.
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "format.h"

namespace ink {

enum class ValueFmt : uint8_t { Thousands0, KiloDollars1, Percent2, MegaDollars2, Fixed1 };

struct SeriesDef {
  const char* id;
  const char* fred_id;
  const char* label;
  const char* short_label;
  ValueFmt fmt;
  const char* attribution[3];
};

inline constexpr SeriesDef CATALOG[] = {
    {"sp500", "SP500", "S&P 500", "S&P", ValueFmt::Thousands0, {"FRED", "S&P DJI", nullptr}},
    {"btc", "CBBTCUSD", "Bitcoin", "BTC", ValueFmt::KiloDollars1, {"FRED", "Coinbase", nullptr}},
    {"mortgage30", "MORTGAGE30US", "Mortgage", "Mort", ValueFmt::Percent2, {"FRED", "Freddie Mac", nullptr}},
    {"home_la", "MEDLISPRI31080", "LA home", "Home", ValueFmt::MegaDollars2, {"FRED", "Realtor.com", nullptr}},
    {"ust10y", "DGS10", "10-yr Treasury", "10y", ValueFmt::Percent2, {"FRED", nullptr, nullptr}},
    {"usd_broad", "DTWEXBGS", "Dollar index", "USD", ValueFmt::Fixed1, {"FRED", nullptr, nullptr}},
};
constexpr int N_SERIES = 6;
inline constexpr uint8_t DEFAULT_SERIES[] = {0, 1, 2, 3};

inline int find_series(const char* id, size_t len) {
  for (int i = 0; i < N_SERIES; ++i)
    if (strlen(CATALOG[i].id) == len && strncmp(CATALOG[i].id, id, len) == 0) return i;
  return -1;
}

inline void format_value(ValueFmt f, double v, char* out, size_t n) {
  switch (f) {
    case ValueFmt::Thousands0: fmt_thousands0(out, n, v); return;
    case ValueFmt::KiloDollars1: snprintf(out, n, "$%.1fk", v / 1000); return;
    case ValueFmt::Percent2: snprintf(out, n, "%.2f%%", v); return;
    case ValueFmt::MegaDollars2: snprintf(out, n, "$%.2fM", v / 1e6); return;
    case ValueFmt::Fixed1: snprintf(out, n, "%.1f", v); return;
  }
}

}  // namespace ink
