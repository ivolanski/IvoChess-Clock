#ifndef LICHESS_LIVE_CLIENT_H
#define LICHESS_LIVE_CLIENT_H

#include "ClockState.h"
#include "LichessApiFunctions.h"

// Opens the live stream (GET /api/board/game/stream/{id}) for 'game' and
// starts watching it. Unlike chess.com's RSocket-over-WebSocket
// (LiveGameClient.h), this is NDJSON over a long-lived chunked HTTP
// response - one JSON object per line, no binary framing - read
// incrementally over many lichessLiveLoop() calls. See
// LichessLiveClient.cpp for why HTTPClient::getString()/writeToStream()
// can't be used here (verified against the ESP32 core's own source: both
// either buffer until the connection closes, which never happens on a
// live stream, or block the whole loop() until a full chunk arrives).
void lichessLiveConnect(const LichessGameInfo &game, const char *token);

// Closes the connection (if open).
void lichessLiveDisconnect();

// True while a live connection is open/active.
bool lichessLiveIsConnected();

// True if connected but no NDJSON line - a real state update or a
// keepalive blank line, Lichess sends one or the other periodically -
// has arrived in well over the expected cadence. Same staleness concept
// as chess.com's liveGameIsStale() (LiveGameClient.h): catches a TCP
// socket that's gone quietly one-way-dead without firing a disconnect.
bool lichessLiveIsStale();

// Pumps the stream (reads+parses whatever bytes have arrived since the
// last call, non-blocking) and updates 'state'. Call this EVERY loop()
// iteration, same contract as chess.com's liveGameLoop().
void lichessLiveLoop(ClockState &state);

#endif  // LICHESS_LIVE_CLIENT_H
