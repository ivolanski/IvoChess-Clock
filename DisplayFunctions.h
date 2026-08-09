#ifndef DISPLAY_FUNCTIONS_H
#define DISPLAY_FUNCTIONS_H

#include "ClockState.h"

// Starts the dedicated FreeRTOS task that owns the e-paper display
// exclusively (nothing else may call display.* / GxEPD2 directly - see
// DisplayFunctions.cpp). Runs initDisplay()+drawStartupScreen() itself,
// at the top of the task, before waiting for the first real update - so
// call this once from setup() in place of calling those two directly.
void startDisplayTask();

// Asks the display task to render 'state' (fullRefresh as
// updateDisplay() already expected). Non-blocking: copies 'state' into a
// small request and hands it off via a length-1 "latest wins" queue -
// returns in microseconds regardless of how long the actual e-paper
// refresh takes (~3.6s on real hardware for a full refresh). If another
// request arrives before the display task gets to render the previous
// one, the previous one is silently replaced - the display always ends
// up showing the MOST RECENT state asked for, never a stale one queued
// up behind it. Call this instead of updateDisplay() directly.
void requestDisplayUpdate(bool fullRefresh, const ClockState &state);

// Whether the anti-burn-in logo screen would show right now, given the
// current state - shared with IvoChess_Clock.ino so its own full-refresh
// scheduling and updateDisplay()'s actual content dispatch never disagree
// about what's on screen (they used to compute this separately). Pure
// (no display I/O), safe to call from the main loop task.
bool isLogoPhaseNow(const ClockState &state);

#endif  // DISPLAY_FUNCTIONS_H
