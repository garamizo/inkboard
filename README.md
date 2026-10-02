# inkboard

Firmware for a 7.5" e-paper desk dashboard built on an ESP32-C6. The board fetches weather
(Open-Meteo) and market data (FRED) itself and renders the 800×480 screen on the device, so
there is no server to run: it wakes, updates the panel and deep-sleeps.

**Hardware:** ESP32-C6 SuperMini + Waveshare e-Paper Driver HAT (rev 2.3) + Waveshare 7.5" V2
(800×480 B/W) in the [Smart E-Paper Desk Dashboard](https://makerworld.com/en/models/2443888-smart-e-paper-desk-dashboard-esp32-7-5)
case.

**Stack:** C++ / Arduino-ESP32 3.x / PlatformIO ([why](docs/toolchain.md)), display via
[GxEPD2](https://github.com/ZinggJM/GxEPD2).

## First-time setup

### Linux or Windows: Wi-Fi + flash

You need the assembled board ([docs/wiring.md](docs/wiring.md)), a USB-C data cable and a
2.4 GHz Wi-Fi network (the ESP32-C6 has no 5 GHz radio).

1. **Install git, [just](https://github.com/casey/just) and [uv](https://docs.astral.sh/uv/).**

   Linux:

   ```bash
   sudo apt install git just   # or your distro's package manager
   curl -LsSf https://astral.sh/uv/install.sh | sh
   ```

   Windows (PowerShell; open a new terminal afterwards so the tools are on `PATH`):

   ```powershell
   winget install Git.Git Casey.Just astral-sh.uv
   ```

   Windows 10/11 needs no USB driver: the board shows up as a `COM` port on its own.

2. **Get the code and run setup:**

   ```bash
   git clone https://github.com/garamizo/inkboard.git
   cd inkboard
   just setup
   ```

   `just setup` installs PlatformIO, creates `include/secrets.h` from its template, and on
   Linux checks that you can open the serial port (it prints the `usermod` command if not).
   If the VS Code PlatformIO extension is already installed, it reuses the extension's copy:
   two PlatformIO versions building the same project delete each other's build files
   ([#1](https://github.com/garamizo/inkboard/issues/1)).

3. **Set your Wi-Fi credentials and FRED key:** fill in `WIFI_SSID`, `WIFI_PASSWORD` and
   `FRED_API_KEY` in `include/secrets.h`. Get the key free at
   [fredaccount.stlouisfed.org/apikeys](https://fredaccount.stlouisfed.org/apikeys); it is only
   needed when the layout uses `market_trends`. The file is gitignored, so your password and
   key never get committed.

   Optional: set your location and widgets, see
   [Change the dashboard layout](#change-the-dashboard-layout).

4. **Flash.** Plug the board in over USB-C, then:

   ```bash
   just flash smoke   # optional: hardware + Wi-Fi check first, see docs/smoke-test.md
   just flash         # dashboard firmware, then the serial monitor (Ctrl+C to exit)
   ```

   The first build downloads the toolchain (~1 GB into `~/.platformio`) and takes a few
   minutes. If the upload can't connect, the board is probably in deep sleep: tap **RESET**
   (not BOOT) and run `just flash` again. The board wakes, sees the computer and stays awake. More troubleshooting: [docs/smoke-test.md](docs/smoke-test.md).

## Change the dashboard layout

The board's whole layout is one query string: `FRAME_QUERY` in `include/config.h`. A new
layout only needs a reflash.

```c
#define FRAME_QUERY \
  "w=market_trends:2/3,calendar_weather:1/3&lat=34.05&lon=-118.24&tz=America/Los_Angeles&units=imperial"
```

1. **Pick the widgets with `w`:** up to three `type:size` entries, drawn as columns from left
   to right. Sizes are `1/3`, `2/3` or `1` (full width) and must add up to the full width,
   e.g. `market_trends:2/3,calendar_weather:1/3` or `calendar_weather:1`.
2. **Set the options.** `tz` is global; the rest belong to a widget and are only allowed when
   that widget is in `w`:

   | Parameter | Widget | Values | Default |
   |---|---|---|---|
   | `tz` | all | IANA time zone, e.g. `America/New_York`; sets the date, clock and update times | `UTC` |
   | `lat`, `lon` | `calendar_weather` | your location in degrees (rounded to 0.1°) | required |
   | `units` | `calendar_weather` | `imperial` or `metric` | `imperial` |
   | `series` | `market_trends` | 1 to 4 series ids, comma-separated (below) | `sp500,btc,mortgage30,home_la` |
   | `years` | `market_trends` | chart span, 1 to 10 | `5` |

   Series ids: `sp500` (S&P 500), `btc` (Bitcoin), `mortgage30` (30-year mortgage rate),
   `home_la` (LA median home listing price), `ust10y` (10-year Treasury), `usd_broad`
   (dollar index).
3. **Preview it on this computer:** `just preview` (or `just preview "<query>"`) writes
   `.pio/preview.png`. A mistake prints a one-line error instead (e.g. `lat: not used by any
   widget in w`). `just preview "" --fixtures` renders offline from recorded data; to pass
   several flags, quote them as one string: `just preview "" "--fixtures --out x.png"`.
4. **Flash** with `just flash`. The new layout shows on the next update.

Examples:

```text
w=calendar_weather:1&lat=40.7&lon=-74.0&tz=America/New_York&units=metric
w=market_trends:1&series=sp500,btc,ust10y,usd_broad&years=10&tz=America/Chicago
w=calendar_weather:1/3,market_trends:2/3&lat=51.5&lon=-0.1&tz=Europe/London&units=metric&series=sp500,ust10y
```

New widget types or market series are firmware changes: widgets live in `include/widgets/` and
series in `include/series.h`.

## Repository layout

```
VERSION              release version (dashboard footer, board User-Agent); dev builds add -<git hash>
platformio.ini       build config (pinned platform + libraries)
justfile             common commands: `just` lists them (test, preview, flash, ...)
include/pins.h       every GPIO assignment, mirrors docs/wiring.md
include/render/      canvas, text, bitmap fonts, icons, PNG writer (portable, host-tested)
include/widgets/     calendar_weather and market_trends
include/sources/     Open-Meteo and FRED clients (streaming JSON parsers)
src/                 dashboard firmware: Wi-Fi, TLS, clock, display, deep sleep (docs/dashboard-bringup.md)
extras/smoke/        hardware smoke test + refresh soak test (pio run -e smoke)
extras/minimal/      bare display check (pio run -e minimal), mirrors the kit's firmware
tools/               generators: fonts, time-zone table, fixtures, Pillow reference images
test/                host test suites, recorded fixtures, golden PNGs, reference images
docs/
  hardware.md        what each board does, how the e-paper HAT works
  wiring.md          connection table, pin budget, battery + speaker plans
  smoke-test.md      flashing, expected output, troubleshooting
  toolchain.md       language / framework choice and commands
```

## Roadmap

- [x] Hardware smoke test
- [ ] Wi-Fi provisioning + NTP clock
- [x] Dashboard layout engine (widgets rendered on the board)
- [x] Data sources (FRED markets, Open-Meteo weather, fetched by the board)
- [x] Deep-sleep update cycle (battery voltage still to do)
- [ ] Speaker output (I2S)
- [ ] OTA updates
