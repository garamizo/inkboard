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
chip then stays in the ROM bootloader until the next reset.

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
| Full refresh | About 4 s with flashing. Clears ghosting; run one periodically (e.g. every ~20 partial updates or hourly). |
| Partial refresh | Under 1 s without flashing. Rewrites a window, but ghosting builds up. |
| Power | Draws power only while refreshing. The image stays with zero power. |

**Care:** don't leave the panel powered in the middle of a refresh for long, don't expose it to
direct sunlight or UV for long periods, and store it showing a white screen if it will sit
unused for months.

Specs: [7.5inch e-Paper V2 specification (PDF)](https://files.waveshare.com/upload/6/60/7.5inch_e-Paper_V2_Specification.pdf).

## Design notes for battery life

An e-paper dashboard can run for months on a battery because nothing draws power between
updates:

1. Wake from deep sleep on a timer (the C6 draws a few µA in deep sleep, but a clone board's
   regulator and LEDs may draw more; measure it).
2. Connect to Wi-Fi, fetch data, and render.
3. `display.hibernate()`, then PWR LOW.
4. Go back to deep sleep.
