#include "ButtonFunctions.h"
#include "config.h"
#include "GameDataSource.h"
#include "ChessConnectBLE.h"
#include "SystemLog.h"

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
      // Gated on hasGame FIRST, not just currentDataSource: mid-game the
      // button sends a move (ChessConnect) or does nothing (chess.com/
      // Lichess), but with no game in progress every source should resume
      // the game search instead - including ChessConnect, which previously
      // fired a bogus BLE button/move event even while idle with nothing
      // paired to a game yet.
      if (state.hasGame) {
        if (currentDataSource == DATA_SOURCE_CHESSCONNECT_BLE) {
          systemLog("Button pressed - sending ChessConnect move event");
          chessConnectSendButtonEvent();
        }
      } else {
        resumeGameSearch(state);
      }
    }
  }
}
