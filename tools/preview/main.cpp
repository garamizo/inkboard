// Host preview (spec §3.4): renders FRAME_QUERY (or --query) to a PNG with the board's own
// parsers and renderer. Live data comes through curl; --fixtures uses test/fixtures.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <fstream>
#include <regex>
#include <sstream>
#include <string>

#include "config.h"
#include "data_cache.h"
#include "model.h"
#include "render/png.h"
#include "sources/fred.h"
#include "sources/openmeteo.h"
#include "../../test/fixtures/fixtures.h"

static ink::Model g_model;
static uint8_t g_frame[ink::FRAME_BYTES];
static ink::SeriesData g_scratch;

static std::string slurp(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  std::stringstream s;
  s << f.rdbuf();
  return s.str();
}

// Streams `url` (or a fixture file) into a JSON handler; true if the document was complete.
static bool fetch(const std::string& url, const std::string& fixture, ink::json::Handler& h) {
  ink::json::Parser p(h);
  if (!fixture.empty()) {
    const std::string doc = slurp(fixture);
    p.feed(doc.data(), doc.size());
    return !doc.empty() && p.finish();
  }
  const std::string cmd = "curl -sS --fail-with-body --max-time 60 '" + url + "'";
  FILE* f = popen(cmd.c_str(), "r");
  if (!f) return false;
  char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) p.feed(buf, n);
  return pclose(f) == 0 && p.finish();
}

static std::string fred_key() {
  if (const char* k = getenv("FRED_API_KEY"))
    if (*k) return k;
  std::smatch m;
  const std::string s = slurp(std::string(INK_TEST_DIR) + "/../include/secrets.h");
  if (std::regex_search(s, m, std::regex("#define\\s+FRED_API_KEY\\s+\"([^\"]+)\""))) return m[1];
  return "";
}

int main(int argc, char** argv) {
  std::string query = FRAME_QUERY, out = std::string(INK_TEST_DIR) + "/../.pio/preview.png";
  bool fixtures = false;
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--query") && i + 1 < argc) query = argv[++i];
    else if (!strcmp(argv[i], "--fixtures")) fixtures = true;
    else if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
    else {
      fprintf(stderr, "usage: %s [--query Q] [--fixtures] [--out PATH]\n", argv[0]);
      return 2;
    }
  }
  ink::Bitmap frame(g_frame, ink::FRAME_W, ink::FRAME_H);
  char error[192];
  if (!ink::parse_query(query.c_str(), g_model.layout, error, sizeof error)) {
    fprintf(stderr, "config error: %s\n", error);
    ink::render_config_error(frame, error, query.c_str());
  } else {
    const ink::Layout& l = g_model.layout;
    g_model.now = fixtures ? FIXTURE_NOW : static_cast<int64_t>(time(nullptr));
    const int32_t today = ink::tz::to_local(l.zone, g_model.now).day;
    const std::string fix = std::string(INK_TEST_DIR) + "/fixtures/";
    if (l.has_weather) {
      char path[400];
      ink::openmeteo_path(path, sizeof path, l.lat, l.lon, l.metric, l.tz);
      ink::OpenMeteoParser h;
      ink::Weather w;
      if (fetch(std::string("https://") + ink::OPENMETEO_HOST + path, fixtures ? fix + "weather_la.json" : "", h) &&
          h.result(w)) {
        g_model.weather = ink::WeatherCache{true, ink::weather_key(l), w, {}};
        g_model.weather.status.fetched_at = g_model.now;
      } else {
        fprintf(stderr, "weather: fetch failed\n");
      }
    }
    const std::string key = fixtures ? "" : fred_key();
    if (l.has_market && !fixtures && key.empty()) fprintf(stderr, "FRED_API_KEY not set: market data skipped\n");
    for (int i = 0; l.has_market && i < l.n_series && (fixtures || !key.empty()); ++i) {
      const ink::SeriesDef& sd = ink::CATALOG[l.series[i]];
      const ink::FredPlan plan = ink::plan_fred(ink::SeriesCache{}, g_model.now, today, l.years);
      char path[400];
      ink::fred_path(path, sizeof path, sd.fred_id, key.c_str(), plan.observation_start);
      ink::SundayResampler r;
      r.begin_full(g_scratch, plan.keep_from, today);
      ink::FredParser fp(r);
      const bool ok = fetch(std::string("https://") + ink::FRED_HOST + path,
                            fixtures ? fix + "fred_" + sd.fred_id + "_full.json" : "", fp) &&
                      fp.saw_observations() && r.finish(plan.keep_from);
      if (!ok) {
        fprintf(stderr, "%s: fetch failed%s\n", sd.fred_id, fp.api_key_error() ? " (API key rejected)" : "");
        g_model.series[i].status.auth_rejected = fp.api_key_error();
        continue;
      }
      ink::SeriesCache& c = g_model.series[i];
      c.valid = true;
      snprintf(c.fred_id, sizeof c.fred_id, "%s", sd.fred_id);
      c.data = g_scratch;
      c.status.fetched_at = g_model.now;
    }
    ink::build_frame(frame, g_model, "fw " INKBOARD_VERSION " preview");
  }
  const std::vector<uint8_t> png = ink::png_encode(g_frame, ink::FRAME_W, ink::FRAME_H);
  FILE* f = fopen(out.c_str(), "wb");
  if (!f || fwrite(png.data(), 1, png.size(), f) != png.size()) {
    fprintf(stderr, "cannot write %s\n", out.c_str());
    return 1;
  }
  fclose(f);
  printf("%s\n", out.c_str());
  return 0;
}
