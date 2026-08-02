#ifndef DISPLAY_FUNCTIONS_H
#define DISPLAY_FUNCTIONS_H

#include "ClockState.h"

void initDisplay();
void drawStartupScreen();

// Redraws the screen with the current state. Alternates on its own, by
// time (SCREEN_CYCLE_INTERVAL_MS), between the logo screen (clean) and
// the status screen (WiFi/IP/API/battery/game) - the caller doesn't
// need to worry about that.
void updateDisplay(bool fullRefresh, const ClockState &state);

#endif  // DISPLAY_FUNCTIONS_H
