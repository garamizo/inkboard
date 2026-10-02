# Dashboard bring-up

Checks the firmware on real hardware. Do them in order; record results in the table at the end.

**Before you start:**
- `include/secrets.h` has your Wi-Fi credentials and your FRED API key (copy it from
  `include/secrets.h.example`; the key is free at https://fredaccount.stlouisfed.org/apikeys).
- The serial monitor shows `sleeping <s> s` after each wake.
- `just flash-dev` builds with deep sleep **off** (`INKBOARD_DEEP_SLEEP=0`): the dev board
  waits awake between updates, so USB and logs are always there. Prod (`just flash`) keeps
  deep sleep on.
- While a computer is on its USB, the board stays awake between updates, so `just flash`,
  `just flash-dev` and `just monitor` work any time. On battery or a charger it deep-sleeps,
  and its USB port is off. A computer plugged in during sleep is noticed at the next wake.
  `just flash-dev` waits for that wake by itself. To skip the wait, tap RESET (not BOOT): the
  board wakes at once and stays awake for the upload.
- If uploads succeed but the board never starts the firmware (serial shows only
  `wait usb download`), it's stuck in the USB downloader: tap RESET. See the troubleshooting
  table in `docs/smoke-test.md`.
- RESET (or a re-flash) keeps the board's wake state, so it behaves like an early timer wake.
  Unplugging the power clears that state: the first successful wake after power-up redraws
  the stored frame once, because the board can't know what the panel shows.
- Press RESET to run a wake immediately. The shortest wait between wakes is 300 s
  (`wake::MIN_SLEEP_S`).

## 1. Orientation and bit order
Set `USE_CALIBRATION_PATTERN 1` in `include/config.h`, then flash (`just flash` or `just flash-dev`).
- A solid square is **top-left**, a dot is **bottom-right**, and the brackets are at the
  other two corners.
- The text reads normally (not mirrored), and it is black on white.
- If the colours are inverted, set `FRAME_INVERT true` and repeat.
Then set `USE_CALIBRATION_PATTERN 0` again.

## 2. First frame
Flash again. The panel should match `just preview` (`.pio/preview.png`), and serial should
show the weather and FRED fetches succeeding and then `sleeping <s> s`.

## 3. Cached data
Press RESET within the same data cycle (a few minutes after step 2). Serial should show no
fetches (the caches in flash are fresh), and the panel shows the same dashboard.

## 4. Offline badge
Turn off the Wi-Fi router and let the board wake three times.
- That takes 5 + 15 + 60 min; pressing RESET runs a wake immediately.
- On the third failure the **"offline since h:mm"** badge appears bottom-right, over the
  intact last dashboard.
- Power-cycle the board (unplug it) while it's still offline. The stored frame survives,
  and after three more failures the badge is drawn over it again. Its time now says just
  "offline", because RTC memory was lost.
Turn the Wi-Fi back on, then press RESET: the next wake redraws the clean frame without the
badge.

## 5. Config error
Set `FRAME_QUERY` to `"w=market_trends:2/3"` and flash. The error screen shows
"w: sizes add up to 2/3, need 3/3". Press RESET: the screen is not redrawn. Restore
`FRAME_QUERY` and flash again.

## 6. Power
Measure the current with a USB power meter or a meter on B+:
- deep sleep: ___ µA
- awake per cycle: ___ s at ___ mA

Record both in `docs/hardware.md` (Design notes for battery life).

## Results

| Step | Date | Result | Notes |
|---|---|---|---|
| 1 Orientation | | | |
| 2 First frame | | | |
| 3 Cached data | | | |
| 4 Offline badge | | | |
| 5 Config error | | | |
| 6 Power | | | |
