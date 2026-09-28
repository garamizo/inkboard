#pragma once
// Board configuration (tracked). Wi-Fi credentials live in secrets.h (gitignored).

// https:// uses TLS with the CA bundle in ca_certs.h. http:// works for a dev server on
// the LAN, e.g. "http://192.168.1.20:8765" with `uvicorn ... --host 0.0.0.0 --port 8765`.
#ifndef SERVER_URL  // `just flash dev` injects the LAN dev server with -DSERVER_URL=...
#define SERVER_URL "https://inkboard.signalwave.dev"
#endif

// This board's layout and options (spec §2.2). Preview it in a browser at
// SERVER_URL/v1/frame.png?<FRAME_QUERY>.
#define FRAME_QUERY \
  "w=market_trends:2/3,calendar_weather:1/3&lat=34.05&lon=-118.24&tz=America/Los_Angeles&units=imperial"

// For bring-up (docs/dashboard-bringup.md step 1): 1 fetches /v1/test.bin instead of the dashboard.
#define USE_CALIBRATION_PATTERN 0

// Sleep when the server gives no X-Next-Refresh-Seconds, and after a config error.
#define FALLBACK_SLEEP_S 3600

// Server frames use bit 1 = white, the same as GxEPD2's own buffer, so no inversion is
// expected. Confirmed with the calibration pattern in docs/dashboard-bringup.md.
#define FRAME_INVERT false
