#ifndef ADMIN_PORTAL_H
#define ADMIN_PORTAL_H

#include "ClockState.h"
#include "SoundFunctions.h"  // SoundEvent, SOUND_EVENT_COUNT - for soundEnabled[]/soundMelodyOverride[] below
#include "config.h"          // SOUND_MELODY_MAX_LEN

// Global buffer with the current PHPSESSID (used by ChessApiFunctions).
// ChessApiFunctions.cpp updates this IN PLACE (and persists it via
// persistSessionCookies() below) whenever chess.com silently renews the
// session using chessComRememberMe - see the comment on renewSession()
// there for how/why that works.
extern char phpsessid[];

// Chess.com's long-lived "remember me" cookie (CHESSCOM_REMEMBERME,
// captured from a browser the same way as phpsessid). This is what lets
// ChessApiFunctions.cpp mint a fresh PHPSESSID on its own when the
// current one expires - a real browser tab does the same thing
// transparently, this is that same mechanism. Rotates on every use (the
// server issues a new token each time), which is why it's also updated
// in place and re-persisted alongside phpsessid.
extern char chessComRememberMe[];

// Global buffer with the account's own chess.com username (used by
// DisplayFunctions to always draw "me" in the bottom row, like sitting
// across a real board, and by LiveGameClient to resolve win/loss).
// Empty string = unknown, falls back to whatever order/wording chess.com
// gave us.
//
// NOTE: don't read this directly outside ChessApiFunctions.cpp/
// LiveGameClient.cpp - use GameDataSource.cpp's activeMyUsername()
// instead, which returns the right one of this/lichessUsername for
// whichever data source is currently active. Reading this global
// directly is exactly the bug that made Lichess games silently show
// wrong LED colors/win-loss the first time this was built - see
// activeMyUsername()'s own comment in GameDataSource.h.
extern char myUsername[];

// Lichess's equivalent of chessComRememberMe/myUsername above - a long-
// lived OAuth access token (captured via the webadmin's "Connect to
// Lichess" PKCE flow, see AdminPortal.cpp's /oauth/lichess/callback
// route) and the account's own username. Unlike chess.com's cookie,
// Lichess tokens don't rotate and don't need silent renewal - see
// LichessApiFunctions.h for why. Empty lichessToken = not connected.
extern char lichessToken[];
extern char lichessUsername[];

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

// How many pixels of the strip are actually driven - defaults to config.h's
// LED_COUNT on first boot, but can be changed from the webadmin (applied
// live via LedFunctions.h's setLedCount()) so a build with a different
// physical strip length doesn't need a recompile.
extern uint16_t ledCount;

// How long (ms) the game-over result stays on screen before returning to
// the waiting/status screen. Configurable in seconds in the admin portal.
extern unsigned long resultDisplayDurationMs;

// Sound settings (see SoundFunctions.cpp) - indexed by SoundEvent. Each
// event can be muted independently (soundEnabled) and/or given a custom
// melody (soundMelodyOverride) - empty string means "use the compiled-in
// DEFAULT_SOUND_* from config.h". Note availability differs per event/data
// source: SOUND_CHECK only ever fires via ChessConnect (see
// ChessConnectBLE.cpp); SOUND_GAME_WIN/LOSS/DRAW only fire when
// activeMyUsername() resolves (chess.com/Lichess with a username
// configured) - everything else (including all of ChessConnect's game
// endings) falls through to SOUND_GAME_END instead.
extern bool soundEnabled[SOUND_EVENT_COUNT];
extern char soundMelodyOverride[SOUND_EVENT_COUNT][SOUND_MELODY_MAX_LEN];

// Master volume, 0-100 (0 = mute) - see SoundFunctions.cpp's speakerTone()
// for how this maps to PWM duty cycle. Applies to every event; there's no
// per-event volume, only per-event mute (soundEnabled above).
extern uint8_t soundVolume;

// Persists the CURRENT phpsessid/chessComRememberMe to Preferences -
// called by ChessApiFunctions.cpp after a successful session renewal, so
// the fresh values survive a reboot. Doesn't touch any other setting.
void persistSessionCookies();

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
