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

// Redraws ONLY the two clock boxes via a real partial window (no
// full-screen flash). Use this for the once-a-second "clock is
// ticking" case while a game is active; use updateDisplay(true, ...)
// for anything else (new move, game start/end, phase changes).
void updateGameClocksPartial(const ClockState &state);

#endif  // DISPLAY_FUNCTIONS_H
