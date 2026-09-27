#pragma once
// Pin map: ESP32-C6 SuperMini <-> Waveshare e-Paper Driver HAT (rev 2.3, 9-pin).
// See docs/wiring.md for the diagram and the reasoning behind each choice.

// --- e-Paper Driver HAT -----------------------------------------------------
// SPI is routed through the GPIO matrix, so any free pins work. These keep
// the ADC pins (GPIO0-6), the strapping pins (4, 5, 8, 9, 15) and USB (12, 13)
// free where possible.
constexpr int PIN_EPD_DIN  = 18;  // SPI MOSI
constexpr int PIN_EPD_CLK  = 19;  // SPI SCK
constexpr int PIN_EPD_CS   = 20;  // SPI chip select, active LOW
constexpr int PIN_EPD_DC   = 14;  // data (HIGH) / command (LOW)
constexpr int PIN_EPD_RST  = 2;   // controller reset, active LOW
constexpr int PIN_EPD_BUSY = 3;   // input: LOW while the panel is refreshing
constexpr int PIN_EPD_PWR  = 1;   // HIGH switches on the HAT's panel supply

// --- On-board -----------------------------------------------------------------
constexpr int PIN_LED_STATUS = 15;  // blue status LED (also a strapping pin)
constexpr int PIN_LED_RGB    = 8;   // WS2812 addressable LED (also a strapping pin)
constexpr int PIN_BOOT_BTN   = 9;   // BOOT button, LOW when pressed

// --- Reserved for later (not wired yet) --------------------------------------
// Battery sense: B+ -> 100k -> GPIO0 -> 100k -> GND (reads half the battery voltage).
constexpr int PIN_BATTERY_ADC = 0;
// I2S speaker amp (e.g. MAX98357A): BCLK / LRCLK / DIN.
constexpr int PIN_I2S_BCLK = 6;
constexpr int PIN_I2S_LRC  = 7;
constexpr int PIN_I2S_DOUT = 5;
