# inkboard: firmware (PlatformIO) + render server (server/). `just` lists the recipes.

# On Windows only `flash` and `monitor` are supported (PowerShell); the rest are for the Linux server.
set windows-shell := ["powershell.exe", "-NoLogo", "-Command"]

public_url := "https://inkboard.signalwave.dev"
local_url := "http://127.0.0.1:18440"
# The default board layout (same as FRAME_QUERY in the firmware config).
query := "w=market_trends:2/3,calendar_weather:1/3&lat=34.05&lon=-118.24&tz=America/Los_Angeles&units=imperial"

_default:
    @just --list --unsorted

# Server tests; args go to pytest (e.g. `-k market`, `--update-goldens`).
test *ARGS:
    cd server && uv run pytest -q {{ARGS}}

# Dev server with auto-reload on :8765, reachable on the LAN; never touches production.
dev:
    #!/usr/bin/env bash
    set -euo pipefail
    cd server
    set -a; [[ -f .env ]] && . ./.env; set +a
    ip=$(hostname -I | awk '{print $1}')
    echo "dev server: http://127.0.0.1:8765"
    echo "for a board on the LAN, set SERVER_URL in include/config.h to \"http://$ip:8765\""
    exec uv run uvicorn inkboard_server.main:app --reload --host 0.0.0.0 --port 8765

# Deploy production (server + cloudflared) from a clean checkout of main.
up:
    #!/usr/bin/env bash
    set -euo pipefail
    branch=$(git rev-parse --abbrev-ref HEAD)
    if [[ "$branch" != main || -n "$(git status --porcelain)" ]] && [[ "${INKBOARD_DEPLOY_ANY:-}" != 1 ]]; then
      echo "just up deploys production: run it from a clean checkout of main (on '$branch' now)." >&2
      echo "Use 'just dev' for development. Override once with: INKBOARD_DEPLOY_ANY=1 just up" >&2
      exit 1
    fi
    cd server
    [[ -f .env ]] || { echo "Copy server/.env.example to server/.env and fill it in." >&2; exit 1; }
    set -a; . ./.env; set +a
    for name in FRED_API_KEY CLOUDFLARE_TUNNEL_TOKEN; do
      [[ -n "${!name:-}" ]] || { echo "Set $name in server/.env." >&2; exit 1; }
    done
    docker compose up -d --build
    docker compose ps

# Stop the server (the public site goes offline; the cache is kept).
down:
    cd server && docker compose down

# Follow server logs, e.g. `just logs cloudflared`.
logs SERVICE="":
    cd server && docker compose logs -f {{SERVICE}}

# Health checks (local, public, cache bypass), then open the public frame.
check:
    #!/usr/bin/env bash
    set -uo pipefail
    echo "local  /healthz: $(curl -s --max-time 5 {{local_url}}/healthz || echo DOWN)"
    mkdir -p server/.cache
    curl -s -o server/.cache/preview.png -D - --max-time 25 "{{public_url}}/v1/frame.png?{{query}}" \
      | grep -iE '^(HTTP|cf-cache-status|etag|x-next-refresh-seconds)' || echo "public: no response"
    if file -b server/.cache/preview.png 2>/dev/null | grep -q PNG; then
      echo "frame: server/.cache/preview.png"; xdg-open server/.cache/preview.png >/dev/null 2>&1 &
    else
      echo "frame: not available"
    fi

# Flash + serial monitor (Linux and Windows). ENV: supermini-c6 (dashboard, prod API), smoke, minimal.
flash ENV="supermini-c6":
    pio run -e {{ENV}} -t upload -t monitor

# Dev build against this machine's `just dev` (deep sleep off; Linux). ENV: supermini-c6, smoke.
flash-dev ENV="supermini-c6":
    #!/usr/bin/env bash
    set -euo pipefail
    url="${INKBOARD_DEV_URL:-http://$(hostname -I | awk '{print $1}'):8765}"
    echo "dev firmware -> $url (keep 'just dev' running here)"
    export PLATFORMIO_BUILD_FLAGS="-DSERVER_URL=\\\"$url\\\" -DINKBOARD_DEEP_SLEEP=0"
    pio run -e {{ENV}}
    # The board's USB port only exists while it is awake (a few seconds per wake). Wait for
    # it, then upload at once: esptool resets the chip into the bootloader over USB itself.
    echo "waiting for the board to wake (up to 7 min; or hold BOOT, tap RESET, release BOOT)..."
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
