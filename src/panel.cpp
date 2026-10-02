#include "panel.h"

#include <Arduino.h>
#include <GxEPD2_BW.h>
#include <SPI.h>

#include "config.h"
#include "pins.h"

// A 1/8-height page buffer (6 KB) is GxEPD2's minimum; frames go straight to the controller
// with writeImage and never touch it.
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
