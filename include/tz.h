#pragma once
// IANA zone -> POSIX TZ rule (generated table) and local time without a tz database
// (spec §2.2). Our own rule evaluator, so the host tests and the board run the same code.
// Pure; host-tested against Python's zoneinfo (test/test_data/tz_cases.h).
#include <stdint.h>
#include <string.h>

#include "civil.h"
#include "tz_table.h"

namespace ink::tz {

inline const char* lookup(const char* name) {
  size_t lo = 0, hi = sizeof(TABLE) / sizeof(TABLE[0]);
  while (lo < hi) {
    const size_t mid = (lo + hi) / 2;
    const int c = strcmp(TABLE[mid].name, name);
    if (c == 0) return TABLE[mid].rule;
    if (c < 0) lo = mid + 1;
    else hi = mid;
  }
  return nullptr;
}

struct Transition {
  char kind;     // 'M' month.week.day, 'J' Julian 1..365 without Feb 29, 'N' 0..365 with it
  int m, w, d;   // 'M': month 1-12, week 1-5 (5 = last), weekday 0 = Sunday
  int day_n;     // 'J' / 'N'
  int32_t time_s;  // local time of day of the switch, may be negative or > 24 h
};

struct Rule {
  int32_t std_offset;  // seconds east of UTC (POSIX writes the opposite sign)
  int32_t dst_offset;
  bool has_dst;
  Transition start, end;
};

namespace detail {

inline bool name(const char*& s) {
  if (*s == '<') {
    const char* e = strchr(s, '>');
    if (e == nullptr) return false;
    s = e + 1;
    return true;
  }
  const char* b = s;
  while ((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z')) ++s;
  return s - b >= 3;
}

inline bool number(const char*& s, int& out) {
  if (*s < '0' || *s > '9') return false;
  out = 0;
  while (*s >= '0' && *s <= '9') out = out * 10 + (*s++ - '0');
  return true;
}

// [+-]hh[:mm[:ss]]
inline bool hms(const char*& s, int32_t& out) {
  int sign = 1;
  if (*s == '+' || *s == '-') sign = *s++ == '-' ? -1 : 1;
  int v[3] = {0, 0, 0};
  if (!number(s, v[0])) return false;
  for (int i = 1; i < 3 && *s == ':'; ++i) {
    ++s;
    if (!number(s, v[i])) return false;
  }
  out = sign * (v[0] * 3600 + v[1] * 60 + v[2]);
  return true;
}

inline bool transition(const char*& s, Transition& t) {
  t = Transition{'M', 0, 0, 0, 0, 7200};
  if (*s == 'M') {
    ++s;
    if (!number(s, t.m) || *s != '.') return false;
    ++s;
    if (!number(s, t.w) || *s != '.') return false;
    ++s;
    if (!number(s, t.d)) return false;
    if (t.m < 1 || t.m > 12 || t.w < 1 || t.w > 5 || t.d > 6) return false;
  } else if (*s == 'J') {
    ++s;
    t.kind = 'J';
    if (!number(s, t.day_n) || t.day_n < 1 || t.day_n > 365) return false;
  } else {
    t.kind = 'N';
    if (!number(s, t.day_n) || t.day_n > 365) return false;
  }
  if (*s == '/') {
    ++s;
    if (!hms(s, t.time_s)) return false;
  }
  return true;
}

inline int32_t transition_day(const Transition& t, int y) {
  const int32_t jan1 = days_from_civil(y, 1, 1);
  if (t.kind == 'J') return jan1 + t.day_n - 1 + (is_leap(y) && t.day_n >= 60 ? 1 : 0);
  if (t.kind == 'N') return jan1 + t.day_n;
  const int32_t first = days_from_civil(y, t.m, 1);
  const int sun0 = (weekday(first) + 1) % 7;  // 0 = Sunday
  int32_t d = first + (t.d - sun0 + 7) % 7 + (t.w - 1) * 7;
  while (civil_from_days(d).m != t.m) d -= 7;  // week 5 means "last"
  return d;
}

}  // namespace detail

inline bool parse(const char* s, Rule& r) {
  using namespace detail;
  if (s == nullptr) return false;
  int32_t off;
  if (!name(s) || !hms(s, off)) return false;
  r = Rule{};
  r.std_offset = -off;
  r.dst_offset = r.std_offset;
  if (*s == '\0') return true;
  if (!name(s)) return false;
  r.has_dst = true;
  r.dst_offset = r.std_offset + 3600;
  if (*s != ',') {
    if (!hms(s, off)) return false;
    r.dst_offset = -off;
  }
  // tzdata footers with DST always spell out both transitions.
  if (*s != ',') return false;
  ++s;
  if (!transition(s, r.start) || *s != ',') return false;
  ++s;
  if (!transition(s, r.end)) return false;
  return *s == '\0';
}

inline bool is_dst(const Rule& r, int64_t t) {
  if (!r.has_dst) return false;
  const int y = civil_from_days(static_cast<int32_t>(floor_div(t + r.std_offset, 86400))).y;
  // The start time is local standard time, the end time local daylight time.
  const int64_t start = int64_t(detail::transition_day(r.start, y)) * 86400 + r.start.time_s - r.std_offset;
  const int64_t end = int64_t(detail::transition_day(r.end, y)) * 86400 + r.end.time_s - r.dst_offset;
  return start < end ? (t >= start && t < end) : !(t >= end && t < start);
}

inline int32_t utc_offset(const Rule& r, int64_t t) { return is_dst(r, t) ? r.dst_offset : r.std_offset; }

struct Local {
  int32_t day;
  int hh, mm, ss;
  int32_t offset;
};

inline Local to_local(const Rule& r, int64_t t) {
  const int32_t off = utc_offset(r, t);
  const int64_t l = t + off;
  const int32_t day = static_cast<int32_t>(floor_div(l, 86400));
  const int32_t sod = static_cast<int32_t>(l - int64_t(day) * 86400);
  return Local{day, sod / 3600, sod / 60 % 60, sod % 60, off};
}

// schedule.py: step UTC in 15-minute increments (every real offset is a multiple of 15 min)
// until local time reads hh:00, so DST and half-hour zones need no special cases.
inline int64_t next_top_of_hour(const Rule& r, int64_t now) {
  int64_t t = now - floor_mod(now, 900);
  for (;;) {
    t += 900;
    const Local l = to_local(r, t);
    if (l.mm == 0 && t > now) return t;
  }
}

inline int32_t next_refresh_seconds(const Rule& r, int64_t now) {
  const int64_t s = next_top_of_hour(r, now) + 60 - now;
  return static_cast<int32_t>(s < 300 ? 300 : s > 21600 ? 21600 : s);
}

}  // namespace ink::tz
