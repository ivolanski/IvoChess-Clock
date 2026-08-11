#ifndef GAME_DATA_SOURCE_H
#define GAME_DATA_SOURCE_H

#include "ClockState.h"

// Abstraction layer: Display/LEDs/Admin only know about ClockState -
// they NEVER know whether the data came from chess.com (WiFi) or
// ChessConnect (Bluetooth, DGT3000). Switching sources in the future
// (from the admin page) shouldn't require changing anything outside
// this file.
enum DataSourceType {
  DATA_SOURCE_CHESSCOM_WIFI = 0,
  DATA_SOURCE_CHESSCONNECT_BLE = 1,  // not implemented yet - stub
  DATA_SOURCE_LICHESS_WIFI = 2,
};

extern DataSourceType currentDataSource;

// The account username to match against state.players[].username for
// "who is me" (win/loss, whose-turn LED color) - chess.com's myUsername
// or Lichess's lichessUsername (both in AdminPortal.h), whichever
// currentDataSource is actually active right now.
//
// Use this instead of reading myUsername/lichessUsername directly.
// DisplayFunctions.cpp and LedFunctions.cpp used to read the chess.com
// global directly, which - since GameDataSource.h's whole point is that
// "Display/LEDs/Admin only know about ClockState" - was a real bug once
// Lichess became a second source: a Lichess game would keep comparing
// player names against the chess.com account's username, silently
// showing wrong LED colors and win/loss.
const char *activeMyUsername();

// Which of state.players[0]/[1] is "me" (activeMyUsername() match), or -1
// if unknown/unresolvable (ChessConnect never discloses this - see
// activeMyUsername() above - or activeMyUsername() isn't configured).
// Shared by LedFunctions.cpp (whose-turn LED color) and IvoChess_Clock.ino
// (own-move vs opponent-move sound trigger) so there's exactly one
// implementation of "who is me" instead of two copies that could drift.
int resolveMyPlayerIndex(const ClockState &state);

void initGameDataSource();

// Clears the idle-timeout state (both chess.com's and Lichess's - only the
// currently active source's statics actually matter, but resetting both is
// harmless and simpler than branching on currentDataSource here) and
// state.waitingTimedOut, so polling for a new game resumes immediately.
// Called from ButtonFunctions.cpp on a button press while NOT using
// ChessConnect - the button's replacement for the old "must physically
// restart the board" requirement.
void resumeGameSearch(ClockState &state);

// Call ~once per second: polls for a new active game over HTTP when
// idle, and updates state.apiStatus/apiOk. Returns true if something
// notable changed (e.g. found a new game) worth a full display refresh.
bool updateGameData(ClockState &state);

// Call EVERY loop() iteration (not gated behind a delay): pumps the live
// WebSocket connection when one is active, so it stays responsive.
void gameDataSourceFastTick(ClockState &state);

#endif  // GAME_DATA_SOURCE_H
