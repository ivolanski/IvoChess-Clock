#ifndef CHESSCONNECT_BLE_H
#define CHESSCONNECT_BLE_H

#include "ClockState.h"

// BLE GATT peripheral implementing the "DGT3000 BLE Gateway" protocol
// (project_details/dgt3000-gateway-protocol.md) - Chessconnect drives
// this clock the same way it drives a real DGT 3000 + BLE gateway box.
// Validated against the real protocol first via tests/chessconnect_ble_poc/
// (real captures against Chessconnect, both bot and online games, through
// to a result) before this real integration was written.
//
// THREADING: BLE callbacks run on the NimBLE stack's own FreeRTOS task,
// not the main loop() task. Every other writer of ClockState in this
// codebase (GameDataSource.cpp, LiveGameClient.cpp, LichessLiveClient.cpp,
// AdminPortal.cpp, IvoChess_Clock.ino itself) assumes single-threaded
// access - see DisplayFunctions.cpp's own comment on this. So the BLE
// callback never touches ClockState directly: it acks immediately (safe,
// no shared state involved) and hands off a small event onto a FreeRTOS
// queue. chessConnectBleLoop(), called from the main task, drains that
// queue and is the ONLY thing that ever applies BLE-derived data to
// ClockState - same "queue hand-off between tasks" pattern
// DisplayFunctions.cpp already uses for the display task, just applied to
// the opposite direction (another task producing, main task consuming).

// Sets up the GATT server/advertising. Call once from setup().
void initChessConnectBLE();

// Drains any BLE events received since the last call and applies them to
// 'state'. Call every loop() iteration (not gated behind the 1s tick) -
// Chessconnect pushes setTime roughly once per second and expects the
// display to keep up; this is the ChessConnect equivalent of
// gameDataSourceFastTick()'s WebSocket pumping for chess.com/Lichess.
void chessConnectBleLoop(ClockState &state);

// True while a Chessconnect central is connected - a cheap status flag
// for the top status bar (DisplayFunctions.cpp), not used for any
// correctness-critical logic. Safe to read from the main task despite
// being written from the BLE task's onConnect/onDisconnect callbacks -
// it's a single bool read/write, not a data structure that needs the
// event queue's ordering guarantees.
bool chessConnectBleIsConnected();

#endif  // CHESSCONNECT_BLE_H
