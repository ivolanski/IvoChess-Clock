#ifndef CLOCK_STATE_H
#define CLOCK_STATE_H

#include <Arduino.h>

#define USERNAME_MAX_LEN 32

struct PlayerInfo {
  char username[USERNAME_MAX_LEN];
  int rating;
  long clockMs;
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

  // --- current game ---
  bool hasGame;
  PlayerInfo players[2];
  int moveCount;
  int activePlayerIndex;
  char lastResultSummary[128];
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
};

#endif  // CLOCK_STATE_H
