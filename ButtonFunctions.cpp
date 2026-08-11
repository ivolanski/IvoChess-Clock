#include "ButtonFunctions.h"
#include "config.h"
#include "GameDataSource.h"
#include "ChessConnectBLE.h"

void initButton() {
  pinMode(BUTTON_PIN, INPUT_PULLUP);
}

// Time-based debounce: a press is only recognized once the pin has read
// LOW (pressed - INPUT_PULLUP means idle-HIGH, pressed-LOW) continuously
// for BUTTON_DEBOUNCE_MS, and only fires once per physical press (not
// again until the pin has been seen HIGH again in between).
void updateButton(ClockState &state) {
  static bool lastStableState = HIGH;  // HIGH = not pressed
  static bool lastRawState = HIGH;
  static unsigned long lastChangeMs = 0;

  bool raw = digitalRead(BUTTON_PIN);
  unsigned long now = millis();

  if (raw != lastRawState) {
    lastRawState = raw;
    lastChangeMs = now;
  }

  if ((now - lastChangeMs) >= BUTTON_DEBOUNCE_MS && raw != lastStableState) {
    lastStableState = raw;
    if (lastStableState == LOW) {
      // Debounced press edge - exactly one action per physical press.
      if (currentDataSource == DATA_SOURCE_CHESSCONNECT_BLE) {
        chessConnectSendButtonEvent();
      } else {
        resumeGameSearch(state);
      }
    }
  }
}
