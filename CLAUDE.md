# inkboard

7.5" e-paper desk dashboard: ESP32-C6 firmware (C++ / Arduino-ESP32 3.x / PlatformIO +
pioarduino) that fetches Open-Meteo and FRED and renders 800×480 1-bit frames on the board.
Design: `docs/superpowers/specs/2026-09-30-ondevice-render-design.md` (the widget designs are in
`2026-09-27-dashboard-design.md` §4–§5; section numbers like "spec §6.3" in code comments refer to
those documents).

## Commands

`just` lists every recipe. The ones you need most:

- `pio test -e native` (or `just test`): host-side unit, render and widget tests, no network.
  `INKBOARD_UPDATE_GOLDENS=1 pio test -e native` after an intended visual change, then look at
  the PNGs in `test/goldens/`.
- `just preview ["<query>"] [flags]`: render a layout to `.pio/preview.png` on this machine
  (`just preview "" --fixtures` renders offline).
- `pio run -e supermini-c6`: build the dashboard firmware (`-e smoke`, `-e minimal` for the
  hardware checks in `extras/`).
- `just flash-dev [env]`: dev firmware with deep sleep off. Needs the board on USB.
- `just flash [env]`: production firmware (deep sleep on). Needs the board on USB.

## Rules

- Flashing and serial reads need the physical board; ask before uploading.
- Secrets are gitignored: `include/secrets.h` (Wi-Fi and `FRED_API_KEY`). Never commit them or
  print their contents. Never log a FRED URL: it contains the API key.
- `include/pins.h` mirrors `docs/wiring.md`; change both together.
- The firmware keeps logic that can be tested on the host in header-only files in `include/`
  (`wake_logic.h`, `http_time.h`, `cycle.h`, `data_cache.h`, `civil.h`, `tz.h`, `query.h`, ...)
  and in `include/render/`, `include/widgets/` and `include/sources/`, with tests in `test/`.
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
- **The port can change across a deep sleep.** If a host process (e.g. a serial monitor) keeps the
  old `/dev/ttyACM*` node open, the board re-enumerates as a new node (`ttyACM1`) after waking.
  Glob `ttyACM*` instead of hard-coding the node (spec §8).
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
