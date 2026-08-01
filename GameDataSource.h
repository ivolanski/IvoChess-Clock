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
};

extern DataSourceType currentDataSource;

void initGameDataSource();

// Call ~once per second: polls for a new active game over HTTP when
// idle, and updates state.apiStatus/apiOk. Returns true if something
// notable changed (e.g. found a new game) worth a full display refresh.
bool updateGameData(ClockState &state);

// Call EVERY loop() iteration (not gated behind a delay): pumps the live
// WebSocket connection when one is active, so it stays responsive.
void gameDataSourceFastTick(ClockState &state);

#endif  // GAME_DATA_SOURCE_H
