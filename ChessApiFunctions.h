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

#endif  // CHESS_API_FUNCTIONS_H
