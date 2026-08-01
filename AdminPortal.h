#ifndef ADMIN_PORTAL_H
#define ADMIN_PORTAL_H

#include "ClockState.h"

// Global buffer with the current PHPSESSID (used by ChessApiFunctions).
extern char phpsessid[];

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
