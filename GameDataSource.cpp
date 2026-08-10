#include "GameDataSource.h"
#include "config.h"
#include "ChessApiFunctions.h"
#include "LiveGameClient.h"
#include "LichessApiFunctions.h"
#include "LichessLiveClient.h"
#include "AdminPortal.h"
#include "ChessConnectBLE.h"

DataSourceType currentDataSource = DATA_SOURCE_CHESSCOM_WIFI;

const char *activeMyUsername() {
  switch (currentDataSource) {
    case DATA_SOURCE_LICHESS_WIFI: return lichessUsername;
    // ChessConnect never discloses which side is the local player (only
    // White/Black - see ChessConnectBLE.cpp's whiteIndex comment), so
    // there's no username to return here. Explicit empty string rather
    // than falling through to myUsername - that would only coincidentally
    // fail to match "White"/"Black" today; this way it can never match by
    // accident, and the intent (identity is genuinely unknown for this
    // source) is visible in the code, not just implied.
    case DATA_SOURCE_CHESSCONNECT_BLE: return "";
    case DATA_SOURCE_CHESSCOM_WIFI:
    default: return myUsername;
  }
}

static GameInfo currentGame;
static unsigned long lastPollAttempt = 0;

// Bounds how long the clock will keep polling /service/play/games while
// genuinely idle (connected, no game) - see WAITING_FOR_GAME_TIMEOUT_MS.
// idleSinceMs is reset to 'now' every time we're NOT in that idle state
// (offline, or a game is active), so it always reflects the start of the
// CURRENT idle streak, not idle time accumulated across separate streaks.
// pollingStoppedUntilRestart is a static local, like everything else
// here - it only clears on a real reboot, deliberately: this isn't a
// longer sleep timer, it's meant to force a conscious restart before
// polling resumes.
static unsigned long idleSinceMs = 0;
static bool pollingStoppedUntilRestart = false;

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
    idleSinceMs = now;  // being offline isn't "idle waiting for a game" - don't count it against the timeout
    return false;
  }

  if (state.hasGame) {
    idleSinceMs = now;  // reset the idle-streak clock; it starts counting again once this game ends
    return false;  // already watching live via gameDataSourceFastTick() - nothing to poll here
  }

  if (pollingStoppedUntilRestart) {
    state.apiOk = false;
    state.waitingTimedOut = true;
    snprintf(state.apiStatus, sizeof(state.apiStatus), "Idle timeout - restart to resume");
    return false;
  }

  if (now - idleSinceMs >= WAITING_FOR_GAME_TIMEOUT_MS) {
    pollingStoppedUntilRestart = true;
    state.apiOk = false;
    state.waitingTimedOut = true;
    snprintf(state.apiStatus, sizeof(state.apiStatus), "Idle timeout - restart to resume");
    Serial.printf("[GameDataSource] No game found for %lu min - stopping polling until restart.\n",
                  WAITING_FOR_GAME_TIMEOUT_MS / 60000UL);
    return false;
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

// ---- source: Lichess via WiFi (HTTP to find the game, then NDJSON stream for live data) ----
// Mirrors updateFromChessComWiFi()'s shape exactly (poll gate, idle
// timeout, same-game-id reconnect logic) - kept as a fully separate
// function/statics rather than sharing chess.com's, since only one
// source is ever active at a time (currentDataSource is a single value,
// not a set - see GameDataSource.h) but a user switching back and forth
// between services must not have one source's in-flight state corrupt
// the other's.
static LichessGameInfo currentLichessGame;
static unsigned long lastLichessPollAttempt = 0;
static unsigned long lichessIdleSinceMs = 0;
static bool lichessPollingStoppedUntilRestart = false;
static bool lichessLiveWasConnected = false;
static unsigned long lichessLiveConnectStartedAt = 0;

static bool updateFromLichessWiFi(ClockState &state) {
  unsigned long now = millis();

  if (!state.wifiConnected) {
    snprintf(state.apiStatus, sizeof(state.apiStatus), "No WiFi");
    state.apiOk = false;
    lichessIdleSinceMs = now;
    return false;
  }

  if (state.hasGame) {
    lichessIdleSinceMs = now;
    return false;  // already watching live via gameDataSourceFastTick() - nothing to poll here
  }

  if (lichessPollingStoppedUntilRestart) {
    state.apiOk = false;
    state.waitingTimedOut = true;
    snprintf(state.apiStatus, sizeof(state.apiStatus), "Idle timeout - restart to resume");
    return false;
  }

  if (now - lichessIdleSinceMs >= WAITING_FOR_GAME_TIMEOUT_MS) {
    lichessPollingStoppedUntilRestart = true;
    state.apiOk = false;
    state.waitingTimedOut = true;
    snprintf(state.apiStatus, sizeof(state.apiStatus), "Idle timeout - restart to resume");
    Serial.printf("[GameDataSource] No Lichess game found for %lu min - stopping polling until restart.\n",
                  WAITING_FOR_GAME_TIMEOUT_MS / 60000UL);
    return false;
  }

  if (now - lastLichessPollAttempt < LICHESS_GAME_POLL_INTERVAL_MS) {
    return false;
  }
  lastLichessPollAttempt = now;

  char previousGameId[sizeof(currentLichessGame.id)];
  strncpy(previousGameId, currentLichessGame.id, sizeof(previousGameId));
  previousGameId[sizeof(previousGameId) - 1] = '\0';

  bool found = lichessFetchActiveGame(lichessToken, currentLichessGame, state.apiStatus, sizeof(state.apiStatus));
  state.apiOk = (strncmp(state.apiStatus, "OK", 2) == 0);

  if (!found) {
    return false;
  }

  // Same reasoning as chess.com's reconnect-vs-new-game split
  // (updateFromChessComWiFi() above): a reconnect after a live-connection
  // blip must not reset the clocks/move count back to zero.
  bool sameGameAsBefore = (previousGameId[0] != '\0') && (strcmp(previousGameId, currentLichessGame.id) == 0);

  state.hasGame = true;

  // Populate players[] right away from the discovery poll (white=[0],
  // black=[1], matching the convention LichessLiveClient.cpp's gameFull
  // handling also uses) rather than waiting for the stream's gameFull
  // event to arrive - that event lands moments AFTER lichessLiveConnect()
  // returns (it's parsed incrementally as bytes stream in, not
  // synchronously during connect), so without this there'd be a brief
  // window with state.hasGame=true but stale players[] left over from
  // whatever game was on screen before. gameFull harmlessly overwrites
  // these same fields moments later with its own (equally authoritative)
  // copy.
  int meIdx = currentLichessGame.myColorIsWhite ? 0 : 1;
  int oppIdx = 1 - meIdx;
  strncpy(state.players[meIdx].username, lichessUsername, USERNAME_MAX_LEN - 1);
  state.players[meIdx].username[USERNAME_MAX_LEN - 1] = '\0';
  state.players[meIdx].rating = currentLichessGame.myRating;
  strncpy(state.players[oppIdx].username, currentLichessGame.opponent.username, USERNAME_MAX_LEN - 1);
  state.players[oppIdx].username[USERNAME_MAX_LEN - 1] = '\0';
  state.players[oppIdx].rating = currentLichessGame.opponent.rating;

  if (sameGameAsBefore) {
    Serial.printf("[GameDataSource] Reconnecting to Lichess game already in progress (id=%s) - keeping move %d / clocks as-is.\n",
                  currentLichessGame.id, state.moveCount);
    // moveCount/activePlayerIndex/clockBaseline* intentionally left alone - same reasoning as chess.com's reconnect path above.
  } else {
    Serial.printf("[GameDataSource] New Lichess game found: me(%s) vs %s(%d) - id=%s\n",
                  currentLichessGame.myColorIsWhite ? "white" : "black",
                  currentLichessGame.opponent.username, currentLichessGame.opponent.rating,
                  currentLichessGame.id);
    state.moveCount = 0;
    state.activePlayerIndex = -1;
    state.clockBaselineMs[0] = 0;
    state.clockBaselineMs[1] = 0;
    state.clockBaselineAtMs = now;

    // Same defensive clear as chess.com's new-game path - see its
    // comment above for why (fast back-to-back games, display priority).
    state.resultDisplayUntilMs = 0;
    state.lastResultSummary[0] = '\0';
    state.lastGameOutcome = OUTCOME_NONE;
  }

  lichessLiveWasConnected = false;
  lichessLiveConnectStartedAt = now;
  lichessLiveConnect(currentLichessGame, lichessToken);
  return true;
}

// ---- source: ChessConnect / DGT3000 via Bluetooth ----
// Unlike chess.com/Lichess, there's no "poll for a new game" step here at
// all - Chessconnect pushes everything proactively over BLE, applied via
// gameDataSourceFastTick() -> chessConnectBleLoop() every loop() iteration
// (see its override of the !hasGame early-return below, and
// ChessConnectBLE.cpp for the actual translation). This function only
// supplies a sensible waiting-screen status the very first time this
// source is ever polled - after that, chessConnectBleLoop() (running far
// more often) is the sole owner of state.apiStatus for this source.
static bool updateFromChessConnectBLE(ClockState &state) {
  static bool initialStatusSet = false;
  if (!initialStatusSet) {
    initialStatusSet = true;
    snprintf(state.apiStatus, sizeof(state.apiStatus), "Waiting for Chessconnect...");
    state.apiOk = false;
  }
  return false;  // never "just found a new game" via this path - chessConnectBleLoop() already applies it directly when it happens
}

void initGameDataSource() {
  // The source itself (currentDataSource) is already loaded by
  // AdminPortal (Preferences) before this function is called.
}

bool updateGameData(ClockState &state) {
  switch (currentDataSource) {
    case DATA_SOURCE_CHESSCONNECT_BLE:
      return updateFromChessConnectBLE(state);
    case DATA_SOURCE_LICHESS_WIFI:
      return updateFromLichessWiFi(state);
    case DATA_SOURCE_CHESSCOM_WIFI:
    default:
      // Keeping chess.com as the default (not e.g. erroring) means a
      // datasrc value this build doesn't recognize - a downgrade after
      // Lichess support existed, say - degrades safely to the
      // best-established path instead of doing nothing.
      return updateFromChessComWiFi(state);
  }
}

static void chessComFastTick(ClockState &state) {
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

// Mirrors chessComFastTick() exactly, against LichessLiveClient.h's
// equivalent functions instead - see updateFromLichessWiFi() above for
// why this stays a fully separate function/statics rather than sharing
// chess.com's.
static void lichessFastTick(ClockState &state) {
  lichessLiveLoop(state);

  if (lichessLiveIsConnected()) {
    lichessLiveWasConnected = true;

    if (lichessLiveIsStale()) {
      Serial.println("[GameDataSource] Lichess live connection stale (no lines received) - forcing reconnect.");
      lichessLiveDisconnect();
      state.hasGame = false;
      lichessLiveWasConnected = false;
    }
    return;
  }

  bool gaveUpOnHandshake = !lichessLiveWasConnected && (millis() - lichessLiveConnectStartedAt > LIVE_CONNECT_GRACE_MS);
  if (lichessLiveWasConnected || gaveUpOnHandshake) {
    Serial.println("[GameDataSource] Lichess live connection lost/failed to establish - falling back to polling.");
    lichessLiveDisconnect();
    state.hasGame = false;
    lichessLiveWasConnected = false;
  }
}

void gameDataSourceFastTick(ClockState &state) {
  // ChessConnect must run BEFORE the !hasGame guard below, unlike the
  // other two sources: BLE events (including the very first setTime of a
  // brand new game, which is what SETS hasGame true in the first place)
  // arrive independently of whatever hasGame currently is. chess.com/
  // Lichess's fast-tick functions only ever pump an ALREADY-established
  // live connection, so gating them behind hasGame is correct for them -
  // it just isn't for a passive BLE peripheral waiting on its first
  // command.
  if (currentDataSource == DATA_SOURCE_CHESSCONNECT_BLE) {
    chessConnectBleLoop(state);
    return;
  }

  if (!state.hasGame) {
    return;
  }
  switch (currentDataSource) {
    case DATA_SOURCE_LICHESS_WIFI:
      lichessFastTick(state);
      return;
    case DATA_SOURCE_CHESSCOM_WIFI:
      chessComFastTick(state);
      return;
    default:
      return;
  }
}
