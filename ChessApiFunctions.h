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

#endif  // CHESS_API_FUNCTIONS_H
