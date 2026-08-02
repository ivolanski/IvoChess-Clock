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

// Whether the anti-burn-in logo screen would show right now, given the
// current state - shared with IvoChess_Clock.ino so its own full-refresh
// scheduling and updateDisplay()'s actual content dispatch never disagree
// about what's on screen (they used to compute this separately).
bool isLogoPhaseNow(const ClockState &state);

#endif  // DISPLAY_FUNCTIONS_H
