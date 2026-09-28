# inkboard: firmware (PlatformIO) + render server (server/). `just` lists the recipes.

public_url := "https://inkboard.signalwave.dev"
local_url := "http://127.0.0.1:18440"
# The default board layout (same as FRAME_QUERY in the firmware config).
query := "w=market_trends:2/3,calendar_weather:1/3&lat=34.05&lon=-118.24&tz=America/Los_Angeles&units=imperial"

_default:
    @just --list --unsorted

# Server tests; args go to pytest (e.g. `-k market`, `--update-goldens`).
test *ARGS:
    cd server && uv run pytest -q {{ARGS}}

# Run the server locally with auto-reload on :8765 (uses server/.env if present).
dev:
    cd server && set -a && { [ -f .env ] && . ./.env || true; } && set +a && \
      uv run uvicorn inkboard_server.main:app --reload --port 8765

# Build and start the server + cloudflared (docs/cloudflare-tunnel.md).
up:
    #!/usr/bin/env bash
    set -euo pipefail
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

# Flash firmware and open the serial monitor; ENV: supermini-c6 (default), minimal, ...
flash ENV="supermini-c6":
    pio run -e {{ENV}} -t upload -t monitor

# Serial console.
monitor:
    pio device monitor
