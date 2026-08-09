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

// Asks the display task for a TRUE partial-window redraw of just the
// currently active player's clock digits, instead of a full-screen
// redraw - much faster (~500ms vs ~2-3.6s on this panel, validated on
// real hardware first via tests/epaper_partial_refresh_poc/) and doesn't
// flash the whole panel. Meant for the periodic "no real move happened,
// just extrapolate the ticking clock locally" case (see
// IvoChess_Clock.ino) - anything that changes more than the active
// clock's digits (a move, a new game, a result) should still go through
// requestDisplayUpdate(true, ...) for a real full redraw. Silently does
// nothing if state.hasGame is false or activePlayerIndex isn't 0/1 - same
// non-blocking "latest wins" queue as requestDisplayUpdate().
void requestGameClockPartialRefresh(const ClockState &state);

// Asks the display task for a TRUE partial-window redraw of everything a
// MOVE can change - both players' rating/"on move" triangle/clock zone
// and the move count - but not the name row (static once a game starts)
// or the top status bar (unrelated to moves). Meant for moveChanged in
// IvoChess_Clock.ino when nothing else that would need a real full
// redraw happened alongside it (a new game, a phase change, a result).
// Same non-blocking "latest wins" queue, same real-hardware validation
// path (tests/epaper_partial_refresh_poc/) as requestGameClockPartialRefresh().
void requestGameMovePartialRefresh(const ClockState &state);

// Whether the anti-burn-in logo screen would show right now, given the
// current state - shared with IvoChess_Clock.ino so its own full-refresh
// scheduling and updateDisplay()'s actual content dispatch never disagree
// about what's on screen (they used to compute this separately). Pure
// (no display I/O), safe to call from the main loop task.
bool isLogoPhaseNow(const ClockState &state);

#endif  // DISPLAY_FUNCTIONS_H
