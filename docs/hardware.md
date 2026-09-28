# Hardware

inkboard runs on the [Smart E-Paper Desk Dashboard](https://makerworld.com/en/models/2443888-smart-e-paper-desk-dashboard-esp32-7-5)
kit: a 3D-printed case holding the three boards below. For how they are wired together, see
[wiring.md](wiring.md).

```
  ┌───────────────┐   SPI + control (7 wires)  ┌──────────────────┐  24-pin FFC  ┌─────────────────┐
  │ ESP32-C6      │───────────────────────────▶│ e-Paper Driver   │─────────────▶│ 7.5" e-Paper V2 │
  │ SuperMini     │◀─────────── BUSY ──────────│ HAT (rev 2.3)    │              │ 800×480 B/W     │
  │               │── 3V3 / GND ──────────────▶│                  │              │ UC8179 ctrl     │
  └──────┬────────┘                            └──────────────────┘              └─────────────────┘
         │ USB-C (power, flashing, serial)
         │ B+/B- pads → LiPo (optional)
```

## ESP32-C6 SuperMini

A thumb-sized clone board built around Espressif's ESP32-C6.

| | |
|---|---|
| CPU | Single-core RISC-V, 160 MHz |
| Memory | 512 KB SRAM, 4 MB flash |
| Radios | Wi-Fi 6 (2.4 GHz), Bluetooth 5 LE, 802.15.4 (Zigbee / Thread / Matter) |
| USB | Native USB-Serial-JTAG on USB-C. Flashing and the serial console need no adapter chip. |
| On board | WS2812 RGB LED (GPIO8), status LED (GPIO15), BOOT button (GPIO9), RESET button |
| Power | USB-C 5 V → 3.3 V regulator; LiPo charger feeding the B+/B- pads |

**BOOT and RESET:** if an upload fails to connect, hold BOOT, tap RESET, and release BOOT. The
chip then stays in the ROM bootloader until the next RESET press or power cycle: the strap
pins are latched only on a chip reset, so esptool's USB reset after the upload doesn't leave
the bootloader. Tap RESET after flashing.

## Waveshare e-Paper Driver HAT (rev 2.3)

The panel is not a "smart" display: it is a bare glass panel whose controller chip (UC8179)
sits on the ribbon cable. The panel needs high voltages (roughly ±15 V and more for the gate
drivers) to move the ink particles, and the 3.3 V ESP32 cannot supply them. The HAT is mostly
the power stage that makes those voltages. Its main blocks are:

1. **Boost converter.** An inductor, a MOSFET and diodes. The *panel's controller* switches
   the MOSFET through its GDR pin and senses current through a resistor (the RESE pin). The
   **Display Config** switch picks that sense resistor (A = 3 Ω, B = 0.47 Ω). A larger panel
   like the 7.5" needs more current, hence **B**.
2. **Charge pumps.** Capacitor and diode ladders that turn the boost output into the positive
   and negative gate and source voltages (VGH/VGL, VSH/VSL).
3. **Power switch (the PWR pin).** Added in rev 2.3: a MOSFET that switches the HAT's
   supply. PWR HIGH = powered. Driving it LOW between updates removes all leakage, which
   matters on battery.
4. **Interface Config switch.** Chooses 4-wire SPI (0, separate DC line) or 3-wire SPI (1, the
   D/C bit sent in-band). We use 0.
5. **Pass-through signals.** DIN, CLK, CS, DC, RST and BUSY go straight to the FPC connector
   and the panel's controller.

Waveshare's schematic and manual are on the
[E-Paper Driver HAT wiki](https://www.waveshare.com/wiki/E-Paper_Driver_HAT).

## 7.5" e-Paper V2 panel

| | |
|---|---|
| Resolution | 800 × 480, 1 bit (black / white) |
| Controller | UC8179 (GxEPD2 class `GxEPD2_750_T7`) |
| Full refresh | About 3 s with flashing (measured: 3.1-3.4 s). Clears ghosting; run one periodically (e.g. every ~20 partial updates or hourly). |
| Partial refresh | About 1.2 s without flashing (measured). Rewrites a window, but ghosting builds up. |
| Power | Draws power only while refreshing. The image stays with zero power. |

**Care:** don't leave the panel powered in the middle of a refresh for long, don't expose it to
direct sunlight or UV for long periods, and store it showing a white screen if it will sit
unused for months.

Specs: [7.5inch e-Paper V2 specification (PDF)](https://files.waveshare.com/upload/6/60/7.5inch_e-Paper_V2_Specification.pdf).

## Design notes for battery life

An e-paper dashboard can run for months on a battery because nothing draws power between
updates:

1. Wake from deep sleep on a timer. The C6 chip draws a few µA in deep sleep, but the
   SuperMini's regulator is reported to draw about 300-400 µA on its own. That alone empties a
   1000 mAh cell in roughly 4 months, before any updates. Measure your board. If battery life
   matters, replace the regulator with a low-quiescent one or bypass it.
2. Connect to Wi-Fi, fetch data, and render.
3. `display.hibernate()`, then PWR LOW.
4. Go back to deep sleep.

## Battery power path (verify before relying on it)

What is known about the SuperMini's battery circuit:

- The B+/B- pads are fed by a linear Li-ion charger (reported as a TP4054) that charges from
  USB 5 V. The charge current is set by a resistor on the board and isn't documented; the
  TP4054 family tops out around 500 mA, so use a cell of 500 mAh or more.
- One pinout write-up warns: "Do not connect USB and external battery simultaneously without a
  protection diode." That seems to conflict with having a charger at all, and suggests some
  batches wire B+ straight to the regulator input with no power-path switching.

To check your own board, before soldering a cell (multimeter, USB unplugged):

1. Test continuity from **B+** to the **5V** pin, and from B+ to the regulator's input pin.
2. **B+ goes to the regulator through a diode or MOSFET** (you see a diode drop in diode mode,
   or no continuity): USB and battery can be connected together. USB then powers the board
   and charges the cell. This is the normal setup.
3. **B+ is shorted straight to 5V:** don't connect USB and battery at the same time. USB would
   push 5 V into the cell. Add a Schottky diode from USB 5V to the regulator input, or use an
   external charger/power-path module.

## Kit reference firmware

The kit's designer publishes firmware at
[VoIPshare/ESP32-eInk-Dashboard](https://github.com/VoIPshare/ESP32-eInk-Dashboard) (Arduino
CLI, GxEPD2 `GxEPD2_750_T7`, `init(115200, true, 2, false)`). It is useful as a reference for
features (Home Assistant, Zigbee, Bambu Lab, Proxmox widgets). Its pin map differs from
inkboard's; see [wiring.md](wiring.md).
