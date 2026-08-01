#ifndef ADMIN_PORTAL_H
#define ADMIN_PORTAL_H

#include "ClockState.h"

// Global buffer with the current PHPSESSID (used by ChessApiFunctions).
extern char phpsessid[];

// Global buffer with the account's own chess.com username (used by
// DisplayFunctions to always draw "me" in the bottom row, like sitting
// across a real board, and by LiveGameClient to resolve win/loss).
// Empty string = unknown, falls back to whatever order/wording chess.com
// gave us.
extern char myUsername[];

// LED colors (see LedFunctions.cpp) - all configurable from the admin
// portal, defaults come from config.h (DEFAULT_LED_*) the first time the
// device boots. {R, G, B}, 0-255 each.
extern uint8_t ledColorNoWifi[3];
extern uint8_t ledColorLowBattery[3];
extern uint8_t ledColorWon[3];
extern uint8_t ledColorLost[3];
extern uint8_t ledColorDraw[3];       // also used when it's unclear whose turn it is
extern uint8_t ledColorMyTurn[3];
extern uint8_t ledColorOpponentTurn[3];
extern uint8_t ledBrightnessDay;
extern uint8_t ledBrightnessNight;

// How long (ms) the game-over result stays on screen before returning to
// the waiting/status screen. Configurable in seconds in the admin portal.
extern unsigned long resultDisplayDurationMs;

// A SINGLE web portal/server - reachable via the "IvoChess-Setup" hotspot
// (when there's no WiFi configured yet, or the saved WiFi failed) OR via
// the normal network IP (whenever connected, including to CHANGE
// networks without needing to reset anything - e.g. left the hotel
// lobby, went to the room, want to switch WiFi). Call once in setup().
void initAdminPortal(ClockState *statePtr);

// Processes pending web + captive portal DNS requests. Call on every
// loop() iteration (not behind a long delay()).
void handleAdminPortal();

// Updates state.wifiConnected/wifiStrength/wifiSSID/ipAddress/apMode
// with the current status. Call periodically from loop().
void updateWiFiStatus(ClockState &state);

#endif  // ADMIN_PORTAL_H
