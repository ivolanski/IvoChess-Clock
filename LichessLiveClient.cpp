#include "LichessLiveClient.h"
#include "config.h"
#include "GameDataSource.h"
#include "AdminPortal.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

static const char *USER_AGENT =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36";

// ---------------------------------------------------------------------------
// Static/file-scope, not local: this connection has to survive across
// MANY lichessLiveLoop() calls (the whole length of a game), unlike
// LichessApiFunctions.cpp's short-lived request/response calls where a
// local WiFiClientSecure+HTTPClient is fine. Same pattern LiveGameClient.cpp
// uses for its WebSocketsClient.
// ---------------------------------------------------------------------------
static WiFiClientSecure secureClient;
static HTTPClient http;
static NetworkClient *rawStream = nullptr;
static bool connected = false;
static ClockState *g_state = nullptr;

static unsigned long lastLineAtMs = 0;
// No confirmed heartbeat cadence from Lichess's docs (unlike chess.com's
// documented RSOCKET_KEEPALIVE_MS) - this is deliberately generous, wide
// enough to tolerate a real state-update gap during a slow-time-control
// game while still catching a truly dead connection. Tighten this once
// real hardware testing shows the actual cadence (Lichess is known to
// send periodic blank keepalive lines - see the blank-line handling
// below - but the exact interval isn't in the OpenAPI spec).
#define LICHESS_STALE_THRESHOLD_MS (60UL * 1000UL)

// ---- HTTP chunked-transfer decoding + NDJSON line accumulation ----
//
// HTTPClient::getStreamPtr() (used below, not getString()) hands back the
// RAW underlying socket with NO dechunking - verified by reading the
// ESP32 core's HTTPClient.cpp: chunk decoding only happens inside
// writeToStream()/writeToStreamDataBlock(), which BLOCKS until each
// chunk fully arrives (unusable here - loop() must stay responsive for
// the e-paper display and webadmin). So chunked-transfer framing
// ("<hex-size>\r\n<data>\r\n", repeated, ending in a zero-size chunk) is
// parsed by hand, byte by byte, across as many loop() ticks as it takes -
// the same class of problem RSocketCodec.cpp already solves for
// chess.com's binary framing (partial data across reads).
enum ChunkParseState {
  CHUNK_SIZE,       // accumulating hex digits of the next chunk's size, up to \n
  CHUNK_DATA,       // passing through exactly chunkRemaining more payload bytes
  CHUNK_DATA_CRLF,  // consuming the \r\n that follows each chunk's data
};
static ChunkParseState chunkState = CHUNK_SIZE;
static char chunkSizeBuf[16];
static size_t chunkSizeBufLen = 0;
static long chunkRemaining = 0;

#define LICHESS_LINE_MAX 768  // generous for a gameFull line (two players' info + clock + variant); gameState lines are much shorter
static char lineBuf[LICHESS_LINE_MAX];
static size_t lineLen = 0;

static void handleJsonLine(const char *json, size_t len);

// Feeds one already-dechunked payload byte into the NDJSON line
// accumulator, dispatching handleJsonLine() on '\n'. A blank line (just
// "\r\n" or "\n") is Lichess's keepalive - counts toward staleness
// tracking but isn't sent to the JSON parser (an empty string is not
// valid JSON and would just log a spurious parse error every time).
static void feedLineByte(char c) {
  if (c == '\n') {
    lastLineAtMs = millis();
    // Trim a trailing \r if the line used \r\n.
    if (lineLen > 0 && lineBuf[lineLen - 1] == '\r') {
      lineLen--;
    }
    if (lineLen > 0) {
      lineBuf[lineLen] = '\0';
      handleJsonLine(lineBuf, lineLen);
    }
    lineLen = 0;
    return;
  }
  if (lineLen < LICHESS_LINE_MAX - 1) {
    lineBuf[lineLen++] = c;
  } else {
    // Line longer than expected (shouldn't happen for this API's actual
    // payloads) - drop it rather than overflow or silently truncate into
    // something that parses as different-but-valid JSON.
    lineLen = 0;
  }
}

// Feeds one raw byte straight off the socket (still chunk-framed) through
// the chunk-decoder state machine, handing dechunked payload bytes to
// feedLineByte().
static void feedChunkByte(char c) {
  switch (chunkState) {
    case CHUNK_SIZE:
      if (c == '\r') {
        return;  // ignore, wait for the \n
      }
      if (c == '\n') {
        chunkSizeBuf[chunkSizeBufLen] = '\0';
        chunkRemaining = strtol(chunkSizeBuf, nullptr, 16);
        chunkSizeBufLen = 0;
        if (chunkRemaining <= 0) {
          // Zero-size chunk = server closed the stream cleanly (game
          // ended and Lichess stopped sending, or the connection is
          // being torn down). Treat exactly like any other disconnect -
          // GameDataSource.cpp's Lichess branch falls back to polling.
          connected = false;
        } else {
          chunkState = CHUNK_DATA;
        }
        return;
      }
      if (chunkSizeBufLen < sizeof(chunkSizeBuf) - 1) {
        chunkSizeBuf[chunkSizeBufLen++] = c;
      }
      return;

    case CHUNK_DATA:
      feedLineByte(c);
      chunkRemaining--;
      if (chunkRemaining <= 0) {
        chunkState = CHUNK_DATA_CRLF;
      }
      return;

    case CHUNK_DATA_CRLF:
      // Exactly 2 bytes ("\r\n") always follow chunk data - consume both
      // then go back to reading the next chunk's size. Not counting them
      // explicitly (just watching for the \n) is enough since nothing
      // else can legally appear here.
      if (c == '\n') {
        chunkState = CHUNK_SIZE;
      }
      return;
  }
}

// ---- game-state tracking (mirrors LiveGameClient.cpp's shape) ----

static long lastClocks[2] = {0, 0};
static int lastMoveCountSeen = -1;

static int countMoves(const char *moves) {
  if (moves == nullptr || moves[0] == '\0') return 0;
  int count = 1;
  for (const char *p = moves; *p; p++) {
    if (*p == ' ') count++;
  }
  return count;
}

// Which of state.players[0] (white)/[1] (black) is "me" - same
// activeMyUsername()-based matching every other data source uses (see
// GameDataSource.cpp), so win/loss and "whose turn" LED colors work
// through the exact same code path regardless of which service is
// active. Deliberately NOT using the "color" field from the discovery
// poll (LichessGameInfo::myColorIsWhite) for this - one resolution
// mechanism project-wide is simpler than two, and this one already
// handles the "not configured" fallback consistently.
static int myPlayerIndex() {
  const char *me = activeMyUsername();
  if (me[0] == '\0') return -1;
  for (int i = 0; i < 2; i++) {
    if (strcasecmp(g_state->players[i].username, me) == 0) return i;
  }
  return -1;
}

static void applyClockUpdate(long wtimeMs, long btimeMs, int moveCount) {
  long newClocks[2] = {wtimeMs, btimeMs};

  if (lastMoveCountSeen != -1 && moveCount != lastMoveCountSeen) {
    // New move: alternate the guess immediately, same reasoning as
    // LiveGameClient.cpp's updateActiveIndex() - a fresh move count
    // change means the turn just flipped, before this event's own clock
    // numbers can be trusted to say who's "active" now.
    if (g_state->activePlayerIndex != -1) {
      g_state->activePlayerIndex = 1 - g_state->activePlayerIndex;
    } else {
      g_state->activePlayerIndex = (moveCount % 2 == 0) ? 0 : 1;  // white moves on even move-count, black on odd
    }
  }
  lastClocks[0] = newClocks[0];
  lastClocks[1] = newClocks[1];
  lastMoveCountSeen = moveCount;

  // Both real clock values, every time - not just clockBaselineMs[].
  // DisplayFunctions.cpp draws from players[i].clockMs directly (see
  // drawPlayerHalf()), and IvoChess_Clock.ino's between-events local
  // extrapolation tick only ever touches the CURRENTLY ACTIVE player's
  // clockMs (it's a single-clock ticking-down fallback, not a general
  // resync) - so the inactive side's clockMs never gets refreshed unless
  // something sets it directly on every real update, same as chess.com's
  // LiveGameClient.cpp already does. Missing this line is exactly why
  // the opponent's clock showed as stuck at zero: clockBaselineMs[] was
  // being updated correctly, but nothing ever copied it into the field
  // the display actually reads.
  g_state->players[0].clockMs = newClocks[0];
  g_state->players[1].clockMs = newClocks[1];

  g_state->clockBaselineMs[0] = newClocks[0];
  g_state->clockBaselineMs[1] = newClocks[1];
  g_state->clockBaselineAtMs = millis();
  g_state->moveCount = moveCount;
}

static void applyGameOver(const char *status, const char *winnerColor) {
  bool over = !(strcmp(status, "started") == 0 || strcmp(status, "created") == 0);
  if (!over) return;

  int meIdx = myPlayerIndex();
  GameOutcome outcome = OUTCOME_NONE;

  if (winnerColor == nullptr || winnerColor[0] == '\0') {
    outcome = OUTCOME_DRAW;
  } else {
    int winnerIdx = (strcmp(winnerColor, "white") == 0) ? 0 : 1;
    if (meIdx != -1) {
      outcome = (winnerIdx == meIdx) ? OUTCOME_WIN : OUTCOME_LOSS;
    }
    // else: activeMyUsername() isn't configured/matched - stays
    // OUTCOME_NONE, same fallback chess.com's LiveGameClient.cpp uses.
  }
  g_state->lastGameOutcome = outcome;
  g_state->lastResultReason[0] = '\0';  // Lichess's board API doesn't give a separate per-player reason string like chess.com's does
  snprintf(g_state->lastResultSummary, sizeof(g_state->lastResultSummary), "%s", status);

  if (meIdx != -1) {
    int oppIdx = 1 - meIdx;
    strncpy(g_state->lastOpponentUsername, g_state->players[oppIdx].username, USERNAME_MAX_LEN - 1);
    g_state->lastOpponentUsername[USERNAME_MAX_LEN - 1] = '\0';
    g_state->lastMyRating = g_state->players[meIdx].rating;
    g_state->lastOpponentRating = g_state->players[oppIdx].rating;
    g_state->lastPlayerRatingsKnown = true;
  } else {
    g_state->lastOpponentUsername[0] = '\0';
    g_state->lastPlayerRatingsKnown = false;
  }
  g_state->lastRatingKnown = false;  // Lichess's board stream doesn't carry a post-game rating delta

  g_state->resultDisplayUntilMs = millis() + resultDisplayDurationMs;
  g_state->hasGame = false;
  connected = false;  // stream will close itself shortly (the zero-size chunk above) - no need to wait for it
}

static void handleJsonLine(const char *json, size_t len) {
  DynamicJsonDocument doc(LICHESS_LINE_MAX + 256);
  DeserializationError err = deserializeJson(doc, json, len);
  if (err) {
    Serial.print("[LichessLive] JSON parse error: ");
    Serial.println(err.c_str());
    return;
  }

  const char *type = doc["type"] | "";

  if (strcmp(type, "gameFull") == 0) {
    JsonObject white = doc["white"];
    JsonObject black = doc["black"];
    const char *whiteName = white["name"] | "?";
    const char *blackName = black["name"] | "?";
    strncpy(g_state->players[0].username, whiteName, USERNAME_MAX_LEN - 1);
    g_state->players[0].username[USERNAME_MAX_LEN - 1] = '\0';
    g_state->players[0].rating = white["rating"] | 0;
    strncpy(g_state->players[1].username, blackName, USERNAME_MAX_LEN - 1);
    g_state->players[1].username[USERNAME_MAX_LEN - 1] = '\0';
    g_state->players[1].rating = black["rating"] | 0;

    JsonObject state = doc["state"];
    long wtime = state["wtime"] | 0L;
    long btime = state["btime"] | 0L;
    int moveCount = countMoves(state["moves"] | "");
    lastMoveCountSeen = -1;  // force applyClockUpdate() to compute a fresh activePlayerIndex from parity, not a stale flip
    applyClockUpdate(wtime, btime, moveCount);

    const char *status = state["status"] | "started";
    const char *winner = state["winner"] | "";
    applyGameOver(status, winner);
    return;
  }

  if (strcmp(type, "gameState") == 0) {
    long wtime = doc["wtime"] | 0L;
    long btime = doc["btime"] | 0L;
    int moveCount = countMoves(doc["moves"] | "");
    applyClockUpdate(wtime, btime, moveCount);

    const char *status = doc["status"] | "started";
    const char *winner = doc["winner"] | "";
    applyGameOver(status, winner);
    return;
  }

  // Other event types (chatLine, opponentGone, etc.) - not needed for
  // clocks/moves/result, ignored on purpose.
}

void lichessLiveConnect(const LichessGameInfo &game, const char *token) {
  secureClient.setInsecure();  // TODO: swap for a real Root CA once this flow is validated - matches the existing chess.com TODO (ChessApiFunctions.cpp)

  char url[96];
  snprintf(url, sizeof(url), LICHESS_GAME_STREAM_URL_FMT, game.id);

  if (!http.begin(secureClient, url)) {
    Serial.println("[LichessLive] http.begin() failed.");
    connected = false;
    return;
  }
  String auth = "Bearer " + String(token);
  http.addHeader("Authorization", auth);
  http.addHeader("Accept", "application/x-ndjson");
  http.addHeader("User-Agent", USER_AGENT);
  http.setTimeout(10000);
  http.setReuse(false);

  int httpCode = http.GET();
  if (httpCode != 200) {
    Serial.printf("[LichessLive] Stream request failed: HTTP %d\n", httpCode);
    http.end();
    connected = false;
    return;
  }

  rawStream = http.getStreamPtr();
  chunkState = CHUNK_SIZE;
  chunkSizeBufLen = 0;
  chunkRemaining = 0;
  lineLen = 0;
  lastMoveCountSeen = -1;
  lastLineAtMs = millis();
  connected = (rawStream != nullptr);
  Serial.printf("[LichessLive] Streaming game %s (%s).\n", game.id, connected ? "connected" : "failed to get stream");
}

void lichessLiveDisconnect() {
  if (connected || rawStream != nullptr) {
    http.end();
  }
  rawStream = nullptr;
  connected = false;
}

bool lichessLiveIsConnected() {
  return connected;
}

bool lichessLiveIsStale() {
  return connected && (millis() - lastLineAtMs > LICHESS_STALE_THRESHOLD_MS);
}

void lichessLiveLoop(ClockState &state) {
  g_state = &state;
  if (!connected || rawStream == nullptr) {
    return;
  }

  // Bounded per tick (not "drain everything available") so a burst of
  // buffered data can't hog loop() and delay the e-paper/webadmin - the
  // remainder is simply picked up on the next tick(s), same tradeoff
  // chess.com's WebSocket pump already accepts implicitly via its own
  // library's internal buffering.
  const int MAX_BYTES_PER_TICK = 2048;
  int budget = MAX_BYTES_PER_TICK;
  while (budget-- > 0 && rawStream->available() > 0) {
    int c = rawStream->read();
    if (c < 0) break;
    feedChunkByte((char)c);
    if (!connected) break;  // the zero-size chunk handler above may have ended the stream mid-loop
  }

  if (!connected) {
    http.end();
    rawStream = nullptr;
  }
}
