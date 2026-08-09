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
  .waitingTimedOut = false,

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
  .lastMyRating = 0,
  .lastOpponentRating = 0,
  .lastPlayerRatingsKnown = false,
  .resultDisplayUntilMs = 0,
  .clockBaselineMs = {0, 0},
  .clockBaselineAtMs = 0,

  .lastDisplayUpdate = 0,
  .lastFullRefresh = 0,
  .lastGameActiveAt = 0,
};

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== IvoChess Clock ===");

  startDisplayTask();  // owns initDisplay()/drawStartupScreen() itself now - see DisplayFunctions.cpp
  initLEDs();
  initBatteryADC();

  initAdminPortal(&state);  // connects to saved WiFi OR starts the hotspot; loads PHPSESSID/language/source
  initTime();
  initGameDataSource();

  updateWiFiStatus(state);
  updateBatteryInfo(state);
  updateLEDs(state);

  state.lastFullRefresh = millis();
  requestDisplayUpdate(/*fullRefresh=*/true, state);
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

  if (state.hasGame) {
    state.lastGameActiveAt = now;
  }

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
  bool logoPhaseNow = isLogoPhaseNow(state);
  bool phaseChanged = (logoPhaseNow != lastLogoPhase);
  lastLogoPhase = logoPhaseNow;

  // moveChanged is deliberately NOT part of this - a move is handled in
  // its own branch below (drawGameMovePartial()), since nothing else on
  // the game screen actually needs a truly full redraw when a move is
  // the only thing that happened (names are the only static piece once a
  // game starts; everything else - rating display aside, ratings do
  // change - already gets redrawn by the partial path).
  bool needsFullRefresh = phaseChanged || gameDataChanged || resultWindowChanged ||
                          (now - state.lastFullRefresh) > FULL_REFRESH_INTERVAL_MS;

  static unsigned long lastClockRefresh = 0;
  // Consecutive partial-only redraws (clock ticks OR move updates) since
  // the last TRUE full redraw - shared by both, since both use the same
  // fast partial waveform and contribute to the same e-paper ghosting.
  // See GAME_CLOCK_PARTIAL_REFRESH_MAX_STREAK (config.h).
  static int partialRefreshStreak = 0;

  if (needsFullRefresh) {
    // Any full redraw here also clears accumulated e-paper ghosting from
    // the partial-only redraws below, so reset that streak too.
    requestDisplayUpdate(/*fullRefresh=*/true, state);
    state.lastFullRefresh = now;
    lastClockRefresh = now;
    partialRefreshStreak = 0;
  } else if (moveChanged && state.hasGame) {
    // A move happened, but nothing else that would need a truly full
    // redraw - try the targeted move partial refresh (drawGameMovePartial(),
    // DisplayFunctions.cpp), bounded by the same ghosting streak as the
    // per-second clock-only tick below.
    if (partialRefreshStreak >= GAME_CLOCK_PARTIAL_REFRESH_MAX_STREAK) {
      requestDisplayUpdate(/*fullRefresh=*/true, state);
      state.lastFullRefresh = now;
      partialRefreshStreak = 0;
    } else {
      requestGameMovePartialRefresh(state);
      partialRefreshStreak++;
    }
    // A move already carries a real RSocket/board-API clock value, not an
    // extrapolation - nothing for the periodic-tick branch below to add
    // this same second.
    lastClockRefresh = now;
  } else if (state.hasGame && (now - lastClockRefresh >= GAME_CLOCK_REFRESH_INTERVAL_MS)) {
    // No real move/clock update landed during this window (that would
    // have gone through the moveChanged branch above, which resets
    // lastClockRefresh) - extrapolate locally so the display doesn't just
    // sit frozen between updates. Computed fresh from clockBaselineMs/
    // clockBaselineAtMs (the last REAL values + when they were captured -
    // see ClockState.h) each time, rather than chaining off the
    // previously-displayed value, so it can't drift from loop() timing
    // jitter - always exact-elapsed-time accurate relative to the real
    // anchor point.
    bool didPartial = false;
    if (state.activePlayerIndex == 0 || state.activePlayerIndex == 1) {
      int idx = state.activePlayerIndex;
      bool useFullRefresh = partialRefreshStreak >= GAME_CLOCK_PARTIAL_REFRESH_MAX_STREAK;

      // + latency offset: the value computed here won't actually be
      // visible until the redraw below finishes, so extrapolate to THAT
      // moment, not to "now" - otherwise the clock reads a fixed amount
      // behind real time the instant it's drawn (this is the only place
      // either offset applies - a real clock value elsewhere is exact and
      // is never adjusted like this). Which offset depends on which kind
      // of redraw is about to happen below.
      long latency = useFullRefresh ? DISPLAY_REFRESH_LATENCY_MS : DISPLAY_REFRESH_LATENCY_PARTIAL_MS;
      long elapsed = (long)(now - state.clockBaselineAtMs) + latency;
      long extrapolated = state.clockBaselineMs[idx] - elapsed;
      state.players[idx].clockMs = (extrapolated > 0) ? extrapolated : 0;

      if (useFullRefresh) {
        // Ghosting safety net (GAME_CLOCK_PARTIAL_REFRESH_MAX_STREAK) -
        // see config.h. Also keeps FULL_REFRESH_INTERVAL_MS's watchdog
        // timer in sync so it doesn't fire again immediately after.
        requestDisplayUpdate(/*fullRefresh=*/true, state);
        state.lastFullRefresh = now;
        partialRefreshStreak = 0;
      } else {
        // True partial-window refresh of just this player's clock digits
        // - see drawGameClockPartial() (DisplayFunctions.cpp) for how,
        // and why an earlier attempt at this corrupted the display.
        requestGameClockPartialRefresh(state);
        partialRefreshStreak++;
      }
      didPartial = true;
    }
    if (!didPartial) {
      // activePlayerIndex isn't a valid side (shouldn't normally happen
      // mid-game) - nothing sensible to tick, but still redraw so the
      // screen doesn't silently go stale.
      requestDisplayUpdate(/*fullRefresh=*/true, state);
      state.lastFullRefresh = now;
      partialRefreshStreak = 0;
    }
    lastClockRefresh = now;
  }
  state.lastDisplayUpdate = now;
}
