# Smoke test: is the hardware OK?

`src/main.cpp` checks each part of the kit and reports over USB serial. Run it before you
assemble the case, while the wires are still easy to reach.

## Steps

1. Wire the boards as in [wiring.md](wiring.md). Set the HAT switches to **Display Config = B**
   and **Interface Config = 0**, and latch the ribbon cable.
2. Plug the SuperMini into your PC over USB-C. It should appear as `/dev/ttyACM0`.
3. Flash and watch:

   ```bash
   pio run -t upload -t monitor
   ```

   If the upload can't connect, hold **BOOT**, tap **RESET**, release **BOOT**, and retry.
   After a manual bootloader entry, tap RESET once more after flashing to start the firmware.

## Expected serial output

```
inkboard smoke test

=== MCU ===
Chip: ESP32-C6 rev 1, 1 core(s) @ 160 MHz
Flash: 4096 KB, free heap: ...
MAC: xx:xx:xx:xx:xx:xx
LED: status LED blinks, RGB LED cycles red -> green -> blue

=== Wi-Fi scan ===
PASS: 7 network(s) found
   -48 dBm  MyHomeWifi
   ...

=== e-Paper wiring ===
BUSY with HAT power off: floating
BUSY with HAT power on, after reset: driven HIGH
PASS: controller is powered and idle

=== e-Paper panel ===
Full refresh (all black): ~4000 ms
Full refresh (test pattern): ~4000 ms
Partial refresh 1: ~600-1000 ms
...
Display test done.
```

**Warning signs:** a refresh that finishes in a few milliseconds means the firmware isn't
waiting on BUSY (BUSY wiring). A refresh that takes about 10-20 s and prints `Busy Timeout!`
means BUSY never came back (power, ribbon or switches).

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
| No `/dev/ttyACM0` | Charge-only USB cable; try another cable or port. |
| `Permission denied` on the port | You are not in the `dialout` group (see [toolchain.md](toolchain.md)). |
| No serial output after flashing | Press RESET. The monitor attached after the first lines were printed. |
| `nothing drives BUSY` (floating) | BUSY wire not on GPIO3, or the controller is unpowered: measure 3.3 V between HAT VCC and GND, check PWR → GPIO1, reseat the ribbon. |
| `BUSY held LOW` | Controller stuck in reset/busy (check RST → GPIO2), a solder bridge to GND, or wrong switch settings. |
| `IO 20 is not set as GPIO` log errors | Harmless GxEPD2 quirk, fixed in current firmware. |
| `controller ignores commands` / panel stays blank with ~2 µs `_Update_Full` | The controller never got the command. The test then tries 3-wire mode and every DIN/CLK/CS/DC order and prints the one that works. If none works: the ribbon isn't seated, VCC is missing, or the controller is damaged. |
| BUSY PASS even with the HAT unpowered | The HAT pulls BUSY up itself, so that check alone can't prove the controller is alive; the command test does. |
| Faint or washed-out image | Display Config set to A (should be B). |
| Image garbled or shifted | Wrong panel driver. A 7.5" V1 (640×384) or HD (880×528) needs a different GxEPD2 class. |
| Wi-Fi finds 0 networks | Antenna or radio problem. The SuperMini's ceramic antenna is weak inside some cases; test outside the case too. |
