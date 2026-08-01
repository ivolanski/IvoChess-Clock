#ifndef LIVE_GAME_CLIENT_H
#define LIVE_GAME_CLIENT_H

#include "ClockState.h"
#include "ChessApiFunctions.h"

// Opens the RSocket-over-WebSocket connection for 'game' and starts
// watching it live (clocks, moves, end-of-game result).
void liveGameConnect(const GameInfo &game);

// Closes the connection (if open).
void liveGameDisconnect();

// True while a live connection is open/active.
bool liveGameIsConnected();

// Pumps the WebSocket client (processes incoming frames as they arrive)
// and updates 'state' (clocks/moveCount/activePlayerIndex, or
// hasGame=false + lastResultSummary + resultDisplayUntilMs when the game
// ends). Call this EVERY loop() iteration (not gated behind a delay) -
// the WebSocket needs prompt servicing to stay responsive.
void liveGameLoop(ClockState &state);

#endif  // LIVE_GAME_CLIENT_H
