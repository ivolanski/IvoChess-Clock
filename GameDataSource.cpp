#include "GameDataSource.h"
#include "config.h"
#include "ChessApiFunctions.h"
#include "LiveGameClient.h"

DataSourceType currentDataSource = DATA_SOURCE_CHESSCOM_WIFI;

static GameInfo currentGame;
static unsigned long lastPollAttempt = 0;

// Tracks the live WebSocket connection across ticks so we can notice when
// it's lost and fall back to polling - liveGameIsConnected() existed but
// was never actually checked anywhere, so a dropped connection (network
// hiccup, server hiccup, anything short of a clean "results" event) left
// state.hasGame stuck true forever with stale clocks/move count on screen.
static bool liveWasConnected = false;
static unsigned long liveConnectStartedAt = 0;
#define LIVE_CONNECT_GRACE_MS 8000  // handshake can take a few seconds; don't bail before giving it a fair shot

// ---- source: chess.com via WiFi (HTTP to find the game, then RSocket/WebSocket for live data) ----
static bool updateFromChessComWiFi(ClockState &state) {
  unsigned long now = millis();

  if (!state.wifiConnected) {
    snprintf(state.apiStatus, sizeof(state.apiStatus), "No WiFi");
    state.apiOk = false;
    return false;
  }

  if (state.hasGame) {
    return false;  // already watching live via gameDataSourceFastTick() - nothing to poll here
  }

  if (now - lastPollAttempt < GAME_POLL_INTERVAL_MS) {
    return false;
  }
  lastPollAttempt = now;

  bool found = fetchActiveGame(currentGame, state.apiStatus, sizeof(state.apiStatus));
  state.apiOk = (strncmp(state.apiStatus, "OK", 2) == 0);

  if (!found) {
    return false;
  }

  state.hasGame = true;
  state.players[0] = currentGame.players[0];
  state.players[1] = currentGame.players[1];
  state.moveCount = 0;
  state.activePlayerIndex = -1;

  liveWasConnected = false;
  liveConnectStartedAt = now;
  liveGameConnect(currentGame);
  return true;
}

// ---- source: ChessConnect / DGT3000 via Bluetooth (NOT IMPLEMENTED YET) ----
static bool updateFromChessConnectBLE(ClockState &state) {
  // TODO: implement once the ChessConnect source code arrives.
  snprintf(state.apiStatus, sizeof(state.apiStatus), "ChessConnect not implemented");
  state.apiOk = false;
  return false;
}

void initGameDataSource() {
  // The source itself (currentDataSource) is already loaded by
  // AdminPortal (Preferences) before this function is called.
}

bool updateGameData(ClockState &state) {
  switch (currentDataSource) {
    case DATA_SOURCE_CHESSCONNECT_BLE:
      return updateFromChessConnectBLE(state);
    case DATA_SOURCE_CHESSCOM_WIFI:
    default:
      return updateFromChessComWiFi(state);
  }
}

void gameDataSourceFastTick(ClockState &state) {
  if (currentDataSource != DATA_SOURCE_CHESSCOM_WIFI || !state.hasGame) {
    return;
  }

  liveGameLoop(state);

  if (liveGameIsConnected()) {
    liveWasConnected = true;
    return;
  }

  // Not connected right now - fine while the handshake is still in
  // flight, but if it either never came up within the grace window or
  // dropped after being up, stop pretending we're watching a live game:
  // fall back to state.hasGame=false so updateFromChessComWiFi() resumes
  // polling /service/play/games (re-finds the same game and reconnects,
  // or notices it ended and shows the waiting screen instead of leaving
  // stale clocks on screen forever).
  bool gaveUpOnHandshake = !liveWasConnected && (millis() - liveConnectStartedAt > LIVE_CONNECT_GRACE_MS);
  if (liveWasConnected || gaveUpOnHandshake) {
    Serial.println("[GameDataSource] Live connection lost/failed to establish - falling back to polling.");
    liveGameDisconnect();
    state.hasGame = false;
    liveWasConnected = false;
  }
}
