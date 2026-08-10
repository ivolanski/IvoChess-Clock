#ifndef CLOCK_STATE_H
#define CLOCK_STATE_H

#include <Arduino.h>

#define USERNAME_MAX_LEN 32

// Sentinel for a rating/move-count that's genuinely unknown (ChessConnect
// mode - see ChessConnectBLE.cpp - never receives either from Chessconnect,
// unlike chess.com/Lichess). DisplayFunctions.cpp checks for this instead
// of blindly formatting %d, so the screen shows "?" rather than "(-1)".
#define RATING_UNKNOWN (-1)
#define MOVE_COUNT_UNKNOWN (-1)

struct PlayerInfo {
  char username[USERNAME_MAX_LEN];
  int rating;
  long clockMs;
};

// How the last-finished game ended, FROM OUR OWN ACCOUNT'S POINT OF VIEW
// (needs myUsername configured in the admin portal to resolve WIN/LOSS -
// falls back to DRAW/NONE-only reporting otherwise). Computed once in
// LiveGameClient.cpp when the game-over RSocket event arrives, so
// LedFunctions.cpp/DisplayFunctions.cpp don't have to re-parse text.
enum GameOutcome {
  OUTCOME_NONE = 0,  // no result to show right now
  OUTCOME_WIN,
  OUTCOME_LOSS,
  OUTCOME_DRAW,
};

struct ClockState {
  // --- network ---
  bool wifiConnected;
  bool apMode;              // true = we're on the setup hotspot, not a real network
  int wifiStrength;         // RSSI, or 0 if disconnected
  char wifiSSID[33];
  char wifiQuality[16];      // "Disconnected", "Poor", "Good", "Excellent" - ja calculado, pronto pra mostrar
  char ipAddress[16];

  // --- battery ---
  float batteryVoltage;
  int batteryPercentage;

  // --- status of the last query to the server (chess.com or another source) ---
  char apiStatus[40];       // e.g. "OK", "HTTP 401", "No WiFi", "Timeout"
  bool apiOk;

  // True once the "waiting for game" polling has given up after
  // WAITING_FOR_GAME_TIMEOUT_MS of continuous idle time (see
  // GameDataSource.cpp) - deliberately requires a physical restart to
  // clear (it's a static local, not reset anywhere else), not a timer
  // that quietly resumes on its own. Exists so a clock left on 24/7 with
  // nobody playing doesn't poll the server forever - see the "screen
  // stays on WAITING FOR GAME all day" concern this was built for.
  bool waitingTimedOut;

  // --- current game ---
  bool hasGame;
  PlayerInfo players[2];
  int moveCount;
  int activePlayerIndex;
  char lastResultSummary[128];   // compact one-line form, e.g. for logs/fallback rendering
  GameOutcome lastGameOutcome;
  char lastResultReason[40];     // chess.com's raw per-player reason string: "resigned", "checkmated", "agreed", "timeout"...
  int lastRatingDelta;           // my rating change, only meaningful if lastRatingKnown
  bool lastRatingKnown;          // false when myUsername wasn't configured/matched - don't show a rating delta of 0 as if it were real
  char lastOpponentUsername[USERNAME_MAX_LEN];
  // Both players' ratings as last known going into the result screen (from
  // players[] at the moment the game ended - chess.com's post-game "results"
  // event doesn't carry the opponent's new rating, and Lichess's board stream
  // doesn't carry rating deltas at all, so this is the pre-result rating on
  // both platforms rather than a post-result one). Only meaningful if
  // lastPlayerRatingsKnown (needs activeMyUsername() to have resolved which
  // side is "me").
  int lastMyRating;
  int lastOpponentRating;
  bool lastPlayerRatingsKnown;
  unsigned long resultDisplayUntilMs;  // 0 = not showing a result right now; else millis() timestamp until which the result screen stays up

  // Anchor for local clock extrapolation between RSocket updates: the
  // last REAL clock values from RSocket (or 0/0 at game start before the
  // first one arrives) and when they were captured. NEVER touched by the
  // periodic local-extrapolation tick in IvoChess_Clock.ino - only by a
  // real RSocket clock update (LiveGameClient.cpp) or a new game
  // starting (GameDataSource.cpp) - so extrapolation is always computed
  // fresh from this fixed point + elapsed time, instead of chaining off
  // an already-extrapolated value (which would compound drift).
  long clockBaselineMs[2];
  unsigned long clockBaselineAtMs;

  // --- redraw control ---
  unsigned long lastDisplayUpdate;
  unsigned long lastFullRefresh;

  // Last millis() at which hasGame was true - lets the display tell "no
  // game at all right now" (fine to cycle the anti-burn-in logo screen)
  // apart from "briefly reconnecting mid-game" (a live connection blip -
  // see gameDataSourceFastTick() in GameDataSource.cpp - is not the same
  // as genuinely idle, and showing the logo screen during it looked like
  // it had frozen the game). 0 = no game seen yet this boot.
  unsigned long lastGameActiveAt;

  // ChessConnect only (see ChessConnectBLE.cpp): the opponent's last move
  // text (e.g. "PE7E5"), shown briefly in the bottom bar in place of the
  // move count - chess.com/Lichess never populate this. Empty/0 = nothing
  // to show right now; DisplayFunctions.cpp falls back to the normal move
  // count display once millis() passes chessConnectMoveTextUntilMs.
  char chessConnectMoveText[20];
  unsigned long chessConnectMoveTextUntilMs;
};

#endif  // CLOCK_STATE_H
