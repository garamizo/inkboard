# inkboard

7.5" e-paper desk dashboard: ESP32-C6 thin-client firmware (C++ / Arduino-ESP32 3.x /
PlatformIO + pioarduino) and a Python render server (`server/`, FastAPI + Pillow). The server
renders 800×480 1-bit frames; the board fetches, draws and deep-sleeps. Design:
`docs/superpowers/specs/2026-09-27-dashboard-design.md` (section numbers like "spec §6.3" in
code comments refer to it).

## Commands

`just` lists every recipe. The ones you need most:

- `just test [pytest args]`: server tests (no network). `--update-goldens` after an intended
  visual change, then look at the PNGs in `server/tests/goldens/`.
- `pio test -e native`: host-side unit tests for the pure firmware logic in `include/`.
- `pio run -e supermini-c6`: build the dashboard firmware (`-e smoke`, `-e minimal` for the
  hardware checks in `extras/`).
- `just dev`: dev server on :8765 (auto-reload, LAN-reachable, never touches production).
- `just flash-dev [env]`: dev firmware pointed at this machine's `just dev`, deep sleep off.
- `just flash [env]`: production firmware (public API). Needs the board on USB.
- `just up` / `just down` / `just check` / `just logs`: production server (Docker Compose +
  Cloudflare Tunnel). `just up` refuses anything but a clean `main`.

## Rules

- Don't run `just up`, `just down`, or anything that touches the production containers
  unless asked: it is the public site at inkboard.signalwave.dev.
- Flashing and serial reads need the physical board; ask before uploading.
- Secrets are gitignored: `include/secrets.h` (Wi-Fi) and `server/.env` (API keys). Never
  commit them or print their contents.
- `include/pins.h` mirrors `docs/wiring.md`; change both together.
- The firmware keeps logic that can be tested on the host in header-only files in `include/`
  (`wake_logic.h`, `badge.h`, `http_time.h`, `body_reader.h`) with tests in `test/test_logic/`.
  Hardware calls stay in `src/`.

## Gotchas

- **One PlatformIO install.** The uv-installed `pio` and the VS Code extension's copy
  (`~/.platformio/penv`) delete each other's `.pio/build` mid-compile (issue #1). Symptom:
  missing `.d` / `.sconsign` files. `just setup` links the extension's copy.
- **Changing an env var that PlatformIO reads (e.g. `PLATFORMIO_BUILD_FLAGS`,
  `PLATFORMIO_UPLOAD_PORT`) wipes the build folder.** `flash-dev` passes the port with `-p` for
  this reason.
- **The board's USB port only exists while it is awake.** It deep-sleeps unless a computer is
  on USB (checked at each wake). To flash a sleeping board: tap RESET (not BOOT), then flash.
- **Stuck in the USB downloader:** serial shows `rst:0x15 (USB_UART_HPSYS),boot:0x0
  (USB_BOOT) … wait usb download` after every reset. The C6 latches its strap pins (BOOT =
  GPIO9) only on a chip reset (RESET button or power-on); USB resets, including esptool's
  hard reset, reuse the latched value. So after a BOOT + RESET entry every USB reset returns
  to the downloader. Tapping RESET fixes it; no need to unplug the battery. Verified: the
  strap register `GPIO_STRAP_REG` (0x60091038) reads 0x0 when stuck, 0x4 normally.

## Style

- Commit subjects: `<area>: <what>` in plain language (e.g. `flash-dev: …`, `Firmware: …`,
  `Docs: …`).
- Comments say why, not what; match the surrounding density.
- Docs are short and direct: numbered steps, tables for symptom → cause.
