#include "LedFunctions.h"
#include "config.h"
#include "AdminPortal.h"
#include "GameDataSource.h"
#include "TimeFunctions.h"

#include <Adafruit_NeoPixel.h>

static Adafruit_NeoPixel strip(LED_COUNT, LED_DATA_PIN, NEO_GRB + NEO_KHZ800);

void initLEDs() {
  strip.begin();
  strip.setBrightness(ledBrightnessDay);
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
// why the WHOLE strip changes color together, instead of splitting it
// per-player or per-pixel.
void updateLEDs(const ClockState &state) {
  strip.setBrightness(isNightTime() ? ledBrightnessNight : ledBrightnessDay);

  if (!state.wifiConnected) {
    setAllLEDs(ledColorNoWifi[0], ledColorNoWifi[1], ledColorNoWifi[2]);
    return;
  }

  // Figure out the "true" state color first, then only LAYER the
  // low-battery warning on top as a blink (alternating with the true
  // color) instead of fully replacing it - otherwise a low battery during
  // an active game would hide whose turn it is, which matters more.
  uint8_t baseColor[3];

  if (state.hasGame) {
    int meIdx = resolveMyPlayerIndex(state);
    if (state.activePlayerIndex == -1) {
      memcpy(baseColor, ledColorDraw, 3);  // whose turn isn't known yet - neutral color
    } else if (meIdx != -1) {
      memcpy(baseColor, (state.activePlayerIndex == meIdx) ? ledColorMyTurn : ledColorOpponentTurn, 3);
    } else {
      // activeMyUsername() isn't configured - fall back to raw index so there's
      // still a turn indicator, just not tied to "me" specifically.
      memcpy(baseColor, (state.activePlayerIndex == 0) ? ledColorMyTurn : ledColorOpponentTurn, 3);
    }
  } else {
    switch (state.lastGameOutcome) {
      case OUTCOME_WIN:  memcpy(baseColor, ledColorWon, 3); break;
      case OUTCOME_LOSS: memcpy(baseColor, ledColorLost, 3); break;
      case OUTCOME_DRAW: memcpy(baseColor, ledColorDraw, 3); break;
      case OUTCOME_NONE:
      default: baseColor[0] = baseColor[1] = baseColor[2] = 0; break;  // idle - off
    }
  }

  if (state.batteryPercentage > 0 && state.batteryPercentage < LOW_BATTERY_THRESHOLD_PERCENT) {
    static bool blinkOn = false;
    blinkOn = !blinkOn;
    if (blinkOn) {
      setAllLEDs(ledColorLowBattery[0], ledColorLowBattery[1], ledColorLowBattery[2]);
      return;
    }
  }

  setAllLEDs(baseColor[0], baseColor[1], baseColor[2]);
}
