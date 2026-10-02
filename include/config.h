#pragma once
// Board configuration (tracked). Wi-Fi credentials and the FRED key live in secrets.h (gitignored).

// Firmware version, shown in the dashboard footer and sent as the User-Agent. Set by
// tools/version.py from the repo's VERSION file; `just flash-dev` adds -<git hash>.
#ifndef INKBOARD_VERSION
#define INKBOARD_VERSION "dev"
#endif

// This board's layout and options (README "Change the dashboard layout"). Preview it on a
// computer with: just preview
#define FRAME_QUERY \
  "w=market_trends:2/3,calendar_weather:1/3&lat=34.05&lon=-118.24&tz=America/Los_Angeles&units=imperial"

// For bring-up (docs/dashboard-bringup.md step 1): 1 draws the calibration pattern instead of the dashboard.
#define USE_CALIBRATION_PATTERN 0

// 1 (prod): deep-sleep between updates unless a computer is on USB. `just flash-dev` builds
// with -DINKBOARD_DEEP_SLEEP=0: the board never deep-sleeps, so USB and logs stay available.
#ifndef INKBOARD_DEEP_SLEEP
#define INKBOARD_DEEP_SLEEP 1
#endif

// Sleep after a config error or while showing the calibration pattern.
#define FALLBACK_SLEEP_S 3600

// The renderer uses bit 1 = white, the same as GxEPD2's own buffer, so no inversion is
// expected. Confirmed with the calibration pattern in docs/dashboard-bringup.md.
#define FRAME_INVERT false
