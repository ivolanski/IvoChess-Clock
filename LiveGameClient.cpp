#include "LiveGameClient.h"
#include "config.h"
#include "AdminPortal.h"
#include "RSocketCodec.h"
#include "Translations.h"

#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <WiFi.h>

// ---------------------------------------------------------------------------
// This ports the RSocket application logic from a Python prototype that
// was validated against the real chess.com server. Key points carried
// over 1:1:
//   - mime types: message/x.rsocket.routing.v0 (metadata) / application/json (data)
//   - the server sends an application-level "ping" as REQUEST_RESPONSE
//     (metadata=route "echo", data=timestamp) that MUST be echoed back
//     (PAYLOAD, complete+next) or the connection gets dropped.
//   - the FINAL event on the watch route carries a "results" field
//     directly - that's the primary (and, for now, only) way this
//     client detects game-over. No HTTP-polling fallback yet (the
//     Python version had one; skipped here for v1 to keep scope manageable).
//   - "who is on the clock" is inferred the same way as the Python
//     GameDisplayState.update_from_event(): on a move-count change,
//     assume the turn flipped (chess always alternates); on repeated
//     events within the same move, whichever clock actually decreased
//     confirms/corrects the guess.
//
// WEBSOCKET LIBRARY: WebSockets (Links2004/Markus Sattler), NOT
// ArduinoWebsockets (gilmaimon) - that one was tried first because it
// "does not validate the TLS certificate chain by default on ESP32", but
// that turned out to be a wrong assumption: its ESP32 branch of
// upgradeToSecuredConnection() (websockets_client.cpp) never actually
// calls setInsecure() on the underlying WiFiClientSecure - its own
// WebsocketsClient::setInsecure() just clears a few cert pointers that
// were already null, so the real TLS client had no CA and no
// insecure-mode set, and client.connect() failed immediately (returned
// false) every time. WebSockets (Links2004) needed a real fix too - its
// own bug (see beginWatching()/setExtraHeaders below) is now fixed, and
// this is the exact same code path already validated live against
// chess.com on this hardware.
//
// IMPORTANT: this depends on a ONE-LINE PATCH to the locally installed
// WebSockets library (WebSocketsClient.cpp, in sendHeader()) - see the
// comment on setExtraHeaders() below for what it fixes and why it's
// needed. If this library gets reinstalled/updated, redo that patch or
// the connection will drop right after every SETUP frame.
// ---------------------------------------------------------------------------

static WebSocketsClient webSocket;
static ClockState *g_state = nullptr;
static bool connected = false;
static char currentWatchRoute[160] = "";
static uint32_t requestStreamId = 1;
static unsigned long lastFrameReceivedAtMs = 0;

// Generous margin over RSOCKET_KEEPALIVE_MS (the server sends a KEEPALIVE
// at roughly that cadence) - a real network hiccup or a slow loop() tick
// (e.g. a blocking full e-paper refresh) can delay one KEEPALIVE without
// anything actually being wrong.
#define LIVE_STALE_THRESHOLD_MS (RSOCKET_KEEPALIVE_MS * 3)

static long lastClocks[2] = {0, 0};
static int lastMoveCount = -1;
static int activeIndexGuess = -1;
static bool haveLastClocks = false;

static void resetTrackingState() {
  lastClocks[0] = lastClocks[1] = 0;
  lastMoveCount = -1;
  activeIndexGuess = -1;
  haveLastClocks = false;
}

static void updateActiveIndex(long newClocks[2], int moveCount) {
  if (lastMoveCount != -1 && moveCount != lastMoveCount) {
    // New move: the turn always alternates in chess - guess the flip
    // immediately (more responsive). Do NOT let this event's raw clock
    // delta override the guess here - it still reflects the time the
    // player who JUST moved spent thinking, not who's active now. The
    // next event within the same move confirms/corrects it below.
    if (activeIndexGuess != -1) {
      activeIndexGuess = 1 - activeIndexGuess;
    }
  } else if (haveLastClocks) {
    // Same move: whichever clock actually went down is who's on the clock.
    long diff0 = lastClocks[0] - newClocks[0];
    long diff1 = lastClocks[1] - newClocks[1];
    if (diff0 > 0 || diff1 > 0) {
      activeIndexGuess = (diff0 >= diff1) ? 0 : 1;
    }
  }
  lastClocks[0] = newClocks[0];
  lastClocks[1] = newClocks[1];
  lastMoveCount = moveCount;
  haveLastClocks = true;
}

static void sendFrame(const uint8_t *buf, size_t len) {
  webSocket.sendBIN(buf, len);
}

static void handlePayloadJson(const char *jsonStr, size_t jsonLen) {
  DynamicJsonDocument doc(2048);
  DeserializationError err = deserializeJson(doc, jsonStr, jsonLen);
  if (err) {
    Serial.print("[LiveGame] JSON parse error: ");
    Serial.println(err.c_str());
    return;
  }

  if (!doc["results"].isNull()) {
    // Game over - this event carries the final result directly.
    JsonArray results = doc["results"];
    JsonArray ratings = doc["ratings"];

    int winnerIdx = -1;
    for (int i = 0; i < (int)results.size() && i < 2; i++) {
      const char *r = results[i];  // null if this element isn't a string - guard before strcmp (crashes on nullptr)
      if (r != nullptr && strcmp(r, "win") == 0) winnerIdx = i;
    }

    int meIdx = -1;
    if (myUsername[0] != '\0') {
      for (int i = 0; i < 2; i++) {
        if (strcasecmp(g_state->players[i].username, myUsername) == 0) {
          meIdx = i;
          break;
        }
      }
    }

    GameOutcome outcome = OUTCOME_NONE;
    if (winnerIdx == -1) {
      outcome = OUTCOME_DRAW;
    } else if (meIdx != -1) {
      outcome = (winnerIdx == meIdx) ? OUTCOME_WIN : OUTCOME_LOSS;
    }
    // else: myUsername isn't configured, so win/loss can't be resolved -
    // stays OUTCOME_NONE (LedFunctions.cpp treats that as "idle/off"),
    // and the summary text below falls back to the neutral per-player form.
    g_state->lastGameOutcome = outcome;

    // Reason: chess.com gives one string per player ("resigned",
    // "checkmated", "timeout", "agreed", "repetition"...) - the LOSING
    // side's is the actual cause of a decisive game; for a draw both
    // sides normally carry the same word (e.g. "agreed"/"agreed").
    int reasonIdx = (winnerIdx == -1) ? 0 : (1 - winnerIdx);
    const char *reason = (reasonIdx < (int)results.size()) ? (const char *)results[reasonIdx] : nullptr;
    if (reason == nullptr) reason = "?";
    strncpy(g_state->lastResultReason, reason, sizeof(g_state->lastResultReason) - 1);
    g_state->lastResultReason[sizeof(g_state->lastResultReason) - 1] = '\0';

    int opponentIdx = (meIdx == -1) ? -1 : (1 - meIdx);
    if (opponentIdx != -1) {
      strncpy(g_state->lastOpponentUsername, g_state->players[opponentIdx].username, sizeof(g_state->lastOpponentUsername) - 1);
      g_state->lastOpponentUsername[sizeof(g_state->lastOpponentUsername) - 1] = '\0';
    } else {
      g_state->lastOpponentUsername[0] = '\0';
    }

    g_state->lastRatingKnown = (meIdx != -1 && meIdx < (int)ratings.size());
    g_state->lastRatingDelta = 0;
    if (g_state->lastRatingKnown) {
      JsonArray pair = ratings[meIdx];
      g_state->lastRatingDelta = pair[1] | 0;
    }

    char summary[128];
    if (outcome != OUTCOME_NONE) {
      const char *headline = (outcome == OUTCOME_WIN) ? T(STR_YOU_WON) : (outcome == OUTCOME_LOSS) ? T(STR_YOU_LOST) : T(STR_DRAW);
      if (g_state->lastRatingKnown) {
        snprintf(summary, sizeof(summary), "%s (%s) [%+d]", headline, reason, g_state->lastRatingDelta);
      } else {
        snprintf(summary, sizeof(summary), "%s (%s)", headline, reason);
      }
    } else {
      // Fallback (myUsername not configured): neutral "name: reason [rating]" per player.
      int pos = 0;
      for (int i = 0; i < 2; i++) {
        const char *r = (i < (int)results.size()) ? (const char *)results[i] : nullptr;
        if (r == nullptr) r = "?";
        int ratingNew = 0, ratingDelta = 0;
        if (i < (int)ratings.size()) {
          JsonArray pair = ratings[i];
          ratingNew = pair[0] | 0;
          ratingDelta = pair[1] | 0;
        }
        int written = snprintf(summary + pos, sizeof(summary) - pos, "%s%s: %s [%d %+d]",
                                (i == 0) ? "" : " | ", g_state->players[i].username, r, ratingNew, ratingDelta);
        if (written < 0) break;
        pos += written;
        if (pos >= (int)sizeof(summary)) break;
      }
    }

    Serial.printf("[LiveGame] Game finished: %s\n", summary);

    strncpy(g_state->lastResultSummary, summary, sizeof(g_state->lastResultSummary) - 1);
    g_state->lastResultSummary[sizeof(g_state->lastResultSummary) - 1] = '\0';
    g_state->hasGame = false;
    g_state->resultDisplayUntilMs = millis() + resultDisplayDurationMs;

    liveGameDisconnect();
    return;
  }

  JsonArray clocksArr = doc["clocks"];
  JsonArray movesArr = doc["moves"];

  long newClocks[2] = {g_state->players[0].clockMs, g_state->players[1].clockMs};
  for (int i = 0; i < (int)clocksArr.size() && i < 2; i++) {
    newClocks[i] = clocksArr[i] | 0L;
  }

  int moveCount = movesArr.isNull() ? g_state->moveCount : (int)movesArr.size();

  updateActiveIndex(newClocks, moveCount);

  g_state->players[0].clockMs = newClocks[0];
  g_state->players[1].clockMs = newClocks[1];
  g_state->moveCount = moveCount;
  g_state->activePlayerIndex = activeIndexGuess;

  // Fresh anchor for local extrapolation (IvoChess_Clock.ino) between now
  // and whenever the next real RSocket clock update lands.
  g_state->clockBaselineMs[0] = newClocks[0];
  g_state->clockBaselineMs[1] = newClocks[1];
  g_state->clockBaselineAtMs = millis();
}

static void handleFrame(const uint8_t *payload, size_t len) {
  RSocketFrame frame;
  if (!decodeFrame(payload, len, frame)) {
    return;
  }

  switch (frame.frameType) {
    case RS_KEEPALIVE: {
      if (frame.flags & RS_FLAG_RESPOND) {
        uint8_t buf[32];
        size_t n = encodeKeepalive(buf, sizeof(buf), frame.keepaliveposition);
        if (n > 0) sendFrame(buf, n);
      }
      break;
    }
    case RS_PAYLOAD: {
      if (frame.data != nullptr && frame.dataLen > 0) {
        handlePayloadJson((const char *)frame.data, frame.dataLen);
      }
      break;
    }
    case RS_REQUEST_RESPONSE: {
      // Application-level ping from the server (observed: metadata route
      // "echo", data=timestamp) - echo it back or the server drops the
      // connection.
      uint8_t buf[256];
      size_t n = encodePayload(buf, sizeof(buf), frame.streamId,
                                frame.metadata, frame.metadataLen,
                                frame.data, frame.dataLen);
      if (n > 0) sendFrame(buf, n);
      break;
    }
    case RS_ERROR: {
      Serial.print("[LiveGame] RSocket ERROR: ");
      if (frame.data) {
        for (size_t i = 0; i < frame.dataLen; i++) Serial.print((char)frame.data[i]);
      }
      Serial.println();
      break;
    }
    default:
      break;
  }
}

static void onWsEvent(WStype_t type, uint8_t *payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED: {
      Serial.printf("[LiveGame] WebSocket connected (free heap: %u bytes), sending SETUP + REQUEST_STREAM...\n", ESP.getFreeHeap());

      uint8_t buf[128];
      size_t n = encodeSetup(buf, sizeof(buf), RSOCKET_KEEPALIVE_MS, RSOCKET_LIFETIME_MS);
      if (n > 0) sendFrame(buf, n);

      n = encodeRequestStream(buf, sizeof(buf), requestStreamId, currentWatchRoute);
      if (n > 0) sendFrame(buf, n);

      connected = true;
      lastFrameReceivedAtMs = millis();
      break;
    }
    case WStype_DISCONNECTED:
      Serial.printf("[LiveGame] WebSocket disconnected (free heap: %u bytes)\n", ESP.getFreeHeap());
      connected = false;
      break;
    case WStype_BIN:
      lastFrameReceivedAtMs = millis();
      handleFrame(payload, length);
      break;
    case WStype_ERROR:
      Serial.println("[LiveGame] WebSocket error.");
      break;
    default:
      break;  // text/ping/pong not expected on this connection
  }
}

void liveGameConnect(const GameInfo &game) {
  strncpy(currentWatchRoute, game.watchRoute, sizeof(currentWatchRoute) - 1);
  currentWatchRoute[sizeof(currentWatchRoute) - 1] = '\0';
  requestStreamId = 1;
  resetTrackingState();

  // Same header set the validated Python client sends for this exact
  // connection, plus a few headers a real Chrome browser sends on a
  // same-origin WebSocket upgrade (Accept-Language, Sec-Fetch-*) so this
  // doesn't stick out as an obviously-non-browser client.
  //
  // NO trailing "\r\n" after the LAST header line here: WebSocketsClient
  // does "handshake += extraHeaders + NEW_LINE" internally (one \r\n
  // added FOR you after this whole block). If the last line already
  // ended in \r\n, that became "\r\n\r\n" - a premature blank line in the
  // middle of the HTTP request - and everything the library appended
  // after (its own default User-Agent + the real terminating blank line)
  // became leftover bytes that chess.com's RSocket backend read as the
  // first (garbage) WebSocket frame and closed the connection over,
  // ~50ms after every SETUP frame. That was the actual bug behind the
  // "connection lost, no server-provided reason" this project hit
  // before switching away from this library the first time.
  char cookieValue[PHPSESSID_MAX_LEN + 32];
  snprintf(cookieValue, sizeof(cookieValue), "PHPSESSID=%s", phpsessid);
  String headers;
  headers += "Cookie: " + String(cookieValue) + "\r\n";
  headers += "Origin: https://www.chess.com\r\n";
  headers += "Accept-Language: pt-BR,pt;q=0.9,en-US;q=0.8,en;q=0.7\r\n";
  headers += "Sec-Fetch-Dest: empty\r\n";
  headers += "Sec-Fetch-Mode: websocket\r\n";
  headers += "Sec-Fetch-Site: same-origin\r\n";
  headers += "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
             "(KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36";
  webSocket.setExtraHeaders(headers.c_str());
  webSocket.onEvent(onWsEvent);

  Serial.printf("[LiveGame] Free heap before connecting: %u bytes\n", ESP.getFreeHeap());
  Serial.printf("[LiveGame] Connecting to wss://www.chess.com%s (route=%s)\n", game.rsocketUrl, currentWatchRoute);

  lastFrameReceivedAtMs = millis();  // reset the staleness clock before the handshake even starts

  // game.rsocketUrl is a PATH relative to www.chess.com (e.g.
  // "/service/play-eu-1-2/rsocket"), not a full URL with its own host -
  // matches what ChessApiFunctions.cpp stores it as, straight from the
  // chess.com API response.
  webSocket.beginSSL("www.chess.com", 443, game.rsocketUrl, "", "");  // no fingerprint, no subprotocol
}

void liveGameDisconnect() {
  webSocket.disconnect();
  connected = false;
}

bool liveGameIsConnected() {
  return connected;
}

bool liveGameIsStale() {
  return connected && (millis() - lastFrameReceivedAtMs > LIVE_STALE_THRESHOLD_MS);
}

void liveGameLoop(ClockState &state) {
  g_state = &state;
  webSocket.loop();
}
