# Wiring

ESP32-C6 SuperMini ↔ Waveshare e-Paper Driver HAT (rev 2.3, 9-pin header) ↔ 7.5" V2 panel.
The pin numbers here are the source of truth for [`include/pins.h`](../include/pins.h).
If you change one, change the other.

## Connection table

| HAT pin | Wire to (SuperMini) | Direction | What it does |
|---------|---------------------|-----------|--------------|
| VCC     | **3V3**             | power     | Logic and panel supply. Use 3.3 V, not 5 V. |
| GND     | **GND**             | power     | Common ground. |
| DIN     | **GPIO18**          | ESP → HAT | SPI data (MOSI). The panel is write-only, so there is no MISO. |
| CLK     | **GPIO19**          | ESP → HAT | SPI clock. |
| CS      | **GPIO20**          | ESP → HAT | Chip select, active LOW. |
| DC      | **GPIO14**          | ESP → HAT | LOW = the byte is a command, HIGH = the byte is data. |
| RST     | **GPIO2**           | ESP → HAT | Controller reset, active LOW. |
| BUSY    | **GPIO3**           | HAT → ESP | LOW while the controller is refreshing. The firmware waits on it. |
| PWR     | **GPIO1**           | ESP → HAT | HIGH turns on the HAT's panel supply. The firmware drives it LOW after `hibernate()` so the panel draws nothing between updates. |

```
   ESP32-C6 SuperMini (USB-C at top, component side up)

            ┌──────[USB-C]──────┐
   TX  16 ──┤                   ├── 5V
   RX  17 ──┤                   ├── GND ────────────── HAT GND
        0 ──┤ (battery sense*)  ├── 3V3 ────────────── HAT VCC
 HAT PWR  1 ┤                   ├── 20 ─────────────── HAT CS
 HAT RST  2 ┤                   ├── 19 ─────────────── HAT CLK
 HAT BUSY 3 ┤                   ├── 18 ─────────────── HAT DIN
        4 ──┤                   ├── 15  (status LED)
        5 ──┤ (I2S DOUT*)       ├── 14 ─────────────── HAT DC
        6 ──┤ (I2S BCLK*)       ├── 9   (BOOT button)
        7 ──┤ (I2S LRC*)        ├── 8   (RGB LED)
            └───────────────────┘
               B+  B-  ← battery pads on the underside

   * reserved for later, not wired yet
```

Header order follows the common SuperMini layout. Check it against the silkscreen on your
board, because clones differ.

## HAT switches (check these before powering up)

The e-Paper Driver HAT has two slide switches:

| Switch | Set to | Why |
|--------|--------|-----|
| **Display Config** | **B** | Selects the current-sense resistor for the panel's boost converter (A = 3 Ω, B = 0.47 Ω). Waveshare specifies B for the 7.5" V2. The wrong setting gives a faint or failed refresh. |
| **Interface Config** | **0** | 4-wire SPI (with a separate DC line). 1 = 3-wire SPI, which this firmware does not use. |

Seat the ribbon cable fully in the FPC connector, contacts facing the board, and close the
latch. A crooked ribbon is the most common cause of missing stripes or a dead panel.

## Why these pins

The ESP32-C6 routes SPI through its GPIO matrix, so any GPIO works electrically. The choices
above avoid the pins that have side effects:

| GPIO | Constraint | Used for |
|------|-----------|----------|
| 0-6  | Only pins with an ADC (ADC1) | 1-3 for the display (plain digital I/O). **0 is kept for battery sense.** |
| 4, 5 | Strapping (JTAG/SDIO timing at boot) | Left free; 5 is a candidate for I2S output (an output after boot is harmless). |
| 8, 9 | Strapping (boot mode); RGB LED and BOOT button on board | Don't use. |
| 12, 13 | Native USB D-/D+ | Don't use; they carry serial and flashing. |
| 15   | Strapping; status LED | Don't use. |
| 16, 17 | UART0 TX/RX | Free. Serial goes over native USB instead, so these are spare GPIOs. |
| 14, 18, 19, 20 | None | Display SPI + DC. |

### Pin budget for planned features

| Feature | Pins | Status |
|---------|------|--------|
| e-Paper | 1, 2, 3, 14, 18, 19, 20 | wired |
| Battery voltage | 0 (ADC) | planned |
| Speaker (I2S amp such as MAX98357A) | 5, 6, 7 | planned |
| Spare | 4, 16, 17 | free |

## Battery (planned)

The SuperMini has **B+ / B-** pads on its underside, fed by an on-board Li-ion charger that
charges from USB-C. Solder a single-cell LiPo there (B- is GND). Double-check the polarity:
reversing it will destroy the board.

The board has **no on-board divider** for reading the battery voltage. To measure it, add:

```
  B+ ──[100 kΩ]──┬──[100 kΩ]── GND
                 │
               GPIO0      (reads V_bat / 2: 4.2 V full → 2.1 V, 3.3 V empty → 1.65 V)
```

This divider draws a constant ~20 µA from the battery. For a months-long battery life, use 1 MΩ
resistors with a 100 nF capacitor from GPIO0 to GND.

## Speaker (planned)

An I2S class-D amp breakout such as the MAX98357A takes digital audio, so the ESP32 needs no
DAC. Wire BCLK → GPIO6, LRC → GPIO7, DIN → GPIO5, VIN → 5V (USB) or B+, GND → GND, and a
4-8 Ω speaker to its output. Its SD pin can be driven LOW to shut it down for battery life.
