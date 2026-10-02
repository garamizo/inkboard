# inkboard: on-device rendering (no server)

**Date:** 2026-09-30 · **Status:** design approved in brainstorming (sections 1–4 reviewed one by one; Codex
reviewed the draft of section 1, all 16 findings accepted and folded in)
**Branch:** `feat/ondevice-render` · **Replaces:** the render server of
`2026-09-27-dashboard-design.md` (§1–§3, §6). Its widget designs (§4, §5) still hold. The last server version
is tagged `v1.0.0` and `server-render-final`.

## Goal

The board needs nothing but Wi-Fi and its owner's FRED API key. The ESP32-C6 fetches weather (Open-Meteo,
keyless) and market data (FRED) itself, renders the 800×480 1-bit frame, and deep-sleeps. The Python server,
its Docker/Cloudflare deployment and `inkboard.signalwave.dev` go away.

## Decisions

| Topic | Decision |
|---|---|
| Rendering | Portable header-only C++ (`include/`), drawing into the same 48,000-byte frame the server sent (rows top to bottom, MSB leftmost, bit 1 = white). The existing `panel_show(frame)` displays it. Not MicroPython (no Pillow, heap fragmentation, new display driver); not GxEPD2/Adafruit_GFX drawing (Arduino-bound, so no host goldens; no thick or dashed lines). |
| Scope | **Full parity** with the server: both widgets at 1/3, 2/3 and full width, up to 3 columns, 6 FRED series available with 1–4 selected, `years` 1–10, imperial/metric, `lat`/`lon`/`tz`. Same look as `server/tests/goldens/`. |
| Config | `FRAME_QUERY` in `include/config.h`, same syntax and validation as the server's `query.py`, parsed on the device. A bad query shows the config error screen. |
| Secrets | `FRED_API_KEY` in `include/secrets.h` next to the Wi-Fi credentials (gitignored). Only needed when a `market_trends` widget is configured. |
| Time | SNTP each wake with Wi-Fi; the RTC keeps it through deep sleep. IANA `tz` → POSIX TZ rule from a generated table, applied with `localtime_r`. |
| Preview | `just preview ["<query>"]` renders a PNG on the host from live data (or `--fixtures`). |
| Tests | `pio test -e native`: logic, parsers and byte-exact goldens of the C++ renderer. The server's goldens are the visual reference until the server is deleted. |
| Server removal | Last phase of the branch, after the side-by-side golden sign-off. |

## 1. Architecture

```
 every wake: parse FRAME_QUERY ─► (Wi-Fi ─► SNTP, only if something is due or the clock is invalid)
   ─► only the sources the visible widgets need:
        Open-Meteo (cache > 30 min) · FRED tail (per series, cache > 6 h, one keep-alive connection)
   ─► stream parse into a scratch copy ─► validate ─► commit to LittleFS (temp + rename)
   ─► Wi-Fi off ─► widgets (clipped views) ─► compositor ─► 48 KB frame
   ─► CRC changed or panel dirty? mark dirty → panel_show → commit CRC
   ─► sleep to next local hh:01, or back off
```

### 1.1 Portable units (`include/`, header-only, host-tested, no Arduino)

| Unit | Job | Ported from |
|---|---|---|
| `render/canvas.h` | 1-bit drawing on the frame; `View` = translated, clipped box | Pillow `ImageDraw` |
| `render/font.h`, `render/fonts/*.h` | Font format, text measure/draw with anchors; generated DejaVu fonts | `draw/fonts.py`, TTFs |
| `render/png.h` | Write and read 1-bit PNGs (stored deflate blocks, no zlib) | `frame.py` `png_bytes` |
| `render/icons.h`, `render/lines.h`, `render/calendar.h` | Weather icons + WMO mapping, chart line styles/markers, month grid | `draw/*.py` |
| `render/format.h` | Python-compatible number formatting, English day/month names | f-strings, `strftime` |
| `widgets/market_trends.h`, `widgets/calendar_weather.h` | `fetch` status + pure `render(view, data, ctx)` | `widgets/*.py` |
| `compositor.h` | Columns, dividers, footer, "No data yet"/error per column, error screen, calibration pattern | `compositor.py`, `frame.py` |
| `query.h` | Parse/validate `FRAME_QUERY` | `query.py`, `widgets/params.py` |
| `series.h` | Series catalog: ids, FRED ids, labels, formatters, attributions | `series.py` |
| `market_data.h` | Weekly grid, normalization, y range, log ticks | `market_data.py` |
| `json_stream.h` | SAX-style streaming JSON tokenizer, bounded state | — |
| `sources/openmeteo.h`, `sources/fred.h` | Request URLs and streaming parsers | `sources/openmeteo.py`, `fred.py` |
| `data_cache.h` | Binary cache formats, FRED tail merge, due/stale rules | `sources/base.py` (the parts that apply) |
| `clock.h`, `tz.h`, `tz_table.h` (generated) | Clock validity, local time, next hh:01 | `schedule.py`, `zoneinfo` |
| `cycle.h`, `wake_logic.h` | Rewritten wake cycle, generic over injected ops | current files |
| `http_time.h` | Kept: HTTP `Date` parse (SNTP fallback) | current file |

### 1.2 Hardware units (`src/`)

- `net.cpp`: Wi-Fi, SNTP, HTTPS GET that streams the body into a parser through `HTTPClient::writeToStream`.
- `data_store.cpp`: LittleFS reads and atomic writes of cache files.
- `panel.cpp`: `panel_show(frame)` unchanged; the GxEPD2 paged error screen is removed (the error screen is a
  canvas render now).
- `main.cpp`: supplies the ops to `cycle.h`; USB-awake loop unchanged.

### 1.3 Removed

`server/` (after the sign-off, §6), `docs/cloudflare-tunnel.md`, the `just` recipes `setup-server`, `test`
(server form), `dev`, `up`, `down`, `logs`, `check`; `include/body_reader.h`; `include/badge.h`;
`src/frame_store.*`; `src/fetch.*`; `SERVER_URL` in `config.h`.

### 1.4 Resource budget (to be measured, §6 phase 1)

- SRAM (512 KB, no PSRAM): frame 48 KB static; GxEPD2 page buffer 6 KB (kept, as today); one TLS connection at a
  time (the SDK uses 16 KB content buffers; the handshake peak is measured, not assumed); parsed series
  4 × ≤523 `double` ≈ 17 KB; weather < 1 KB.
- App flash: one 1.9 MB slot of `min_spiffs.csv`. Fonts are `static const` (memory-mapped flash), one
  translation unit each.
- LittleFS: **128 KB**. Caches stay small (§2.5); each write needs room for its temp copy.

## 2. Data sources, caching, time

### 2.1 Clock (`clock.h`, `net.cpp`)

1. Each wake that brings Wi-Fi up runs SNTP (`pool.ntp.org`, 5 s timeout). If SNTP fails and the clock is not
   valid, the `Date` header of a tiny Open-Meteo request sets it.
2. The clock is valid only if it was synced since the last power-on or chip reset: `clock_valid` lives in
   RTC memory (`RTC_NOINIT_ATTR`, magic-checked) and is cleared on a magic mismatch or a reset reason that
   resets the RTC timer. Phase 1 verifies on hardware which reset reasons keep system time.
3. Wi-Fi comes up when any source is due **or** the clock is invalid.
4. With no valid clock after the network step, nothing is rendered: the panel is left as it is and the board
   backs off (§4).

### 2.2 Time zones (`tz.h`, `tz_table.h`)

- `tools/gen_tz_table.py` reads the POSIX TZ string from the footer of each TZif file in the system tzdata
  and writes `include/tz_table.h`: sorted `(IANA name, POSIX rule)` pairs. Committed; regenerated by hand.
- `tz=` must be a name in the table, otherwise it is a config error. The generator leaves out zones whose
  footer rule disagrees with `zoneinfo` from today on (none with tzdata 2026c); every other zone the
  server accepted is in the table.
- `setenv("TZ", rule)` + `tzset()` + `localtime_r` give local dates, the footer time, and the next local hh:01
  (found by stepping UTC 15 minutes at a time, as `schedule.py` does). DST, half-hour zones and Lord Howe's
  30-minute DST are covered by the rule format. The host tests use the same table and libc.

### 2.3 Which sources a wake uses

Only those the visible widgets need: weather if a `calendar_weather` column exists, the configured FRED series
(union over `market_trends` columns; options are shared, as on the server) if a `market_trends` column exists.

### 2.4 Open-Meteo (`sources/openmeteo.h`)

- Same request as `sources/openmeteo.py`: `current=temperature_2m,weather_code`,
  `daily=weather_code,temperature_2m_max,temperature_2m_min`, `timezone=<tz>`, `forecast_days=8`,
  `temperature_unit` from `units`. Response ≈ 1.5 KB.
- Due when the cache is older than **30 min** (the server's TTL) or its key
  `(round(lat,1), round(lon,1), units, tz)` differs from the config.
- Forecast rows are picked by date (`build_payload`), so a cached forecast from yesterday never shows
  yesterday's row as today.

### 2.5 FRED (`sources/fred.h`, `data_cache.h`)

- **Cache per series** (`/fred_<ID>.bin`): format version, FRED id, `first_sunday` (days since epoch), `n`,
  `n` × `double` (the value of the last observation on or before each consecutive Sunday, exactly
  `resample()`), latest observation `(date, value)`, `covered_from` (first Sunday the cache is valid for),
  `full_fetched_at`, plus the status fields of §2.7. ≈ 4.2 KB for 10 years.
- Render-time pipeline = `summarize()`: grid of Sundays from `today − round(365.25 × years)` plus `today`;
  the `today` point uses the latest observation; values ≤ 0 and `"."` are skipped at parse time;
  normalization, `ratio`, `yoy`, `short_history`, `last_date` computed as in `market_data.py`.
- **Full fetch** (no cache; `years` needs Sundays before `covered_from`; `full_fetched_at` older than 7 days):
  `observation_start` = window start − 62 days, so a monthly series has a value on or before the first
  Sunday. Native frequency, no aggregation (FRED's weekly aggregation drops the incomplete last week and
  loses observation dates).
- **Tail fetch** (cache older than the **6 h** TTL): `observation_start` = the day after a cached Sunday
  `S0` at least 120 days back. Every observation the tail does not cover is on or before `S0`, whose stored
  value is exactly the last observation ≤ `S0`. So Sundays after `S0` are recomputed from `{S0's value} ∪
  tail`, and the merge is exact. Revisions older than 120 days are picked up by the weekly full fetch.
- Sizes: a tail is ≈ 8 KB for a daily series; a full 10-year BTC fetch ≈ 370 KB, streamed, at most weekly.
- All due series use one keep-alive HTTPS connection to `api.stlouisfed.org`.
- Requests set `sort_order=asc`. An out-of-order response fails that refresh and keeps the cache: the
  server sorted, but sorting while streaming would need the whole series in RAM.
- A series with no usable cache makes the whole `market_trends` column "No data yet: fred", as on the server.
- Cache files for series no longer configured are deleted at boot.

### 2.6 Streaming and transport (`json_stream.h`, `net.cpp`)

- `json_stream.h`: SAX-style tokenizer with a 64-byte token limit and depth 8; overflow is a parse error.
  Both parsers sit on it and accept any key order and any chunk split.
- HTTP/1.1 without `Accept-Encoding`; `writeToStream()` to a `Print` sink handles Content-Length and chunked
  bodies. A result counts only if the transfer completed **and** the parser reached the end of the document
  with the required fields.
- Parsed data goes to a scratch copy; the cache file is replaced atomically (temp file + rename, as
  `frame_store.cpp` does today). A truncated body never replaces good data.
- Never logged: request URLs or HTTPClient error strings for FRED (both contain `api_key`). Logs name the
  series and the status code only.
- CA bundle: `tools/gen_ca_certs.sh` gets the roots that `api.open-meteo.com` and `api.stlouisfed.org`
  actually chain to (checked with `openssl s_client` when regenerating), instead of Cloudflare's.

### 2.7 Source status and stale rule

Persisted with each cache: `fetched_at` (last success), `last_attempt_failed`, `retry_not_before`
(`Retry-After` on 429/503), `auth_rejected` (FRED 400/403 with an api_key error).

- **Due:** past TTL (or key/coverage change) and past `retry_not_before`.
- **Stale:** never within the TTL; past it, stale if the last attempt failed or the data is older than
  TTL + 90 min (the server rule). A wake whose Wi-Fi did not connect does not mark sources failed (they
  were never asked), so a board offline for less than 2 h keeps its frame (§4.4).
- **Footer:** "updated h:mm AM" = newest `fetched_at` among visible widgets with data, in the local tz;
  `⚠ stale` if any visible widget's sources are stale; the firmware version; attributions deduplicated in
  widget order, as `compositor.py` does.

### 2.8 Wake budget

- One deadline of 45 s for the network step: Wi-Fi connect (≤ 15 s), SNTP (≤ 5 s), weather, then FRED with what
  is left. FRED is skipped (counted as failed) when less than 8 s remain.
- Wi-Fi is off before rendering and drawing.

## 3. Rendering

### 3.1 Fonts (`tools/gen_fonts.py` → `include/render/fonts/`)

- A `uv` dev script loads DejaVu with Pillow in `fontmode="1"` (the server's path) and writes one header per
  font: monochrome glyph bitmaps with Pillow's left-baseline offsets, advances in 1/64 px, ascent/descent for
  anchors, and kerning pairs measured as `getlength("AB") − getlength("A") − getlength("B")` (non-zero only).
- Character set: printable ASCII plus `° × · — ⚠`, indexed by code point; text is UTF-8.
- Sizes: regular 10, 11, 12, 13, 14, 15, 18; bold 8, 11, 12, 13, 14, 15, 17, 18, 20, 22, 26, 36, 46. The list
  comes from the server's `font(...)` calls; the generator fails if a widget asks for a missing font.
- The TTFs and `LICENSE` move from `server/inkboard_server/fonts/` to `tools/fonts/`.

### 3.2 Canvas (`render/canvas.h`)

- Writes the frame bits directly; there is no grayscale step (the server only filled 0 or 255).
- Primitives follow Pillow where it changes pixels: float coordinates and Pillow's rounding; wide lines as
  polygons, `joint="curve"` for solid lines wider than 2 px; ellipse outline width; polygon fill and outline;
  rounded rectangles; points.
- `text()` anchors: `la`, `lm`, `rm`, `mm`, `ra`, `rb`, `mt` (the ones the server uses).
- `View{canvas, box}` translates and clips; each widget renders into its own view.

### 3.3 Widgets and compositor

- Line-for-line ports of `calendar_weather.py`, `market_trends.py`, `draw/*.py`, `compositor.py`: same
  constants and layout. Three 266-px columns leave the 2 px remainder white, as today.
- `format.h`: round-half-even (`deg()`), `{:,.0f}`, `{:.2f}`, `{:+.0f}%`, `%g` ticks, English names.
- No exceptions on the render path. A widget's data is `Ok(payload) | NoData(source) | AuthRejected`; payload
  checks run before `render`; a failed check draws "error: \<type\>" in that column.
- The config error screen (title, reason, query) and the calibration pattern (`frame.py`) are canvas renders
  and go through the normal show path.

### 3.4 Goldens and preview

- `pio test -e native` renders the server's golden scenarios (`calendar_weather_{third,two_thirds,full}`,
  `market_trends_{third,two_thirds,full,short_series}`, `screen_{default,nodata,render_error,stale}`,
  `api_footer_*` equivalents) from fixtures with a fixed clock and compares bytes with `test/goldens/*.png`.
  `INKBOARD_UPDATE_GOLDENS=1` rewrites them.
- The server goldens are copied to `test/reference/` for the side-by-side review (§6 phase 4).
- `just preview ["<query>"]`: curl fetches Open-Meteo and FRED JSON (key from `$FRED_API_KEY` or
  `include/secrets.h`) into a temp dir; a native program (`[env:preview]`) runs the same parsers, merge and
  renderer and writes `preview.png`. `--fixtures` renders offline.
- Fixtures: raw API responses recorded into `test/fixtures/` (api_key stripped) by `tools/record_fixtures.py`,
  one full and one tail response per FRED series, and Open-Meteo for Los Angeles. The current server fixtures
  were rebuilt from CSV and are not wire-accurate.

## 4. Wake cycle and failure handling

### 4.1 RTC state

`RTC_NOINIT_ATTR` struct, magic-checked: `clock_valid`, `panel_dirty`, `shown_crc`, `fail_count`. Magic
mismatch (power loss) → `{clock_valid=false, panel_dirty=true, shown_crc=0, fail_count=0}`.

### 4.2 `cycle.h::run_cycle(ops, rtc)`

1. **Config.** Parse `FRAME_QUERY`; invalid → render the error screen, show it (§4.3), sleep
   `FALLBACK_SLEEP_S`.
2. **Store.** Mount LittleFS (format on mount failure: it only holds caches); load caches; delete
   `/frame.bin`, `/etag.txt` and unconfigured series files. No filesystem → caches in RAM only, everything due.
3. **Due.** Per §2.7; Wi-Fi only if something is due or the clock is invalid.
4. **Network** within the 45 s budget (§2.8), then Wi-Fi off.
5. **No valid clock** → no render; count as a network failure.
6. **Render** from caches and statuses; CRC32 of the frame.
7. **Show** (§4.3) if `panel_dirty` or CRC ≠ `shown_crc`.
8. **Sleep:**
   - *Network failure* (Wi-Fi did not connect, or every attempted fetch failed): `fail_count++`, back off
     300 / 900 / 3600 s, longer if `Retry-After` asks.
   - *Otherwise* (nothing due, or at least one fetch succeeded): `fail_count = 0`, next local hh:01.
   - Clamped to 300–21,600 s. Dev builds and a computer on USB keep the current awake loop (`idle_step`).

### 4.3 Show

`panel_dirty = true` → `panel_show(frame)` → `shown_crc = crc; panel_dirty = false`. Every drawing path (dashboard,
error screen, calibration) uses it, so a reset mid-refresh leaves the panel dirty and the next wake redraws.

### 4.4 What the owner sees

| Situation | Panel |
|---|---|
| Normal hour | Fresh frame ("updated" moves with weather): a refresh about hourly, as today |
| Offline < 2 h | Same frame (CRC equal), no refresh |
| Offline ≥ 2 h | `⚠ stale` in the footer (weather past TTL + 90 min); one refresh |
| Offline past midnight | New date and month grid from cached data; forecast rows by date |
| FRED down, weather OK | Chart from cache; `⚠ stale` after 6 h + 90 min |
| FRED key wrong or missing | Market column: "FRED API key rejected: check secrets.h"; weather unaffected |
| First boot offline (no clock) | Panel untouched; back-off retries |
| Clock valid, no cache, fetch failed | "No data yet: \<source\>" in that column |
| Bad `FRAME_QUERY` | Config error screen with the reason and the query |

The offline badge is gone; `⚠ stale` plus the "updated" time covers it.

## 5. Tooling, docs

- `include/secrets.h.example` gains `FRED_API_KEY`. A `static_assert` fails the build when `FRAME_QUERY`
  contains `market_trends` and the key is still the placeholder (a constexpr substring check, like the
  existing Wi-Fi check in `src/fetch.cpp`).
- `include/config.h`: `FRAME_QUERY`, `FALLBACK_SLEEP_S`, `USE_CALIBRATION_PATTERN`, `INKBOARD_DEEP_SLEEP`;
  `SERVER_URL` removed.
- `just`: `setup` also asks for the FRED key; `test` runs `pio test -e native`; `preview` as §3.4;
  `flash-dev` = deep sleep off (no dev server); `flash`, `monitor` unchanged; `gen-fonts`, `gen-tz`,
  `record-fixtures` for the generators.
- README: board-owner setup with the FRED key (link to the free key form), layout section pointing at
  `just preview` instead of the server URL, no server-provider section. `CLAUDE.md` commands and rules
  updated. `docs/dashboard-bringup.md` without server steps. The 2026-09-27 spec gets a header note that
  this spec supersedes its server parts.
- VERSION → 2.0.0 (the board no longer talks to the server).

## 6. Phases (each ends green: `pio test -e native` and `pio run -e supermini-c6`)

1. **Hardware spike (throwaway, `extras/spike/`):** TLS to both APIs + 48 KB frame + 4 × 523 doubles + a
   large font in flash; log free heap, min free heap, largest free block, stack high water, wake time;
   SNTP and system time across deep sleep, RESET and software reset; FRED full and tail payload sizes and
   timings; Open-Meteo `Date` header. Results recorded in the spec. Needs the board.
2. **Foundations:** canvas, fonts generator, PNG, format, query, series, clock/tz, json_stream, parsers,
   data_cache, market_data; fixtures recorded. Host tests only.
3. **Widgets and compositor:** ports + goldens; `just preview`.
4. **Firmware cycle:** new `cycle.h`/`wake_logic.h`, `net.cpp`, `data_store.cpp`, `main.cpp`; hardware check
   on the board (normal, offline, bad key, bad query, cold boot).
5. **Sign-off and removal:** side-by-side golden review with the owner; delete `server/` and the server
   tooling/docs; README, CLAUDE.md, VERSION.

## 7. Risks

- **TLS heap peak** with the 48 KB frame resident: phase 1 measures it before any port. Fallback: allocate the
  frame only after Wi-Fi is off.
- **Pixel parity with Pillow** may not be exact (rasterization rounding, kerning). Acceptance is visual
  equivalence signed off by the owner; byte-exact only against the C++ goldens.
- **FRED API changes or key misuse limits** (120 requests/min per key): one board makes ≤ 4 requests per 6 h.
- **tzdata drift:** `tz_table.h` is regenerated by hand; a rule change needs a reflash.
- **Clock after RESET:** if a chip reset keeps a wrong time that looks valid, dates are wrong until the next
  SNTP; phase 1 decides which reset reasons clear `clock_valid`.

## 8. Spike results (Task 1, 2026-10-01)

Spike firmware (`extras/spike`, with a 120 KB static block standing in for the renderer's RAM) on the
SuperMini C6, home Wi-Fi.

| Measurement | Value |
|---|---|
| Free heap at boot (min, largest block) | 245,368 B (240,608, 225,268) |
| After Wi-Fi | 190,848 B (187,284, 172,020) |
| During TLS + FRED (min, largest block) | ~141,700 B (121,716, 114,676–118,772) — gate (≥ 40,000 / ≥ 30,000) passes |
| After closing the Open-Meteo session | 186,740 B |
| Wi-Fi connect | 1.2 s |
| SNTP | 2.5–4.4 s |
| Open-Meteo (new TLS session, chunked) | 792 B in 2.4–3.0 s |
| FRED full CBBTCUSD 10 y (new TLS session, chunked) | 366,064 B in 3.2–3.3 s |
| FRED tails SP500 / MORTGAGE30US / MEDLISPRI31080 | 9,449 / 2,080 / 770 B in 0.31–0.61 s each |
| Keep-alive reused across FRED requests | yes (tails ~0.4 s vs ~3 s with a handshake) |
| Whole spike wake (Wi-Fi + SNTP + 5 requests) | 14.6–16 s |
| Reset reasons that keep system time | 8 (deep sleep), 3 (software reset): clock within boot time of the HTTP `Date` |
| Reset reasons that lose it | 1 (power-on; the C6 reports the RESET pin as power-on too) |
| CA roots | api.open-meteo.com: ISRG Root X1 (chain via Let's Encrypt "Root YR"); api.stlouisfed.org: DigiCert Global Root G3 |
| Spike firmware size | RAM 165,792 / 327,680 B (50.6 %), Flash 1,119,172 / 1,966,080 B (56.9 %) |
| newlib POSIX TZ with `<+1030>` names | works (informational; tz.h has its own evaluator) |

Notes for the firmware:
- `sntp_get_sync_status()` returns `COMPLETED` once and then resets; read it once per sync.
- If a host process keeps the old `/dev/ttyACM*` node open across a deep-sleep wake, the board comes
  back under a new node (`ttyACM1`); tools that wait for the board should glob `ttyACM*`.

### On-board results (Task 19, 2026-10-01)

Per the owner's request, checks ran on the dev build (deep sleep off) and the production build was flashed
last; the long battery run was skipped.

| Check | Result |
|---|---|
| First cycle (cold, empty caches) | weather + 4 FRED full fetches HTTP 200; full panel refresh 2.6 s; next update at hh:01 |
| Heap / stack after a cycle | free ≥ 126 KB minimum during TLS; loop stack high-water 1,820 B free of 8 KB |
| RESET (power-on, clock lost) | SNTP resynced the clock; no fetches (caches fresh from LittleFS); panel redrawn (cold boot is dirty) |
| Production build with a computer on USB | stays awake ("computer on USB"), schedules hh:01; deep sleep starts once unplugged |
| Firmware size | RAM 164,928 B (50.3 %), Flash 1,414,972 / 1,966,080 B (72.0 %) |
