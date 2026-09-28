# inkboard

Firmware for a 7.5" e-paper desk dashboard built on an ESP32-C6. What it shows is still open:
calendar, weather, reminders, home status.

**Hardware:** ESP32-C6 SuperMini + Waveshare e-Paper Driver HAT (rev 2.3) + Waveshare 7.5" V2
(800×480 B/W) in the [Smart E-Paper Desk Dashboard](https://makerworld.com/en/models/2443888-smart-e-paper-desk-dashboard-esp32-7-5)
case.

**Stack:** C++ / Arduino-ESP32 3.x / PlatformIO ([why](docs/toolchain.md)), display via
[GxEPD2](https://github.com/ZinggJM/GxEPD2).

## Quick start

```bash
uv tool install platformio
pio run -t upload -t monitor     # flashes the hardware smoke test
```

Wire it first: [docs/wiring.md](docs/wiring.md). Then check the results against
[docs/smoke-test.md](docs/smoke-test.md).

## Layout

```
platformio.ini       build config (pinned platform + libraries)
include/pins.h       every GPIO assignment, mirrors docs/wiring.md
src/main.cpp         current firmware: hardware smoke test + refresh soak test
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
- [ ] Deep-sleep update cycle + battery voltage
- [ ] Speaker output (I2S)
- [ ] OTA updates
