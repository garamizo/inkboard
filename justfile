# inkboard: firmware (PlatformIO) + render server (server/). `just` lists everything.

public_url := "https://inkboard.signalwave.dev"
local_url := "http://127.0.0.1:18440"
# The default board layout (same as FRAME_QUERY in the firmware config).
query := "w=market_trends:2/3,calendar_weather:1/3&lat=34.05&lon=-118.24&tz=America/Los_Angeles&units=imperial"

default:
    @just --list

# --- server: develop --------------------------------------------------------

# Install / update the server's Python environment.
sync:
    cd server && uv sync

# Server test suite (no network). Extra args go to pytest, e.g. `just test -k market`.
test *ARGS:
    cd server && uv run pytest -q {{ARGS}}

# Rewrite golden PNGs after an intended visual change; review the diffs before committing.
goldens *ARGS:
    cd server && uv run pytest -q --update-goldens {{ARGS}}

# Run the server with auto-reload on :8765 (reads FRED_API_KEY from server/.env if present).
dev:
    cd server && set -a && { [ -f .env ] && . ./.env || true; } && set +a && \
      uv run uvicorn inkboard_server.main:app --reload --port 8765

# Re-record the upstream fixtures the tests use (network; commit the JSON afterwards).
record-fixtures:
    cd server && uv run python tests/fixtures/record_fixtures.py

# --- server: deploy (Docker Compose + Cloudflare Tunnel, see docs/cloudflare-tunnel.md) ---

# Build and start the server and its cloudflared connector.
up:
    #!/usr/bin/env bash
    set -euo pipefail
    cd server
    [[ -f .env ]] || { echo "Copy server/.env.example to server/.env and fill it in (docs/cloudflare-tunnel.md)." >&2; exit 1; }
    set -a; . ./.env; set +a
    for name in FRED_API_KEY CLOUDFLARE_TUNNEL_TOKEN; do
      [[ -n "${!name:-}" ]] || { echo "Set $name in server/.env." >&2; exit 1; }
    done
    docker compose up -d --build
    docker compose ps

# Stop the stack (the public site goes offline; the cache volume is kept).
down:
    cd server && docker compose down

# Rebuild and restart after a code change.
restart: up

# Follow logs, e.g. `just logs cloudflared`.
logs SERVICE="":
    cd server && docker compose logs -f {{SERVICE}}

# Container status.
ps:
    cd server && docker compose ps

# Health checks: local server, public tunnel, and edge cache bypass.
check:
    #!/usr/bin/env bash
    set -uo pipefail
    echo "local  /healthz: $(curl -s --max-time 5 {{local_url}}/healthz || echo DOWN)"
    echo "public /v1/test.png: $(curl -s -o /dev/null --max-time 10 -w '%{http_code}' {{public_url}}/v1/test.png)"
    curl -s -o /dev/null -D - --max-time 20 "{{public_url}}/v1/frame.png?{{query}}" \
      | grep -iE '^(HTTP|cf-cache-status|etag|x-next-refresh-seconds)' || echo "public frame: no response"

# Fetch the default frame (local stack, or `just preview public`) and open it.
preview WHERE="local":
    #!/usr/bin/env bash
    set -euo pipefail
    base={{ if WHERE == "public" { public_url } else { local_url } }}
    mkdir -p server/.cache
    curl -sf --max-time 25 -o server/.cache/preview.png "$base/v1/frame.png?{{query}}"
    echo "wrote server/.cache/preview.png"
    xdg-open server/.cache/preview.png >/dev/null 2>&1 &

# --- firmware (PlatformIO) --------------------------------------------------

# Build the default firmware.
build:
    pio run

# Flash the default firmware and open the serial monitor.
flash:
    pio run -t upload -t monitor

# Flash the hardware smoke test (see docs/smoke-test.md).
smoke:
    pio run -e {{ if path_exists("extras/smoke") == "true" { "smoke" } else { "supermini-c6" } }} -t upload -t monitor

# Flash the bare display check (extras/minimal).
minimal:
    pio run -e minimal -t upload -t monitor

# Serial console.
monitor:
    pio device monitor
