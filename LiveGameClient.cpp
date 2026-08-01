#include "LiveGameClient.h"
#include "config.h"
#include "AdminPortal.h"
#include "RSocketCodec.h"

#include <ArduinoWebsockets.h>
#include <ArduinoJson.h>
#include <WiFi.h>

using namespace websockets;

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
// WEBSOCKET LIBRARY: using ArduinoWebsockets (gilmaimon) instead of
// WebSockets (Links2004) - switched after the Links2004 client kept
// dropping the connection right after opening ("Connection lost", no
// server-provided reason) despite matching every header the validated
// Python client sends. ArduinoWebsockets does not validate the TLS
// certificate chain by default on ESP32 (no fingerprint/CA dance
// needed), which was one of our suspects.
// ---------------------------------------------------------------------------

static WebsocketsClient client;
static ClockState *g_state = nullptr;
static bool connected = false;
static char currentWatchRoute[160] = "";
static uint32_t requestStreamId = 1;

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
  client.sendBinary((const char *)buf, len);
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

    char summary[128];
    int pos = 0;
    for (int i = 0; i < 2; i++) {
      const char *reason = (i < (int)results.size()) ? (const char *)results[i] : "?";
      int ratingNew = 0, ratingDelta = 0;
      if (i < (int)ratings.size()) {
        JsonArray pair = ratings[i];
        ratingNew = pair[0] | 0;
        ratingDelta = pair[1] | 0;
      }
      int written = snprintf(summary + pos, sizeof(summary) - pos, "%s%s: %s [%d %+d]",
                              (i == 0) ? "" : " | ", g_state->players[i].username, reason, ratingNew, ratingDelta);
      if (written < 0) break;
      pos += written;
      if (pos >= (int)sizeof(summary)) break;
    }

    Serial.printf("[LiveGame] Game finished: %s\n", summary);

    strncpy(g_state->lastResultSummary, summary, sizeof(g_state->lastResultSummary) - 1);
    g_state->lastResultSummary[sizeof(g_state->lastResultSummary) - 1] = '\0';
    g_state->hasGame = false;
    g_state->resultDisplayUntilMs = millis() + RESULT_DISPLAY_DURATION_MS;

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

static void onMessageCallback(WebsocketsMessage message) {
  if (message.isBinary()) {
    const std::string &raw = message.rawData();
    handleFrame((const uint8_t *)raw.data(), raw.length());
  } else {
    // Not expected (chess.com's RSocket connection is binary-only), but
    // if the server sends a text error message instead, show it rather
    // than silently dropping it.
    Serial.print("[LiveGame] Unexpected TEXT frame: ");
    Serial.println(message.data());
  }
}

static void onEventCallback(WebsocketsEvent event, String data) {
  if (event == WebsocketsEvent::ConnectionOpened) {
    Serial.printf("[LiveGame] WebSocket connected (free heap: %u bytes), sending SETUP + REQUEST_STREAM...\n", ESP.getFreeHeap());

    uint8_t buf[128];
    size_t n = encodeSetup(buf, sizeof(buf), RSOCKET_KEEPALIVE_MS, RSOCKET_LIFETIME_MS);
    if (n > 0) sendFrame(buf, n);

    n = encodeRequestStream(buf, sizeof(buf), requestStreamId, currentWatchRoute);
    if (n > 0) sendFrame(buf, n);

    connected = true;
  } else if (event == WebsocketsEvent::ConnectionClosed) {
    Serial.printf("[LiveGame] WebSocket disconnected (free heap: %u bytes)\n", ESP.getFreeHeap());
    connected = false;
  } else if (event == WebsocketsEvent::GotPing) {
    Serial.println("[LiveGame] Got WS ping (library auto-replies with pong).");
  } else if (event == WebsocketsEvent::GotPong) {
    Serial.println("[LiveGame] Got WS pong.");
  }
}

void liveGameConnect(const GameInfo &game) {
  strncpy(currentWatchRoute, game.watchRoute, sizeof(currentWatchRoute) - 1);
  currentWatchRoute[sizeof(currentWatchRoute) - 1] = '\0';
  requestStreamId = 1;
  resetTrackingState();

  // Same header set the validated Python client sends for this exact
  // connection. ArduinoWebsockets also sends its own default Origin/
  // User-Agent if we don't override them, but we set them explicitly to
  // match Python exactly rather than rely on the library's defaults.
  char cookieValue[PHPSESSID_MAX_LEN + 32];
  snprintf(cookieValue, sizeof(cookieValue), "PHPSESSID=%s", phpsessid);
  client.addHeader("Cookie", cookieValue);
  client.addHeader("Origin", "https://www.chess.com");
  client.addHeader("User-Agent",
                    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
                    "(KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36");

  client.onMessage(onMessageCallback);
  client.onEvent(onEventCallback);

  // This library DOES have a real setInsecure() on ESP32 (unlike the
  // previous one) - calling it explicitly rather than relying on
  // whatever the default happens to be for this installed version.
  client.setInsecure();

  char url[220];
  snprintf(url, sizeof(url), "wss://www.chess.com%s", game.rsocketUrl);

  Serial.printf("[LiveGame] Free heap before connecting: %u bytes\n", ESP.getFreeHeap());
  Serial.printf("[LiveGame] Connecting to %s (route=%s)\n", url, currentWatchRoute);

  bool ok = client.connect(url);
  if (!ok) {
    Serial.printf("[LiveGame] client.connect() returned false (failed immediately). WiFi.status()=%d\n", WiFi.status());
  }
}

void liveGameDisconnect() {
  client.close();
  connected = false;
}

bool liveGameIsConnected() {
  return connected;
}

void liveGameLoop(ClockState &state) {
  g_state = &state;
  client.poll();
}
