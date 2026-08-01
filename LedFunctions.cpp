#include "LedFunctions.h"
#include "config.h"
#include "TimeFunctions.h"

#include <Adafruit_NeoPixel.h>

static Adafruit_NeoPixel strip(LED_COUNT, LED_DATA_PIN, NEO_GRB + NEO_KHZ800);

void initLEDs() {
  strip.begin();
  strip.setBrightness(LED_BRIGHTNESS);
  strip.clear();
  strip.show();
}

void setAllLEDs(uint8_t r, uint8_t g, uint8_t b) {
  for (int i = 0; i < LED_COUNT; i++) {
    strip.setPixelColor(i, strip.Color(r, g, b));
  }
  strip.show();
}

void clearLEDs() {
  setAllLEDs(0, 0, 0);
}

// The case diffuses the light (individual LEDs aren't visible) - that's
// why the WHOLE case changes color, instead of splitting the strip.
void updateLEDs(const ClockState &state) {
  strip.setBrightness(isNightTime() ? LED_BRIGHTNESS_NIGHT : LED_BRIGHTNESS);

  if (!state.wifiConnected) {
    setAllLEDs(COLOR_BLUE_R, COLOR_BLUE_G, COLOR_BLUE_B);
    return;
  }

  if (state.batteryPercentage > 0 && state.batteryPercentage < 15) {
    static bool blinkOn = false;
    blinkOn = !blinkOn;
    if (blinkOn) {
      setAllLEDs(COLOR_ORANGE_R, COLOR_ORANGE_G, COLOR_ORANGE_B);
      return;
    }
  }

  if (!state.hasGame) {
    if (strstr(state.lastResultSummary, "WON") != nullptr) {
      setAllLEDs(COLOR_GREEN_R, COLOR_GREEN_G, COLOR_GREEN_B);
    } else if (strstr(state.lastResultSummary, "LOST") != nullptr) {
      setAllLEDs(COLOR_PINK_R, COLOR_PINK_G, COLOR_PINK_B);
    } else {
      clearLEDs();
    }
    return;
  }

  if (state.activePlayerIndex == 0) {
    setAllLEDs(COLOR_CYAN_R, COLOR_CYAN_G, COLOR_CYAN_B);
  } else if (state.activePlayerIndex == 1) {
    setAllLEDs(COLOR_YELLOW_R, COLOR_YELLOW_G, COLOR_YELLOW_B);
  } else {
    setAllLEDs(COLOR_WHITE_R, COLOR_WHITE_G, COLOR_WHITE_B);
  }
}
