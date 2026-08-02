/*
 * IvoChess Clock
 * ================================================================
 *
 * Files:
 *   config.h                 - pins, constants, default values
 *   ClockState.h              - central struct with everything the screen needs
 *   Translations.h/.cpp       - translatable text (default: English)
 *   AdminPortal.h/.cpp        - A SINGLE web portal: setup hotspot OR
 *                               normal IP, depending on whether WiFi is
 *                               connected. Fields: WiFi network,
 *                               language, data source, PHPSESSID.
 *   DisplayFunctions.h/.cpp   - e-paper: logo/status alternating (anti burn-in)
 *   LedFunctions.h/.cpp       - WS2812 LED strip
 *   BatteryFunctions.h/.cpp   - battery reading
 *   TimeFunctions.h/.cpp      - NTP, only for the LEDs' night mode
 *   ChessApiFunctions.h/.cpp  - GET /service/play/games (chess.com over HTTP)
 *   RSocketCodec.h/.cpp       - pure RSocket frame encode/decode, no I/O
 *   LiveGameClient.h/.cpp     - RSocket-over-WebSocket client: connects to
 *                               the game found by ChessApiFunctions and
 *                               streams live clocks/moves/result.
 *   GameDataSource.h/.cpp     - ABSTRACTION: hides where the game data
 *                               comes from (chess.com/WiFi today;
 *                               ChessConnect/BLE in the future) behind
 *                               updateGameData()/gameDataSourceFastTick().
 *
 * CURRENT STEP: full live game flow over chess.com (HTTP to find the
 * game, RSocket/WebSocket for live clocks/moves, result shown for
 * resultDisplayDurationMs after the game ends - admin-portal configurable).
 *
 * REQUIRED BOARD SETTING (Arduino IDE > Tools > Partition Scheme):
 *   "Huge APP (3MB No OTA/1MB SPIFFS)" - the default scheme only gives
 *   1.2MB to the app, and this sketch (GxEPD2 + fonts + WebSockets
 *   + ArduinoJson) is right at that ceiling; it will fail to compile
 *   ("text section exceeds available space") on the default scheme. No
 *   OTA update code exists anywhere in this project, so losing the OTA
 *   slot costs nothing.
 *
 * REQUIRED LIBRARIES (Arduino IDE > Library Manager):
 *   - GxEPD2 (ZinggJM)
 *   - Adafruit GFX Library
 *   - Adafruit BusIO
 *   - Adafruit NeoPixel
 *   - ArduinoJson (Benoit Blanchon)
 *   - WebSockets (Markus Sattler / Links2004) - for the live RSocket
 *     connection. This project tried ArduinoWebsockets (Gil Maimon)
 *     first, on the assumption it "skips TLS cert validation by default
 *     on ESP32" - that assumption was wrong: its ESP32 code path never
 *     actually calls setInsecure() on the underlying TLS client, so
 *     client.connect() failed immediately every time. Back on
 *     WebSockets (Links2004) now that ITS actual bug (a malformed
 *     handshake from a header-building double-\r\n - see the comment on
 *     setExtraHeaders() in LiveGameClient.cpp) is fixed instead of
 *     worked around.
 *   IMPORTANT: WebSockets needs a ONE-LINE PATCH to keep working - see
 *     LiveGameClient.cpp's top comment for what and why. Redo it if this
 *     library gets reinstalled/updated.
 *   (Preferences, WebServer, DNSServer, ESPmDNS already ship with the
 *    ESP32 core - no WiFiManager needed anymore, AdminPortal replaces it)
 */

#include "config.h"
#include "ClockState.h"
#include "Translations.h"
#include "AdminPortal.h"
#include "DisplayFunctions.h"
#include "LedFunctions.h"
#include "BatteryFunctions.h"
#include "TimeFunctions.h"
#include "GameDataSource.h"

// IMPORTANT: the default ESP32 Arduino loop() task stack (often only
// 8KB) is a well-known source of TLS/WSS connections failing silently
// or crashing the board - a TLS handshake needs more stack than that.
// Kept as a precaution given how much runs in loop() here (display,
// admin portal, live WebSocket) - not what caused the earlier
// "client.connect() returns false immediately" symptom (that turned out
// to be the ArduinoWebsockets setInsecure() bug documented in
// LiveGameClient.cpp), but still cheap insurance against TLS-under-
// stack-pressure issues in general. Must be a macro call at file scope,
// before setup().
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

static ClockState state = {
  .wifiConnected = false,
  .apMode = false,
  .wifiStrength = 0,
  .wifiSSID = "",
  .wifiQuality = "",
  .ipAddress = "",

  .batteryVoltage = 0.0f,
  .batteryPercentage = 100,

  .apiStatus = "",
  .apiOk = false,

  .hasGame = false,
  .players = {},
  .moveCount = 0,
  .activePlayerIndex = -1,
  .lastResultSummary = "",
  .lastGameOutcome = OUTCOME_NONE,
  .lastResultReason = "",
  .lastRatingDelta = 0,
  .lastRatingKnown = false,
  .lastOpponentUsername = "",
  .resultDisplayUntilMs = 0,
  .clockBaselineMs = {0, 0},
  .clockBaselineAtMs = 0,

  .lastDisplayUpdate = 0,
  .lastFullRefresh = 0,
};

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== IvoChess Clock ===");

  initDisplay();
  drawStartupScreen();
  initLEDs();
  initBatteryADC();

  initAdminPortal(&state);  // connects to saved WiFi OR starts the hotspot; loads PHPSESSID/language/source
  initTime();
  initGameDataSource();

  updateWiFiStatus(state);
  updateBatteryInfo(state);
  updateLEDs(state);

  state.lastFullRefresh = millis();
  updateDisplay(/*fullRefresh=*/true, state);
  state.lastDisplayUpdate = millis();
}

void loop() {
  // Always process the admin portal AND the live game WebSocket, every
  // single loop() iteration - neither should wait behind the 1s tick
  // below, or the WebSocket connection would lag/drop and the browser
  // would feel slow.
  handleAdminPortal();
  gameDataSourceFastTick(state);

  static unsigned long lastTick = 0;
  unsigned long now = millis();
  if (now - lastTick < 1000) {
    return;
  }
  lastTick = now;

  updateWiFiStatus(state);
  updateBatteryInfo(state);
  updateLEDs(state);

  bool gameDataChanged = updateGameData(state);

  static int lastMoveCountSeen = -1;
  bool moveChanged = state.hasGame && (state.moveCount != lastMoveCountSeen);
  lastMoveCountSeen = state.moveCount;

  bool showingResultNow = (state.resultDisplayUntilMs != 0) && (now < state.resultDisplayUntilMs);
  static bool wasShowingResult = false;
  bool resultWindowChanged = (showingResultNow != wasShowingResult);
  if (wasShowingResult && !showingResultNow) {
    // The result window (resultDisplayDurationMs) just ended - clear it
    // so the waiting screen goes back to "Waiting for game..." instead of
    // showing the old result forever.
    state.lastResultSummary[0] = '\0';
    state.lastGameOutcome = OUTCOME_NONE;
    state.resultDisplayUntilMs = 0;
  }
  wasShowingResult = showingResultNow;

  static bool lastLogoPhase = true;
  bool logoPhaseNow = (!state.hasGame) && !showingResultNow &&
                       (((now / SCREEN_CYCLE_INTERVAL_MS) % 2) == 0);
  bool phaseChanged = (logoPhaseNow != lastLogoPhase);
  lastLogoPhase = logoPhaseNow;

  bool needsFullRefresh = phaseChanged || gameDataChanged || resultWindowChanged || moveChanged ||
                          (now - state.lastFullRefresh) > FULL_REFRESH_INTERVAL_MS;

  static unsigned long lastClockRefresh = 0;

  if (needsFullRefresh) {
    // A new move (moveChanged) redraws immediately AND resets the 10s
    // countdown below - "aguarda novos 10 segundos ou um novo lance".
    updateDisplay(/*fullRefresh=*/true, state);
    state.lastFullRefresh = now;
    lastClockRefresh = now;
  } else if (state.hasGame && (now - lastClockRefresh >= GAME_CLOCK_REFRESH_INTERVAL_MS)) {
    // No real RSocket clock update landed during this window (that would
    // have gone through the moveChanged/needsFullRefresh branch above,
    // which resets lastClockRefresh) - extrapolate locally so the display
    // doesn't just sit frozen between RSocket updates. Computed fresh
    // from clockBaselineMs/clockBaselineAtMs (the last REAL values +
    // when they were captured - see ClockState.h) each time, rather than
    // chaining off the previously-displayed value, so it can't drift from
    // loop() timing jitter (e.g. a slow e-paper full refresh delaying
    // this branch past the nominal 10s) - always exact-elapsed-time
    // accurate relative to the real anchor point.
    if (state.activePlayerIndex == 0 || state.activePlayerIndex == 1) {
      int idx = state.activePlayerIndex;
      long elapsed = (long)(now - state.clockBaselineAtMs);
      long extrapolated = state.clockBaselineMs[idx] - elapsed;
      state.players[idx].clockMs = (extrapolated > 0) ? extrapolated : 0;
    }
    // A real partial-window e-paper refresh caused visual corruption in
    // earlier testing on real hardware, so for now we just do a full
    // redraw periodically instead of every second - less "live", but
    // reliable.
    updateDisplay(/*fullRefresh=*/true, state);
    lastClockRefresh = now;
  }
  state.lastDisplayUpdate = now;
}
