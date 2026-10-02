# inkboard: e-paper dashboard firmware (PlatformIO). `just` lists the recipes.

# On Windows only `setup`, `flash` and `monitor` are supported (PowerShell).
set windows-shell := ["powershell.exe", "-NoLogo", "-Command"]

_default:
    @just --list --unsorted

# First-time board setup: PlatformIO, include/secrets.h, serial port access. Needs git, just, uv.
[linux]
setup:
    #!/usr/bin/env bash
    set -euo pipefail
    ide_pio="$HOME/.platformio/penv/bin/pio"
    if command -v pio >/dev/null; then
      echo "PlatformIO: $(pio --version)"
    elif [[ -x "$ide_pio" ]]; then
      # Reuse the VS Code extension's copy: two PlatformIO versions wipe each other's builds (#1).
      mkdir -p ~/.local/bin
      ln -sf "$ide_pio" ~/.local/bin/pio
      ln -sf "$HOME/.platformio/penv/bin/platformio" ~/.local/bin/platformio
      echo "PlatformIO: linked the VS Code extension's copy into ~/.local/bin"
    else
      uv tool install platformio
    fi
    if [[ -f include/secrets.h ]]; then
      echo "Wi-Fi: include/secrets.h exists"
    else
      cp include/secrets.h.example include/secrets.h
      echo "Wi-Fi: created include/secrets.h; put your Wi-Fi name and password and your FRED API key in it"
    fi
    if id -nG | grep -qw dialout; then
      echo "Serial: you are in the dialout group"
    else
      echo "Serial: run 'sudo usermod -aG dialout \$USER', then log out and back in"
    fi

# First-time board setup: PlatformIO, include/secrets.h. Needs git, just, uv.
[windows]
setup:
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/setup.ps1

# Host tests (no network, no board); INKBOARD_UPDATE_GOLDENS=1 rewrites the golden PNGs.
test *ARGS:
    pio test -e native {{ARGS}}

# Render a layout to .pio/preview.png on this machine (default: FRAME_QUERY in include/config.h).
# Live data needs FRED_API_KEY (env or include/secrets.h); --fixtures renders offline.
# Several flags go in one quoted string: just preview "" "--fixtures --out x.png"
preview QUERY="" FLAGS="":
    pio run -s -e preview
    .pio/build/preview/program {{ if QUERY == "" { "" } else { "--query " + quote(QUERY) } }} {{FLAGS}}

# Regenerate the bitmap fonts, time-zone table, Pillow reference images, or recorded fixtures.
gen-fonts:
    uv run tools/gen_fonts.py
gen-tz:
    uv run tools/gen_tz_table.py
gen-ca-certs:
    bash tools/gen_ca_certs.sh
gen-primitives:
    uv run tools/ref_primitives.py
record-fixtures:
    uv run tools/record_fixtures.py

# Production build (deep sleep on) + serial monitor (Linux and Windows): ENV supermini-c6 (dashboard) or smoke.
flash ENV="supermini-c6":
    pio run -e {{ENV}} -t upload -t monitor

# Dev build: deep sleep off, version with the git hash (Linux). ENV: supermini-c6, smoke, minimal.
flash-dev ENV="supermini-c6":
    #!/usr/bin/env bash
    set -euo pipefail
    export INKBOARD_VERSION="$(cat VERSION)-$(git rev-parse --short HEAD)"
    echo "dev firmware $INKBOARD_VERSION (deep sleep off)"
    export PLATFORMIO_BUILD_FLAGS="-DINKBOARD_DEEP_SLEEP=0"
    pio run -e {{ENV}}
    # The board's USB port only exists while it is awake (a few seconds per wake). Wait for
    # it, then upload at once: esptool resets the chip into the bootloader over USB itself.
    echo "waiting for the board to wake (up to 7 min; or tap RESET to wake it now)..."
    for _ in $(seq 1 2100); do
      port=$(ls /dev/ttyACM* 2>/dev/null | head -1 || true)
      [[ -n "$port" ]] && break
      sleep 0.2
    done
    [[ -n "$port" ]] || { echo "board never appeared on USB" >&2; exit 1; }
    # -p, not PLATFORMIO_UPLOAD_PORT: a changed env var changes PlatformIO's config checksum,
    # which wipes the build folder. No `-t nobuild` either: on this platform it drops the
    # bootloader/partition/app offsets from the esptool command. The build is up to date,
    # so this only re-checks it before uploading.
    pio run -e {{ENV}} -t upload -t monitor -p "$port"

# Serial console.
monitor:
    pio device monitor
