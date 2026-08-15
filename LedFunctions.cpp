#include "LedFunctions.h"
#include "config.h"
#include "AdminPortal.h"
#include "GameDataSource.h"
#include "TimeFunctions.h"

#include <Adafruit_NeoPixel.h>

static Adafruit_NeoPixel strip(LED_COUNT, LED_DATA_PIN, NEO_GRB + NEO_KHZ800);

// LED_COUNT (config.h) is only the first-boot default now - setLedCount()
// below can change the strip's actual driven length at runtime, and
// everything else in this file reads it back via strip.numPixels() rather
// than the LED_COUNT macro, so a different physical strip length (set from
// the webadmin) just works without a recompile.
#define LED_COUNT_MIN 1
#define LED_COUNT_MAX 60

void setLedCount(uint16_t count) {
  if (count < LED_COUNT_MIN) count = LED_COUNT_MIN;
  if (count > LED_COUNT_MAX) count = LED_COUNT_MAX;
  if (count == strip.numPixels()) return;
  strip.updateLength(count);
  strip.clear();
  strip.show();
}

#define LED_TEST_STEP_MS 1500
#define LED_TEST_MAX_COLORS 7

struct LedTestState {
  bool active = false;
  uint8_t colors[LED_TEST_MAX_COLORS][3];
  int stepCount = 0;
  int currentStep = 0;
  unsigned long stepStartMs = 0;
};
static LedTestState ledTest;

void startLedTest(const uint8_t colors[][3], int count) {
  ledTest.stepCount = (count > LED_TEST_MAX_COLORS) ? LED_TEST_MAX_COLORS : count;
  for (int i = 0; i < ledTest.stepCount; i++) {
    memcpy(ledTest.colors[i], colors[i], 3);
  }
  ledTest.currentStep = 0;
  ledTest.stepStartMs = millis();
  ledTest.active = (ledTest.stepCount > 0);
}

void initLEDs() {
  strip.begin();
  strip.setBrightness(ledBrightnessDay);
  strip.clear();
  strip.show();
}

void setAllLEDs(uint8_t r, uint8_t g, uint8_t b) {
  for (uint16_t i = 0; i < strip.numPixels(); i++) {
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
  if (ledTest.active) {
    unsigned long now = millis();
    if (now - ledTest.stepStartMs >= LED_TEST_STEP_MS) {
      ledTest.currentStep++;
      ledTest.stepStartMs = now;
      if (ledTest.currentStep >= ledTest.stepCount) {
        ledTest.active = false;  // done - fall through to normal state-driven logic below
      }
    }
    if (ledTest.active) {
      strip.setBrightness(isNightTime() ? ledBrightnessNight : ledBrightnessDay);
      const uint8_t *c = ledTest.colors[ledTest.currentStep];
      setAllLEDs(c[0], c[1], c[2]);
      return;
    }
  }

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
