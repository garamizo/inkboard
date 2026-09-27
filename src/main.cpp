// inkboard - hardware smoke test
//
// Checks each part of the kit and reports over USB serial (115200 baud):
//   1. MCU: chip model, flash, MAC, both on-board LEDs
//   2. Radio: Wi-Fi scan (no credentials needed)
//   3. e-Paper wiring: BUSY line behaviour after power-up and reset
//   4. e-Paper panel: full black, then a test pattern (full refresh),
//      then a counter updated with partial refreshes
// Press BOOT to run the wiring check and display test again.

#include <Arduino.h>
#include <SPI.h>
#include <WiFi.h>
#include <GxEPD2_BW.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSans9pt7b.h>

#include "pins.h"

// Waveshare 7.5" V2 (800x480, UC8179 controller) = GxEPD2_750_T7.
// A full-height frame buffer is 48 KB, which the C6 has room for.
GxEPD2_BW<GxEPD2_750_T7, GxEPD2_750_T7::HEIGHT> display(
    GxEPD2_750_T7(PIN_EPD_CS, PIN_EPD_DC, PIN_EPD_RST, PIN_EPD_BUSY));

static int wifiNetworks = -1;
static String bestSsid;
static int bestRssi = 0;

static void banner(const char* title) {
  Serial.printf("\n=== %s ===\n", title);
}

static void checkMcu() {
  banner("MCU");
  Serial.printf("Chip: %s rev %d, %d core(s) @ %lu MHz\n", ESP.getChipModel(),
                ESP.getChipRevision(), ESP.getChipCores(), ESP.getCpuFreqMHz());
  Serial.printf("Flash: %lu KB, free heap: %lu B\n", ESP.getFlashChipSize() / 1024,
                ESP.getFreeHeap());
  Serial.printf("MAC: %s\n", WiFi.macAddress().c_str());

  Serial.println("LED: status LED blinks, RGB LED cycles red -> green -> blue");
  pinMode(PIN_LED_STATUS, OUTPUT);
  const uint8_t colors[][3] = {{40, 0, 0}, {0, 40, 0}, {0, 0, 40}};
  for (auto& c : colors) {
    digitalWrite(PIN_LED_STATUS, !digitalRead(PIN_LED_STATUS));
    rgbLedWrite(PIN_LED_RGB, c[0], c[1], c[2]);
    delay(400);
  }
  rgbLedWrite(PIN_LED_RGB, 0, 0, 0);
}

static void checkWifi() {
  banner("Wi-Fi scan");
  WiFi.mode(WIFI_STA);
  wifiNetworks = WiFi.scanNetworks();
  if (wifiNetworks < 0) {
    Serial.printf("FAIL: scan error %d\n", wifiNetworks);
  } else {
    Serial.printf("%s: %d network(s) found\n", wifiNetworks > 0 ? "PASS" : "WARN",
                  wifiNetworks);
    for (int i = 0; i < wifiNetworks && i < 5; i++) {
      Serial.printf("  %4d dBm  %s\n", WiFi.RSSI(i), WiFi.SSID(i).c_str());
    }
    if (wifiNetworks > 0) {
      bestSsid = WiFi.SSID(0);  // results are sorted by signal strength
      bestRssi = WiFi.RSSI(0);
    }
  }
  WiFi.scanDelete();
  WiFi.mode(WIFI_OFF);
}

enum class LineState { DrivenHigh, DrivenLow, Floating };

// Reads BUSY once with the internal pull-up and once with the pull-down.
// A line the controller actually drives reads the same both ways; an
// unconnected (or unpowered, high-impedance) line follows the pull.
static LineState probeBusy() {
  pinMode(PIN_EPD_BUSY, INPUT_PULLUP);
  delay(5);
  bool withPullUp = digitalRead(PIN_EPD_BUSY);
  pinMode(PIN_EPD_BUSY, INPUT_PULLDOWN);
  delay(5);
  bool withPullDown = digitalRead(PIN_EPD_BUSY);
  pinMode(PIN_EPD_BUSY, INPUT);
  if (withPullUp && withPullDown) return LineState::DrivenHigh;
  if (!withPullUp && !withPullDown) return LineState::DrivenLow;
  return LineState::Floating;
}

static const char* lineStateName(LineState s) {
  switch (s) {
    case LineState::DrivenHigh: return "driven HIGH";
    case LineState::DrivenLow: return "driven LOW";
    default: return "floating";
  }
}

// The UC8179 drives BUSY HIGH when idle and LOW while working.
static bool checkBusyLine() {
  banner("e-Paper wiring");
  pinMode(PIN_EPD_RST, OUTPUT);
  digitalWrite(PIN_EPD_RST, HIGH);

  digitalWrite(PIN_EPD_PWR, LOW);
  delay(100);
  LineState off = probeBusy();

  digitalWrite(PIN_EPD_PWR, HIGH);
  delay(100);
  digitalWrite(PIN_EPD_RST, LOW);
  delay(10);
  digitalWrite(PIN_EPD_RST, HIGH);
  delay(200);
  LineState on = probeBusy();

  Serial.printf("BUSY with HAT power off: %s\n", lineStateName(off));
  Serial.printf("BUSY with HAT power on, after reset: %s\n", lineStateName(on));

  switch (on) {
    case LineState::DrivenHigh:
      Serial.println("PASS: controller is powered and idle");
      return true;
    case LineState::Floating:
      Serial.println("FAIL: nothing drives BUSY. Either the BUSY wire does not reach GPIO3,\n"
                     "      or the panel controller has no power: check HAT VCC = 3.3 V and\n"
                     "      GND, the PWR wire (GPIO1), and that the ribbon is fully latched.");
      return false;
    default:
      Serial.println("FAIL: BUSY held LOW. The controller is stuck busy or in reset, or BUSY\n"
                     "      is shorted to GND: check the RST wire (GPIO2), look for solder\n"
                     "      bridges, and confirm Interface Config = 0, Display Config = B.");
      return false;
  }
}

static void drawPattern() {
  const int w = display.width(), h = display.height();

  display.fillScreen(GxEPD_WHITE);
  display.setTextColor(GxEPD_BLACK);

  // 1 px frame on the outermost pixels: any missing edge points to a panel defect.
  display.drawRect(0, 0, w, h, GxEPD_BLACK);
  display.drawRect(4, 4, w - 8, h - 8, GxEPD_BLACK);

  display.setFont(&FreeSansBold18pt7b);
  display.setCursor(24, 52);
  display.print("inkboard smoke test");

  display.setFont(&FreeSans9pt7b);
  display.setCursor(24, 90);
  display.printf("%s rev %d  |  MAC %s", ESP.getChipModel(), ESP.getChipRevision(),
                 WiFi.macAddress().c_str());
  display.setCursor(24, 115);
  if (wifiNetworks > 0) {
    display.printf("Wi-Fi: %d networks, strongest \"%s\" %d dBm", wifiNetworks,
                   bestSsid.c_str(), bestRssi);
  } else {
    display.print("Wi-Fi: no networks found");
  }

  // Checkerboard: row/column driver faults show up as broken stripes.
  const int cell = 20, x0 = 24, y0 = 140;
  for (int r = 0; r < 12; r++)
    for (int c = 0; c < 18; c++)
      if ((r + c) % 2 == 0) display.fillRect(x0 + c * cell, y0 + r * cell, cell, cell, GxEPD_BLACK);

  // Line-width ladder: 1..8 px vertical lines, checks fine detail and ghosting.
  int x = 420;
  for (int lw = 1; lw <= 8; lw++) {
    display.fillRect(x, 140, lw, 240, GxEPD_BLACK);
    x += lw + 12;
  }

  // Solid and outlined blocks.
  display.fillRect(560, 140, 210, 110, GxEPD_BLACK);
  display.fillCircle(665, 320, 55, GxEPD_BLACK);
  display.fillCircle(665, 320, 30, GxEPD_WHITE);

  // Corner markers to confirm orientation.
  display.setCursor(12, h - 12);
  display.print("BL");
  display.setCursor(w - 36, h - 12);
  display.print("BR");
  display.setCursor(w - 36, 24);
  display.print("TR");

  // Frame for the partial-refresh counter.
  display.drawRect(24, 400, 360, 50, GxEPD_BLACK);
}

static void drawCounter(int n) {
  const int x = 25, y = 401, w = 358, hh = 48;
  display.setPartialWindow(x, y, w, hh);
  display.firstPage();
  do {
    display.fillRect(x, y, w, hh, GxEPD_WHITE);
    display.setFont(&FreeSans9pt7b);
    display.setTextColor(GxEPD_BLACK);
    display.setCursor(x + 12, y + 30);
    display.printf("partial refresh %d / 5   uptime %lus", n, millis() / 1000);
  } while (display.nextPage());
}

static void runDisplayTest() {
  banner("e-Paper panel");
  digitalWrite(PIN_EPD_PWR, HIGH);
  delay(10);

  // 2 ms reset pulse is required by the Waveshare HAT's reset circuit.
  display.init(115200, true, 2, false);
  display.setRotation(0);

  uint32_t t = millis();
  display.setFullWindow();
  display.fillScreen(GxEPD_BLACK);
  display.display(false);
  Serial.printf("Full refresh (all black): %lu ms\n", millis() - t);

  t = millis();
  display.setFullWindow();
  drawPattern();
  display.display(false);
  Serial.printf("Full refresh (test pattern): %lu ms\n", millis() - t);

  for (int n = 1; n <= 5; n++) {
    t = millis();
    drawCounter(n);
    Serial.printf("Partial refresh %d: %lu ms\n", n, millis() - t);
    delay(1000);
  }

  // Deep sleep for the panel controller, then cut the HAT's supply.
  display.hibernate();
  digitalWrite(PIN_EPD_PWR, LOW);
  Serial.println("Display test done. Compare the panel against docs/smoke-test.md.");
  Serial.println("Press BOOT to run it again.");
}

// The refresh test only makes sense once the controller answers; without it
// every refresh just waits out GxEPD2's 10 s busy timeout.
static void runPanelChecks() {
  if (checkBusyLine()) {
    runDisplayTest();
  } else {
    digitalWrite(PIN_EPD_PWR, LOW);
    Serial.println("Skipping the display test. Fix the wiring, then press BOOT to retest.");
  }
}

void setup() {
  Serial.begin(115200);
  // Native USB: wait briefly so the first lines are not lost when a monitor attaches.
  for (uint32_t start = millis(); !Serial && millis() - start < 3000;) delay(10);
  delay(500);
  Serial.println("\ninkboard smoke test");

  pinMode(PIN_EPD_PWR, OUTPUT);
  digitalWrite(PIN_EPD_PWR, HIGH);
  pinMode(PIN_BOOT_BTN, INPUT_PULLUP);

  // GxEPD2 writes CS/DC before configuring them, which Arduino-ESP32 3.x
  // logs as an error; configure them first.
  pinMode(PIN_EPD_CS, OUTPUT);
  digitalWrite(PIN_EPD_CS, HIGH);
  pinMode(PIN_EPD_DC, OUTPUT);
  digitalWrite(PIN_EPD_DC, HIGH);

  // Hardware SPI on our own pins. No MISO (the panel is write-only), and CS
  // stays a plain GPIO that GxEPD2 toggles itself.
  SPI.begin(PIN_EPD_CLK, -1, PIN_EPD_DIN, -1);

  checkMcu();
  checkWifi();
  runPanelChecks();
}

void loop() {
  static uint32_t lastBlink = 0;
  if (millis() - lastBlink > 1000) {
    lastBlink = millis();
    digitalWrite(PIN_LED_STATUS, !digitalRead(PIN_LED_STATUS));
  }
  if (digitalRead(PIN_BOOT_BTN) == LOW) {
    delay(50);
    while (digitalRead(PIN_BOOT_BTN) == LOW) delay(10);
    runPanelChecks();
  }
}
