# Dashboard bring-up

Checks the thin-client firmware on real hardware (spec §7.3). Do them in order; record
results in the table at the end.

**Before you start:**
- `include/secrets.h` has your Wi-Fi credentials (copy it from `include/secrets.h.example`).
- A server is reachable. Pick one:
  - **Production:** `just check` shows the public URL working. Flash with `just flash`.
  - **Dev server on the LAN:** run `just dev` on this machine and flash with
    `just flash-dev`, which points the firmware at `http://<this machine's LAN IP>:8765`.
    You don't need to deploy anything or set up the tunnel for this.
- The serial monitor shows `GET -> <status>` and `sleeping <s> s` on each wake.
- Deep sleep drops USB serial. To see the next wake, reopen `just monitor` after the sleep,
  or press RESET to run a wake immediately. The shortest sleep the board ever takes is
  300 s (`wake::MIN_SLEEP_S`).

## 1. Orientation and bit order
Set `USE_CALIBRATION_PATTERN 1` in `include/config.h`, then flash (`just flash` or `just flash-dev`).
- A solid square is **top-left**, a dot is **bottom-right**, and the brackets are at the
  other two corners.
- The text reads normally (not mirrored), and it is black on white.
- If the colours are inverted, set `FRAME_INVERT true` and repeat.
Then set `USE_CALIBRATION_PATTERN 0` again.

## 2. First frame
Flash again. The panel should match `SERVER_URL/v1/frame.png?<FRAME_QUERY>` in a
browser (or `just check`), and serial should show `GET -> 200`.

## 3. Unchanged frame
Press RESET within the same data cycle (a few minutes after step 2). Serial should show
`GET -> 304`, and the panel must not flash.

## 4. Offline badge
Stop the server (stop `just dev`; for production, `just down` on the server) and let the board wake three times.
- That takes 5 + 15 + 60 min; pressing RESET runs a wake immediately.
- On the third failure the **"offline since h:mm"** badge appears bottom-right, over the
  intact last dashboard.
- Power-cycle the board (unplug it) while it's still offline. The stored frame survives,
  and after three more failures the badge is drawn over it again. Its time now says just
  "offline", because RTC memory was lost.
Restart the server, then press RESET: the next wake gets a 304 and redraws the clean frame
without the badge. Repeat once with a power cut while the badge is showing, restarting the
server right away: the first wake after power-up also redraws the clean frame.

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
| 3 Unchanged frame | | | |
| 4 Offline badge | | | |
| 5 Config error | | | |
| 6 Power | | | |
