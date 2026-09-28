# inkboard server

Renders 800×480 1-bit frames for inkboard e-paper boards. Each board sends its layout in the
query string; the server keeps no per-device state. Design: `../docs/superpowers/specs/2026-09-27-dashboard-design.md`.

## Develop

    uv sync
    uv run pytest                     # all tests, no network
    uv run pytest --update-goldens    # after an intended visual change; review the PNG diffs
    FRED_API_KEY=... uv run uvicorn inkboard_server.main:app --reload --port 8765

Preview in a browser:
http://127.0.0.1:8765/v1/frame.png?w=market_trends:2/3,calendar_weather:1/3&lat=34.05&lon=-118.24&tz=America/Los_Angeles

## Deploy

    cp .env.example .env              # set FRED_API_KEY and CLOUDFLARE_TUNNEL_TOKEN
    docker compose up -d --build
    docker compose logs -f cloudflared   # "Registered tunnel connection"

Public traffic arrives through the Cloudflare Tunnel (`cloudflared` service); the server
itself is only published on 127.0.0.1:18440 for local checks. One-time Cloudflare setup,
cache and bot settings, and checks: `../docs/cloudflare-tunnel.md`. The cache lives in the
`cache` volume.

## Endpoints

| Path | What |
|---|---|
| `/v1/frame.bin?…` | 48,000-byte packed frame (ETag / If-None-Match → 304) |
| `/v1/frame.png?…` | same frame as PNG |
| `/v1/test.bin`, `/v1/test.png` | hardware calibration pattern |
| `/healthz` | source cache status |
| `/` | widget and series reference |
