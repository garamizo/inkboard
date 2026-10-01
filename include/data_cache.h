#pragma once
// Cached source data on the board (spec §2.5-§2.7): records, their binary file format,
// and the rules for when to fetch and when to call data stale. Pure; host-tested.
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <type_traits>

#include "civil.h"
#include "crc32.h"
#include "market_data.h"
#include "sources/fred.h"
#include "sources/openmeteo.h"

namespace ink {

constexpr int64_t WEATHER_TTL_S = 1800;     // the server's TTLs
constexpr int64_t FRED_TTL_S = 21600;
constexpr int64_t STALE_GRACE_S = 5400;     // base.py: stale once 90 min past the TTL
constexpr int64_t FULL_REFETCH_S = 7 * 86400;
constexpr int64_t RETRY_SPACING_S = 300;    // base.py retry_after

struct SourceStatus {
  int64_t fetched_at = 0;  // last success (unix s)
  int64_t retry_not_before = 0;
  bool last_attempt_failed = false;
  bool auth_rejected = false;
};

struct FetchResult {
  int http_status = -1;    // < 0: no HTTP response
  bool complete = false;   // whole body read and the parser reached the end of the document
  int32_t retry_after_s = -1;
  int64_t date_epoch = -1;
  bool auth_error = false;
};

inline bool fetch_ok(const FetchResult& r) { return r.http_status == 200 && r.complete; }

inline bool is_due(const SourceStatus& s, int64_t now, int64_t ttl) {
  if (now < s.retry_not_before) return false;
  if (s.fetched_at <= 0) return true;
  const int64_t age = now - s.fetched_at;
  return age < 0 || age >= ttl;  // a clock that went backwards: refetch rather than trust
}

inline bool is_stale(const SourceStatus& s, int64_t now, int64_t ttl) {
  const int64_t age = now - s.fetched_at;
  if (age < ttl) return false;
  return s.last_attempt_failed || age >= ttl + STALE_GRACE_S;
}

inline void record_success(SourceStatus& s, int64_t now) {
  s.fetched_at = now;
  s.retry_not_before = 0;
  s.last_attempt_failed = false;
  s.auth_rejected = false;
}

inline void record_failure(SourceStatus& s, const FetchResult& r, int64_t now) {
  s.last_attempt_failed = true;
  s.auth_rejected = r.auth_error;
  s.retry_not_before = now + (r.retry_after_s > RETRY_SPACING_S ? r.retry_after_s : RETRY_SPACING_S);
}

struct WeatherKey {
  double lat, lon;
  bool metric;
  char tz[64];
};

inline bool same_key(const WeatherKey& a, const WeatherKey& b) {
  return a.lat == b.lat && a.lon == b.lon && a.metric == b.metric && strcmp(a.tz, b.tz) == 0;
}

struct WeatherCache {
  bool valid = false;
  WeatherKey key = {};
  Weather data = {};
  SourceStatus status = {};
};

struct SeriesCache {
  bool valid = false;
  char fred_id[16] = "";
  SeriesData data = {};
  int64_t full_fetched_at = 0;
  SourceStatus status = {};
};

inline bool weather_usable(const WeatherCache& c, const WeatherKey& want) { return c.valid && same_key(c.key, want); }

inline bool weather_due(const WeatherCache& c, const WeatherKey& want, int64_t now) {
  if (!same_key(c.key, want)) return true;            // another location: its old spacing does not apply
  if (now < c.status.retry_not_before) return false;  // also for a key that never succeeded (base.py miss failures)
  return !c.valid || is_due(c.status, now, WEATHER_TTL_S);
}

enum class FredFetch : uint8_t { None, Full, Tail };

struct FredPlan {
  FredFetch kind;
  int32_t observation_start;
  int32_t s0;         // Tail: the cached Sunday the tail starts after
  int32_t keep_from;  // first Sunday of the configured window
};

inline FredPlan plan_fred(const SeriesCache& c, int64_t now, int32_t today, int years) {
  const int32_t ws = window_start_day(today, years);
  const int32_t g0 = first_sunday_on_or_after(ws);
  const FredPlan none{FredFetch::None, 0, 0, g0};
  const FredPlan full{FredFetch::Full, ws - 62, 0, g0};  // 62 days back: a monthly series has a value by g0
  if (now < c.status.retry_not_before) return none;
  if (!c.valid || c.data.latest_date == INT32_MIN || c.data.n == 0 || c.data.covered_from == INT32_MIN ||
      c.data.covered_from > g0)
    return full;
  if (!is_due(c.status, now, FRED_TTL_S)) return none;
  if (now - c.full_fetched_at >= FULL_REFETCH_S || now < c.full_fetched_at) return full;
  const int32_t limit = today - 120;
  if (limit < c.data.first_sunday) return full;
  int32_t k = (limit - c.data.first_sunday) / 7;
  if (k >= c.data.n) k = c.data.n - 1;
  const int32_t s0 = c.data.first_sunday + 7 * k;
  return FredPlan{FredFetch::Tail, s0 + 1, s0, g0};
}

enum class CacheKind : uint16_t { Weather = 1, Series = 2 };

constexpr uint32_t CACHE_MAGIC = 0x434B4E49;  // "INKC"
constexpr uint16_t CACHE_VERSION = 1;          // bump when a record's layout changes
constexpr size_t CACHE_HEADER_BYTES = 16;

// [magic u32][version u16][kind u16][size u32][crc32 of the record u32][record bytes]
// Raw struct bytes: the same firmware writes and reads them; the version guards layout changes.
template <class T>
size_t encode_cache(CacheKind k, const T& rec, uint8_t* out, size_t cap) {
  static_assert(std::is_trivially_copyable<T>::value, "cache records are copied as bytes");
  if (cap < CACHE_HEADER_BYTES + sizeof(T)) return 0;
  const uint32_t magic = CACHE_MAGIC, size = sizeof(T);
  const uint16_t version = CACHE_VERSION, kind = static_cast<uint16_t>(k);
  memcpy(out + CACHE_HEADER_BYTES, &rec, sizeof(T));
  const uint32_t crc = crc32(out + CACHE_HEADER_BYTES, sizeof(T));
  memcpy(out, &magic, 4);
  memcpy(out + 4, &version, 2);
  memcpy(out + 6, &kind, 2);
  memcpy(out + 8, &size, 4);
  memcpy(out + 12, &crc, 4);
  return CACHE_HEADER_BYTES + sizeof(T);
}

template <class T>
bool decode_cache(CacheKind k, const uint8_t* in, size_t len, T& rec) {
  static_assert(std::is_trivially_copyable<T>::value, "cache records are copied as bytes");
  if (len != CACHE_HEADER_BYTES + sizeof(T)) return false;
  uint32_t magic, size, crc;
  uint16_t version, kind;
  memcpy(&magic, in, 4);
  memcpy(&version, in + 4, 2);
  memcpy(&kind, in + 6, 2);
  memcpy(&size, in + 8, 4);
  memcpy(&crc, in + 12, 4);
  if (magic != CACHE_MAGIC || version != CACHE_VERSION || kind != static_cast<uint16_t>(k) || size != sizeof(T) ||
      crc != crc32(in + CACHE_HEADER_BYTES, sizeof(T)))
    return false;
  memcpy(&rec, in + CACHE_HEADER_BYTES, sizeof(T));
  return true;
}

}  // namespace ink
