#include "panel.h"

#include <Arduino.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <GxEPD2_BW.h>
#include <SPI.h>

#include "config.h"
#include "pins.h"

// A 1/8-height page buffer (6 KB) is enough for the paged error screen; server frames go
// straight to the controller with writeImage and never touch this buffer.
static GxEPD2_BW<GxEPD2_750_T7, GxEPD2_750_T7::HEIGHT / 8> display(
    GxEPD2_750_T7(PIN_EPD_CS, PIN_EPD_DC, PIN_EPD_RST, PIN_EPD_BUSY));

static void power_on() {
  pinMode(PIN_EPD_PWR, OUTPUT);
  digitalWrite(PIN_EPD_PWR, HIGH);
  delay(50);
  // GxEPD2 drives CS/DC/RST before configuring them; set them up first.
  for (int pin : {PIN_EPD_CS, PIN_EPD_DC, PIN_EPD_RST}) {
    pinMode(pin, OUTPUT);
    digitalWrite(pin, HIGH);
  }
  SPI.begin(PIN_EPD_CLK, -1, PIN_EPD_DIN, -1);
  display.init(115200, true, 2, false);  // initial=true: the controller lost its RAM
}

static void power_off() {
  display.hibernate();
  digitalWrite(PIN_EPD_PWR, LOW);
}

void panel_show(const uint8_t* frame) {
  power_on();
  display.writeImage(frame, 0, 0, GxEPD2_750_T7::WIDTH, GxEPD2_750_T7::HEIGHT, FRAME_INVERT, false, false);
  display.refresh(false);
  power_off();
}

void panel_show_error(const char* message, const char* query) {
  power_on();
  display.setRotation(0);
  display.setFullWindow();
  display.setTextColor(GxEPD_BLACK);
  display.setTextWrap(true);
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setFont(&FreeSansBold12pt7b);
    display.setCursor(24, 60);
    display.print("inkboard config error");
    display.setFont(&FreeSans9pt7b);
    display.setCursor(24, 110);
    display.print(message);
    display.setCursor(24, 380);
    display.print("FRAME_QUERY in include/config.h:");
    display.setCursor(24, 410);
    display.print(query);
  } while (display.nextPage());
  power_off();
}
