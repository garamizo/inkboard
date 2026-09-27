// Minimal e-paper check, following the kit's reference firmware
// (github.com/VoIPshare/ESP32-eInk-Dashboard) closely: PWR HIGH,
// 50 ms, SPI.begin, init(115200, true, 2, false), one full refresh.
// No diagnostics of our own, so any failure is the hardware or the pin map.
//
// Pin map: pins.h by default; build env "minimal-kit-pins" uses the kit's.

#include <Arduino.h>
#include <SPI.h>
#include <GxEPD2_BW.h>
#include <Fonts/FreeSansBold18pt7b.h>

#ifdef KIT_PINS
// VoIPshare preset "esp32_c6_supermini" (configure.h)
constexpr int P_CS = 4, P_DC = 20, P_RST = 21, P_BUSY = 22, P_SCK = 7, P_MOSI = 5, P_PWR = 1;
#else
#include "pins.h"
constexpr int P_CS = PIN_EPD_CS, P_DC = PIN_EPD_DC, P_RST = PIN_EPD_RST, P_BUSY = PIN_EPD_BUSY,
              P_SCK = PIN_EPD_CLK, P_MOSI = PIN_EPD_DIN, P_PWR = PIN_EPD_PWR;
#endif

GxEPD2_BW<GxEPD2_750_T7, GxEPD2_750_T7::HEIGHT> display(GxEPD2_750_T7(P_CS, P_DC, P_RST, P_BUSY));

void setup() {
  Serial.begin(115200);
  for (uint32_t t = millis(); !Serial && millis() - t < 3000;) delay(10);
  delay(500);
  Serial.printf("\nminimal e-paper test: CS=%d DC=%d RST=%d BUSY=%d SCK=%d MOSI=%d PWR=%d\n",
                P_CS, P_DC, P_RST, P_BUSY, P_SCK, P_MOSI, P_PWR);

  pinMode(P_PWR, OUTPUT);
  digitalWrite(P_PWR, HIGH);
  delay(50);

  // GxEPD2 writes CS/DC/RST before configuring them, which Arduino-ESP32 3.x
  // logs as an error; configure them first.
  for (int pin : {P_CS, P_DC, P_RST}) {
    pinMode(pin, OUTPUT);
    digitalWrite(pin, HIGH);
  }

  // CS stays a plain GPIO that GxEPD2 toggles itself (the reference passes it
  // to SPI.begin, which hands it to the SPI peripheral).
  SPI.begin(P_SCK, -1, P_MOSI, -1);
  display.init(115200, true, 2, false);
  display.setRotation(0);
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.fillRect(0, 0, 800, 60, GxEPD_BLACK);
    display.setFont(&FreeSansBold18pt7b);
    display.setTextColor(GxEPD_BLACK);
    display.setCursor(250, 260);
    display.print("Hello, inkboard");
  } while (display.nextPage());
  display.hibernate();
  Serial.println("done: a black bar on top and \"Hello, inkboard\" should be visible");
}

void loop() {}
