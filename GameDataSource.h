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

void initGameDataSource();

// Call ~once per second: polls for a new active game over HTTP when
// idle, and updates state.apiStatus/apiOk. Returns true if something
// notable changed (e.g. found a new game) worth a full display refresh.
bool updateGameData(ClockState &state);

// Call EVERY loop() iteration (not gated behind a delay): pumps the live
// WebSocket connection when one is active, so it stays responsive.
void gameDataSourceFastTick(ClockState &state);

#endif  // GAME_DATA_SOURCE_H
