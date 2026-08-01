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
#define LIVE_CONNECT_GRACE_MS 15000  // handshake can take a few seconds, more so when loop() is delayed by a blocking e-paper refresh; don't bail before giving it a fair shot

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

  char previousGameId[sizeof(currentGame.id)];
  strncpy(previousGameId, currentGame.id, sizeof(previousGameId));
  previousGameId[sizeof(previousGameId) - 1] = '\0';

  bool found = fetchActiveGame(currentGame, state.apiStatus, sizeof(state.apiStatus));
  state.apiOk = (strncmp(state.apiStatus, "OK", 2) == 0);

  if (!found) {
    return false;
  }

  // gameDataSourceFastTick() drops state.hasGame back to false whenever
  // the live WebSocket connection is lost/stale, specifically so we come
  // back here and reconnect - NOT so we treat "still the same game" as
  // "a new game starting". Re-zeroing moveCount/activePlayerIndex, and
  // especially clockMs (currentGame.players[].clockMs is always 0 here -
  // the HTTP discovery endpoint never carries live clock values, only
  // the RSocket stream does), on every reconnect was flashing the screen
  // back to "Move 0 / 00:00" mid-game each time the connection blipped.
  bool sameGameAsBefore = (previousGameId[0] != '\0') && (strcmp(previousGameId, currentGame.id) == 0);

  state.hasGame = true;

  if (sameGameAsBefore) {
    Serial.printf("[GameDataSource] Reconnecting to game already in progress (id=%s) - keeping move %d / clocks as-is.\n",
                  currentGame.id, state.moveCount);
    for (int i = 0; i < 2; i++) {
      strncpy(state.players[i].username, currentGame.players[i].username, USERNAME_MAX_LEN - 1);
      state.players[i].username[USERNAME_MAX_LEN - 1] = '\0';
      state.players[i].rating = currentGame.players[i].rating;
      // clockMs, moveCount, activePlayerIndex, clockBaseline* intentionally left alone.
    }
  } else {
    Serial.printf("[GameDataSource] New game found: %s(%d) vs %s(%d) - id=%s\n",
                  currentGame.players[0].username, currentGame.players[0].rating,
                  currentGame.players[1].username, currentGame.players[1].rating, currentGame.id);
    state.players[0] = currentGame.players[0];
    state.players[1] = currentGame.players[1];
    state.moveCount = 0;
    state.activePlayerIndex = -1;
    state.clockBaselineMs[0] = currentGame.players[0].clockMs;
    state.clockBaselineMs[1] = currentGame.players[1].clockMs;
    state.clockBaselineAtMs = now;

    // Defensive: a new game starting always takes display priority over
    // any still-counting-down result screen anyway (see
    // DisplayFunctions.cpp updateDisplay() - hasGame wins), but clear the
    // old result explicitly too so there's no leftover state from a
    // previous game if this ever gets refactored. Matters most for fast
    // back-to-back games (opponent aborts, you immediately start
    // another) - each game-over event already overwrites all of this
    // fresh (LiveGameClient.cpp), so this only guards the in-between gap.
    state.resultDisplayUntilMs = 0;
    state.lastResultSummary[0] = '\0';
    state.lastGameOutcome = OUTCOME_NONE;
  }

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

    // The TCP socket can go quietly one-way-dead without ever firing a
    // disconnect event (server/NAT/network just stops delivering, no
    // FIN/RST seen) - liveGameIsConnected() alone can't see that.
    // liveGameIsStale() catches it by watching for a gap since the last
    // RSocket frame (KEEPALIVE included) well past the server's normal
    // keepalive cadence.
    if (liveGameIsStale()) {
      Serial.println("[GameDataSource] Live connection stale (no frames received) - forcing reconnect.");
      liveGameDisconnect();
      state.hasGame = false;
      liveWasConnected = false;
    }
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
