# inkboard

Firmware for a 7.5" e-paper desk dashboard built on an ESP32-C6. What it shows is still open:
calendar, weather, reminders, home status.

**Hardware:** ESP32-C6 SuperMini + Waveshare e-Paper Driver HAT (rev 2.3) + Waveshare 7.5" V2
(800×480 B/W) in the [Smart E-Paper Desk Dashboard](https://makerworld.com/en/models/2443888-smart-e-paper-desk-dashboard-esp32-7-5)
case.

**Stack:** C++ / Arduino-ESP32 3.x / PlatformIO ([why](docs/toolchain.md)), display via
[GxEPD2](https://github.com/ZinggJM/GxEPD2).

## First-time setup

There are two roles. A **board owner** only builds and flashes the firmware; the board talks to
the public server at `https://inkboard.signalwave.dev`. A **server provider** runs that render
server (API keys, Docker, Cloudflare Tunnel). Most people only need the first section.

### Board owner (Linux or Windows): Wi-Fi + flash

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

3. **Set your Wi-Fi credentials:** fill in `WIFI_SSID` and `WIFI_PASSWORD` in
   `include/secrets.h`. The file is gitignored, so your password never gets committed.

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

### Server provider (Linux): API keys + deploy

The render server lives in `server/` (Python, served by Docker Compose). It is published
through a Cloudflare Tunnel, so no router port is opened. You need Linux with Docker (Compose
v2), git, just and uv.

1. **Clone and run setup:**

   ```bash
   git clone https://github.com/garamizo/inkboard.git ~/inkboard
   cd ~/inkboard
   just setup-server
   ```

   `just setup-server` checks for Docker Compose, installs the Python dependencies, creates
   `server/.env` from `server/.env.example`, lists the keys that are still empty and runs the
   server tests (no network needed).

2. **Fill in `server/.env`.** It is gitignored; never commit it.

   | Variable | Where to get it |
   |---|---|
   | `FRED_API_KEY` | Free key from [fredaccount.stlouisfed.org/apikeys](https://fredaccount.stlouisfed.org/apikeys) (market data). Weather (Open-Meteo) needs no key. |
   | `CLOUDFLARE_TUNNEL_TOKEN` | Cloudflare Zero Trust → Networks → Tunnels: the token after `--token`. One-time tunnel, DNS, cache and bot settings: [docs/cloudflare-tunnel.md](docs/cloudflare-tunnel.md). |
   | `LOG_LEVEL` | Optional, default `INFO`. |

3. **Try it locally:** `just dev` serves on port 8765 with auto-reload and reads
   `server/.env`. It never touches production. Preview a frame at
   `http://127.0.0.1:8765/v1/frame.png?w=market_trends:2/3,calendar_weather:1/3&lat=34.05&lon=-118.24&tz=America/Los_Angeles`.
   To point a board at it, use `just flash-dev` (see [docs/dashboard-bringup.md](docs/dashboard-bringup.md)).

4. **Deploy:** `just up` builds and starts the server and `cloudflared` from a clean checkout
   of `main`, and refuses to run without both keys. Then:

   ```bash
   just check           # local + public health checks, opens the public frame
   just logs cloudflared   # expect "Registered tunnel connection"
   just down            # stop (the public site goes offline)
   ```

   The containers restart on their own after a reboot (`restart: unless-stopped`).

**Hosting under a different domain:** change `SERVER_URL` in `include/config.h` (and have
your board owners reflash), `public_url` in the `justfile`, and the tunnel's public hostname.
The firmware trusts only the root CAs in `include/ca_certs.h` (the ones Cloudflare's edge
uses); regenerate it with `tools/gen_ca_certs.sh` if your certificate chains to another root.

## Change the dashboard layout

The board sends its whole layout to the server as a query string: `FRAME_QUERY` in
`include/config.h`. The server keeps nothing per board, so a new layout only needs a
reflash, not a server change.

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
3. **Preview it in a browser** before flashing:
   `https://inkboard.signalwave.dev/v1/frame.png?<your FRAME_QUERY>`. A mistake returns a
   one-line error instead of an image (e.g. `lat: not used by any widget in w`). The
   server's home page, `https://inkboard.signalwave.dev/`, lists every widget and series.
4. **Flash** with `just flash`. The new layout shows on the next update.

Examples:

```text
w=calendar_weather:1&lat=40.7&lon=-74.0&tz=America/New_York&units=metric
w=market_trends:1&series=sp500,btc,ust10y,usd_broad&years=10&tz=America/Chicago
w=calendar_weather:1/3,market_trends:2/3&lat=51.5&lon=-0.1&tz=Europe/London&units=metric&series=sp500,ust10y
```

New widget types or market series are server changes: widgets live in
`server/inkboard_server/widgets/` and series in `server/inkboard_server/series.py`. If you run
the server, also update `query` in the `justfile` (used by `just check`) when you change the
default layout.

## Repository layout

```
VERSION              release version (dashboard footer, board User-Agent); dev builds add -<git hash>
platformio.ini       build config (pinned platform + libraries)
justfile             common commands: `just` lists them (test, up, check, preview, flash, ...)
include/pins.h       every GPIO assignment, mirrors docs/wiring.md
src/                 dashboard firmware (thin client, see docs/dashboard-bringup.md)
extras/smoke/        hardware smoke test + refresh soak test (pio run -e smoke)
extras/minimal/      bare display check (pio run -e minimal), mirrors the kit's firmware
server/              render server (Python): widgets, data sources, HTTP API
docs/
  hardware.md        what each board does, how the e-paper HAT works
  wiring.md          connection table, pin budget, battery + speaker plans
  smoke-test.md      flashing, expected output, troubleshooting
  toolchain.md       language / framework choice and commands
  cloudflare-tunnel.md  publishing the server at inkboard.signalwave.dev
```

## Roadmap

- [x] Hardware smoke test
- [ ] Wi-Fi provisioning + NTP clock
- [x] Dashboard layout engine (server-rendered widgets, see server/)
- [x] Data sources (FRED markets, Open-Meteo weather)
- [x] Deep-sleep update cycle (battery voltage still to do)
- [ ] Speaker output (I2S)
- [ ] OTA updates
