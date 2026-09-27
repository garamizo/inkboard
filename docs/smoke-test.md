# Smoke test: is the hardware OK?

`src/main.cpp` checks each part of the kit and reports over USB serial. Run it before you
assemble the case, while the wires are still easy to reach.

## Steps

1. Wire the boards as in [wiring.md](wiring.md). Set the HAT switches to **Display Config = B**
   and **Interface Config = 0**, and latch the ribbon cable. **Check the ribbon orientation**:
   an upside-down ribbon was the fault on our first bring-up (see Troubleshooting).
2. Plug the SuperMini into your PC over USB-C. It should appear as `/dev/ttyACM0`.
3. Flash and watch:

   ```bash
   pio run -t upload -t monitor
   ```

   If the upload can't connect, hold **BOOT**, tap **RESET**, release **BOOT**, and retry.
   After a manual bootloader entry, tap RESET once more after flashing to start the firmware.

If the full test misbehaves, `pio run -e minimal -t upload -t monitor` flashes a bare
display check (`extras/minimal/`) that follows the kit's reference firmware. If that also
fails, the problem is the hardware, not the smoke test.

## Expected serial output

From a healthy unit (2026-09-27, trimmed):

```
=== MCU ===
Chip: ESP32-C6 rev 2, 1 core(s) @ 160 MHz
Flash: 4096 KB, free heap: 327748 B
MAC: xx:xx:xx:xx:xx:xx
LED: status LED blinks, RGB LED cycles red -> green -> blue

=== Wi-Fi scan ===
PASS: 14 network(s) found
   -25 dBm  ...

=== e-Paper wiring ===
BUSY with HAT power off: floating
BUSY after reset: LOW for 1 ms, then driven HIGH
PASS: controller idle after reset

=== e-Paper commands ===
PASS: controller answered the power-on command

=== e-Paper panel ===
_PowerOn : 133000
_Update_Full : 2634001
_PowerOff : 29001
Full refresh (all black): 3382 ms
Full refresh (test pattern): 3082 ms
_Update_Part : 1162001
Partial refresh 1: 1322 ms
Partial refresh 2: 1188 ms
...
Display test done.
```

The `_PowerOn` / `_Update_*` lines come from GxEPD2 and are in µs: power-on ~130 ms, full
update ~2.6 s, partial update ~1.2 s.

**Warning signs:** a GxEPD2 timing of a few µs (`_Update_Full : 2`) means the controller
never went busy, so it didn't get the command. `Busy Timeout!` with ~10000000 µs means BUSY
never came back HIGH, and the most likely cause is the ribbon (see below).

## What to look for on the panel

The panel goes **all black**, then shows the **test pattern**:

| Element | Checks | Damage looks like |
|---------|--------|-------------------|
| Double border at the very edge | Every row and column driver at the extremes | A missing edge line |
| Checkerboard (left) | Row/column drivers across the middle | Broken, smeared or shifted stripes |
| Line ladder 1-8 px (center) | Fine detail, SPI signal integrity | Jagged or missing thin lines, random noise |
| Solid black block and ring (right) | Full-ink areas | Grey patches, speckles, a "crack" line |
| Corner labels TR / BL / BR | Orientation and edges | Text cut off |
| Counter box (bottom) | Partial refresh | It should count 1→5 without the whole screen flashing |

A cracked panel usually shows a permanent line or a region that never changes. A
connector problem shows up as whole missing stripes, or as noise that changes between
refreshes. Reseat the ribbon and try again before you conclude the panel is damaged.

Press **BOOT** to repeat the display test.

## Troubleshooting

| Symptom | Likely cause |
|---------|--------------|
| `Busy Timeout!` on `_PowerOn`, BUSY `driven LOW` / `still LOW 2 s after reset`, panel blank | **Ribbon cable upside down.** This was the fault on our first bring-up: the controller gets power but its boost converter can't start, so it never releases BUSY. Flip the ribbon, latch it, and retest. Only after that suspect Display Config or a damaged panel. |
| No `/dev/ttyACM0` | Charge-only USB cable; try another cable or port. |
| `Permission denied` on the port | You are not in the `dialout` group (see [toolchain.md](toolchain.md)). |
| No serial output after flashing | Press RESET. The monitor attached after the first lines were printed. |
| `nothing drives BUSY` (floating) | BUSY wire not on GPIO3, or the controller is unpowered: measure 3.3 V between HAT VCC and GND, check PWR → GPIO1, reseat the ribbon. |
| `BUSY held LOW` / `still LOW` | Ribbon upside down (first row), RST → GPIO2 not connected, a solder bridge to GND, or wrong switch settings. |
| `IO 20 is not set as GPIO` log errors | Harmless: GxEPD2 writes CS/DC/RST before configuring them. Current firmware configures them first. |
| `controller ignores commands` / panel stays blank with ~2 µs `_Update_Full` | The controller never got the command. The test then tries 3-wire mode and every DIN/CLK/CS/DC order and prints the one that works. If none works: check that each wire goes where [wiring.md](wiring.md) says (the kit's own firmware uses a different pin map), then the ribbon and VCC. |
| BUSY PASS even with the HAT unpowered | The HAT pulls BUSY up itself, so that check alone can't prove the controller is alive; the command test does. |
| Faint or washed-out image | Display Config set to A (should be B). |
| Image garbled or shifted | Wrong panel driver. A 7.5" V1 (640×384) or HD (880×528) needs a different GxEPD2 class. |
| Wi-Fi finds 0 networks | Antenna or radio problem. The SuperMini's ceramic antenna is weak inside some cases; test outside the case too. |
