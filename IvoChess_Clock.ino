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
 * RESULT_DISPLAY_DURATION_MS after the game ends).
 *
 * REQUIRED BOARD SETTING (Arduino IDE > Tools > Partition Scheme):
 *   "Huge APP (3MB No OTA/1MB SPIFFS)" - the default scheme only gives
 *   1.2MB to the app, and this sketch (GxEPD2 + fonts + ArduinoWebsockets
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
 *   - ArduinoWebsockets (Gil Maimon) - for the live RSocket connection.
 *     Switched from "WebSockets" (Links2004) after that one kept
 *     dropping the connection with no server-provided reason; this one
 *     skips TLS cert validation by default on ESP32 and has cleaner
 *     custom-header support (both were suspects).
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
// This is a strong suspect for BOTH the mysterious reboot we saw once
// AND client.connect() returning false immediately with WiFi already
// connected. Must be a macro call at file scope, before setup().
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
  .resultDisplayUntilMs = 0,

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
    // The 15s result window just ended - clear it so the waiting screen
    // goes back to "Waiting for game..." instead of showing the old
    // result forever.
    state.lastResultSummary[0] = '\0';
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
    // Real partial-window refresh (updateGameClocksPartial) caused
    // visual corruption on real hardware, so for now we just do a full
    // redraw periodically instead of every second - less "live", but
    // reliable. See DisplayFunctions.cpp for notes on revisiting this.
    updateDisplay(/*fullRefresh=*/true, state);
    lastClockRefresh = now;
  }
  state.lastDisplayUpdate = now;
}
