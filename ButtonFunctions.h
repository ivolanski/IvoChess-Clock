#ifndef BUTTON_FUNCTIONS_H
#define BUTTON_FUNCTIONS_H

#include "ClockState.h"

void initButton();

// Call EVERY loop() iteration (not gated behind the 1s tick) - a physical
// button press should feel instant, not laggy. Debounces internally
// (BUTTON_DEBOUNCE_MS, config.h) and dispatches on a debounced press:
//   - currentDataSource == DATA_SOURCE_CHESSCONNECT_BLE: sends ChessConnect's
//     buttonEvent (chessConnectSendButtonEvent(), ChessConnectBLE.h) - the
//     "press clock to transmit move" trigger.
//   - otherwise: resumeGameSearch() (GameDataSource.h) - resumes polling
//     after the idle timeout without a physical restart.
void updateButton(ClockState &state);

#endif  // BUTTON_FUNCTIONS_H
