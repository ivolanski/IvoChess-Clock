#ifndef CHESS_API_FUNCTIONS_H
#define CHESS_API_FUNCTIONS_H

#include "ClockState.h"

struct GameInfo {
  char id[48];
  long legacyId;
  char rsocketUrl[80];
  char watchRoute[160];
  PlayerInfo players[2];
};

// Queries GET /service/play/games using the saved PHPSESSID. If there's
// an active game, fills 'game' with the FIRST one in the list and
// returns true. 'statusOut' is ALWAYS filled (e.g. "OK (1 game)",
// "No game", "HTTP 401", "No WiFi", "JSON error") - use it to show on
// the status screen.
bool fetchActiveGame(GameInfo &game, char *statusOut, size_t statusOutLen);

// Re-syncs the internal session cookie jar's PHPSESSID/CHESSCOM_REMEMBERME
// entries from the current phpsessid/chessComRememberMe globals
// (AdminPortal.h). Call this whenever the admin portal saves a new value
// for either - a credential-only save doesn't reboot the device (see
// AdminPortal.cpp handleSave()), so without this the jar would keep using
// whatever was seeded at boot until the next restart.
void refreshSessionCookies();

// Live pass/fail check of the saved chess.com session - does a real request
// (same renewal-and-retry path as fetchActiveGame(), including minting a
// fresh PHPSESSID from CHESSCOM_REMEMBERME if needed) and reports whether
// the session actually works right now, discarding any game found. Used by
// the webadmin to show a green/red status on page load - see handleRoot().
bool testChessComSession();

// Reads back the last time (if any) CHESSCOM_REMEMBERME was lost - either
// found empty on a renewal attempt, or explicitly refused twice and
// discarded - persisted to flash so it survives a reboot, unlike Serial
// output. Returns false (and leaves both buffers untouched) if no such
// event has ever been recorded.
bool getLastChessComSessionFailure(char *reasonOut, size_t reasonLen, char *whenOut, size_t whenLen);

// Stamps flash (not just RAM) with the current time, every time
// CHESSCOM_REMEMBERME is written with a genuine non-empty value - see the
// call in AdminPortal.cpp's persistSessionCookies(). Lets a future "found
// empty at boot" record report the gap since the last known-good write,
// which works regardless of how long the device was powered off for
// (minutes or overnight) - unlike a live serial capture, which can only
// ever span the time the device is actually powered.
void recordGoodCookieWrite();

#endif  // CHESS_API_FUNCTIONS_H
