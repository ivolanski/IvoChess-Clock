// DGT3000 BLE Gateway peripheral - see ChessConnectBLE.h for the
// threading design (BLE task hands off to the main task via a queue).
// Protocol behavior here is deliberately grounded in real captures
// (tests/chessconnect_ble_poc/), not just the written spec - several
// details below exist specifically BECAUSE real traffic disagreed with
// project_details/dgt3000-gateway-protocol.md's own examples.
#include "ChessConnectBLE.h"
#include "AdminPortal.h"  // resultDisplayDurationMs
#include "SoundFunctions.h"

#include <ArduinoJson.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <string.h>

#define DEVICE_NAME "DGT3000-Gateway"
#define SERVICE_UUID               "73822f6e-edcd-44bb-974b-93ee97cb0000"
#define CHAR_PROTOCOL_VERSION_UUID "73822f6e-edcd-44bb-974b-93ee97cb0001"
#define CHAR_COMMAND_UUID          "73822f6e-edcd-44bb-974b-93ee97cb0002"
#define CHAR_EVENT_UUID            "73822f6e-edcd-44bb-974b-93ee97cb0003"
#define CHAR_STATUS_UUID           "73822f6e-edcd-44bb-974b-93ee97cb0004"

// How long the opponent's move text stays in the bottom bar before
// reverting to the move count - first real use of Chessconnect's
// displayText data (chess.com/Lichess never sent anything like it).
#define CHESSCONNECT_MOVE_TEXT_DISPLAY_MS (4UL * 1000UL)

// ---------------------------------------------------------------------------
// Cross-task hand-off (BLE task -> main task) - see the header comment.
// ---------------------------------------------------------------------------
enum ChessConnectEventType {
  CC_EVENT_CONNECTED,
  CC_EVENT_DISCONNECTED,
  CC_EVENT_SET_TIME,
  CC_EVENT_DISPLAY_TEXT,
  CC_EVENT_END_DISPLAY,
};

#define CC_TEXT_MAX_LEN 20

struct ChessConnectEvent {
  ChessConnectEventType type;
  bool leftRunning;
  long leftMs;
  bool rightRunning;
  long rightMs;
  char text[CC_TEXT_MAX_LEN];
};

#define CC_EVENT_QUEUE_LEN 16
static QueueHandle_t eventQueue = nullptr;

static BLECharacteristic *eventChar = nullptr;
static volatile bool bleConnected = false;

// ---------------------------------------------------------------------------
// Command channel (BLE task): fixed-size accumulator, no Arduino String -
// this device runs for hours/days unattended, unlike the throwaway POC
// this was validated against first, so heap fragmentation from repeated
// String concatenation actually matters here.
// ---------------------------------------------------------------------------
#define CC_CMD_BUFFER_SIZE 512
static char cmdBuffer[CC_CMD_BUFFER_SIZE];
static size_t cmdBufferLen = 0;

// Pulls one complete, balanced {...} object off the front of cmdBuffer
// into outBuf (brace-counted, string-aware, same algorithm validated in
// the POC against real chunked writes). Shifts any remaining bytes down
// regardless of whether outBuf was big enough, so a too-large command
// can't wedge the accumulator - see the overflow guard below for why
// that shouldn't happen anyway (every real command observed is well
// under 200 bytes).
static bool extractCompleteJsonObject(char *outBuf, size_t outBufSize) {
  int depth = 0;
  bool inString = false;
  bool escaped = false;
  int start = -1;
  bool found = false;

  for (size_t i = 0; i < cmdBufferLen; i++) {
    char c = cmdBuffer[i];
    if (start == -1) {
      if (c == '{') {
        start = (int)i;
        depth = 1;
        inString = false;
        escaped = false;
      }
      continue;
    }
    if (inString) {
      if (escaped) escaped = false;
      else if (c == '\\') escaped = true;
      else if (c == '"') inString = false;
      continue;
    }
    if (c == '"') {
      inString = true;
    } else if (c == '{') {
      depth++;
    } else if (c == '}') {
      depth--;
      if (depth == 0) {
        size_t objLen = i - (size_t)start + 1;
        if (objLen < outBufSize) {
          memcpy(outBuf, cmdBuffer + start, objLen);
          outBuf[objLen] = '\0';
          found = true;
        } else {
          Serial.println("[ChessConnectBLE] Command too large for buffer - dropping.");
        }
        size_t remaining = cmdBufferLen - (i + 1);
        memmove(cmdBuffer, cmdBuffer + i + 1, remaining);
        cmdBufferLen = remaining;
        return found;
      }
    }
  }
  return false;  // no complete object buffered yet - wait for more bytes
}

static void sendAck(const char *id) {
  if (!eventChar) return;
  // Manual snprintf rather than ArduinoJson here - id is always a short
  // digit-only string per the protocol, so no escaping is needed, and
  // this is the one response that MUST go out promptly (see the header
  // comment - a missed ack stalls Chessconnect's whole command queue,
  // no timeout, no retry).
  char out[64];
  snprintf(out, sizeof(out), "{\"type\":\"response\",\"id\":\"%s\",\"status\":\"ok\"}", id);
  eventChar->setValue((uint8_t *)out, strlen(out));
  eventChar->notify();
}

static void handleCompleteCommand(const char *json) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, json);
  if (err) {
    // Matches the spec: malformed JSON is logged and dropped, connection
    // survives - and there's no id to ack even if we wanted to.
    Serial.printf("[ChessConnectBLE] JSON parse error: %s\n", err.c_str());
    return;
  }

  const char *command = doc["command"] | "";
  const char *id = doc["id"] | "";
  sendAck(id);  // unconditional, regardless of whether we recognize the command

  ChessConnectEvent event = {};
  bool haveEvent = true;

  if (strcmp(command, "setTime") == 0) {
    JsonObject params = doc["params"];
    event.type = CC_EVENT_SET_TIME;
    event.leftRunning = (params["leftMode"] | 0) == 1;
    event.leftMs = ((long)(params["leftHours"] | 0) * 3600L
                     + (long)(params["leftMinutes"] | 0) * 60L
                     + (long)(params["leftSeconds"] | 0)) * 1000L;
    event.rightRunning = (params["rightMode"] | 0) == 1;
    event.rightMs = ((long)(params["rightHours"] | 0) * 3600L
                      + (long)(params["rightMinutes"] | 0) * 60L
                      + (long)(params["rightSeconds"] | 0)) * 1000L;
  } else if (strcmp(command, "displayText") == 0) {
    const char *text = doc["params"]["text"] | "";
    event.type = CC_EVENT_DISPLAY_TEXT;
    strncpy(event.text, text, CC_TEXT_MAX_LEN - 1);
    event.text[CC_TEXT_MAX_LEN - 1] = '\0';
  } else if (strcmp(command, "endDisplay") == 0) {
    event.type = CC_EVENT_END_DISPLAY;
  } else {
    haveEvent = false;  // unknown command - already acked, nothing to apply
  }

  if (haveEvent && eventQueue) {
    if (xQueueSend(eventQueue, &event, 0) != pdTRUE) {
      Serial.println("[ChessConnectBLE] Event queue full - dropping (shouldn't happen at this traffic rate).");
    }
  }
}

class CommandCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *chr) override {
    // this esp32 core's BLECharacteristic::getValue() returns String, not
    // std::string - confirmed the hard way (compile error) building
    // tests/chessconnect_ble_poc/. One short-lived local String here is
    // fine; everything after this line works off the fixed cmdBuffer.
    String value = chr->getValue();
    size_t len = value.length();
    if (len == 0) return;

    if (cmdBufferLen + len >= CC_CMD_BUFFER_SIZE) {
      // Matches the doc's own guidance: reset the accumulator on
      // overflow rather than risk a stuck/garbled state. No real command
      // observed is anywhere close to this size.
      Serial.println("[ChessConnectBLE] Command buffer overflow - resetting accumulator.");
      cmdBufferLen = 0;
    }
    size_t toCopy = len;
    if (cmdBufferLen + toCopy >= CC_CMD_BUFFER_SIZE) {
      toCopy = CC_CMD_BUFFER_SIZE - 1 - cmdBufferLen;  // pathological case - truncate defensively rather than overflow
    }
    memcpy(cmdBuffer + cmdBufferLen, value.c_str(), toCopy);
    cmdBufferLen += toCopy;

    char objBuf[CC_CMD_BUFFER_SIZE];
    while (extractCompleteJsonObject(objBuf, sizeof(objBuf))) {
      handleCompleteCommand(objBuf);
    }
  }
};

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *server) override {
    Serial.println("[ChessConnectBLE] Central connected.");
    bleConnected = true;
    ChessConnectEvent event = {};
    event.type = CC_EVENT_CONNECTED;
    if (eventQueue) xQueueSend(eventQueue, &event, 0);
  }
  void onDisconnect(BLEServer *server) override {
    Serial.println("[ChessConnectBLE] Central disconnected - resuming advertising.");
    bleConnected = false;
    cmdBufferLen = 0;
    ChessConnectEvent event = {};
    event.type = CC_EVENT_DISCONNECTED;
    if (eventQueue) xQueueSend(eventQueue, &event, 0);
    BLEDevice::startAdvertising();  // doc requirement: must accept a fresh connection without user interaction
  }
};

bool chessConnectBleIsConnected() {
  return bleConnected;
}

void chessConnectSendButtonEvent() {
  if (!bleConnected || !eventChar) return;
  static const char *payload = "{\"type\":\"buttonEvent\",\"data\":{\"isRepeat\":false}}";
  eventChar->setValue((uint8_t *)payload, strlen(payload));
  eventChar->notify();

  // This button press IS the "I made my move" action for ChessConnect -
  // unlike chess.com/Lichess (IvoChess_Clock.ino's activePlayerIndex-diff
  // path), there's no side-switch or other signal to detect an own move
  // from afterwards: displayText only ever reports the OPPONENT'S move
  // (see applyEvent() below), so this is the only reliable place to play
  // SOUND_MOVE_OWN for this data source.
  playSoundEvent(SOUND_MOVE_OWN);
}

void initChessConnectBLE() {
  eventQueue = xQueueCreate(CC_EVENT_QUEUE_LEN, sizeof(ChessConnectEvent));

  BLEDevice::init(DEVICE_NAME);

  BLEServer *server = BLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());

  BLEService *service = server->createService(SERVICE_UUID);

  BLECharacteristic *versionChar = service->createCharacteristic(
      CHAR_PROTOCOL_VERSION_UUID, BLECharacteristic::PROPERTY_READ);
  versionChar->setValue("1.0");

  BLECharacteristic *commandChar = service->createCharacteristic(
      CHAR_COMMAND_UUID,
      BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  commandChar->setCallbacks(new CommandCallbacks());

  eventChar = service->createCharacteristic(CHAR_EVENT_UUID, BLECharacteristic::PROPERTY_NOTIFY);
  eventChar->addDescriptor(new BLE2902());  // CCCD - required for Notify to actually work

  // Must exist even though nothing reads/subscribes to it - Android
  // rejects the connection outright if this characteristic is missing.
  BLECharacteristic *statusChar = service->createCharacteristic(
      CHAR_STATUS_UUID, BLECharacteristic::PROPERTY_READ);
  statusChar->setValue("unused");

  service->start();

  BLEAdvertising *advertising = BLEDevice::getAdvertising();
  BLEAdvertisementData advData;
  advData.setName(DEVICE_NAME);  // complete local name in the advertising packet...
  advertising->setAdvertisementData(advData);
  BLEAdvertisementData scanResponseData;
  scanResponseData.setCompleteServices(BLEUUID(SERVICE_UUID));  // ...128-bit service UUID in the scan response - the doc's recommended layout, both together exceed the 31-byte advertising payload
  advertising->setScanResponseData(scanResponseData);
  advertising->setScanResponse(true);
  advertising->setMinPreferred(0x06);
  advertising->setMinPreferred(0x12);
  BLEDevice::startAdvertising();

  Serial.println("[ChessConnectBLE] Advertising as \"DGT3000-Gateway\".");
}

// ---------------------------------------------------------------------------
// Main-task side: applying drained events to ClockState.
// ---------------------------------------------------------------------------

enum ResultText { RESULT_NONE, RESULT_WHITE_WINS, RESULT_BLACK_WINS, RESULT_DRAW };

static ResultText classifyResultText(const char *text) {
  if (strcmp(text, "1-0") == 0) return RESULT_WHITE_WINS;
  if (strcmp(text, "0-1") == 0) return RESULT_BLACK_WINS;
  if (strcmp(text, "1/2-1/2") == 0) return RESULT_DRAW;
  // Deliberately NOT "0-0": real captures confirmed this exact string is
  // also how kingside castling arrives mid-game (project_details/
  // dgt3000-gateway-protocol.md defines it as "draw or aborted", but
  // every "0-0" actually observed in testing was a castle, never a real
  // draw signal). Treating it as game-over here would wrongly end a game
  // the moment a player castles.
  return RESULT_NONE;
}

// Whichever side ticks first in a fresh game's very first setTime is
// White, unconditionally - not a guess, it's the rule that White always
// moves first (confirmed across 5 separate real games during testing,
// including one deliberately played as Black to rule out left/right just
// meaning White always). -1 = not yet determined for the current game.
//
// This is the ONLY identity signal this protocol ever gives us - it
// never discloses which side is "the local player" (see the protocol
// doc: "the device never needs to know who is White"), so players[] are
// always labeled literally "White"/"Black", never "You"/"Opponent".
static int whiteIndex = -1;

// BUG FOUND DURING REAL TESTING (round 1): re-deriving whiteIndex only
// when it was -1 (i.e. only right after a CLEANLY DETECTED result) left
// it stuck on a stale value whenever a game ended without one reaching
// us. Tried fixing that by ALSO re-deriving on a large upward clock
// jump ("must be a new game") - that was WORSE: a normal Fischer
// increment/delay bonus can legitimately be several seconds, so it
// false-triggered mid-game, flipping which index is "White" while a
// single game was still in progress (reproduced live: the on-screen
// left/right order kept swapping mid-game, not just between games).
// Reverted - a fixed threshold can never safely distinguish "new game"
// from "this time control's increment is just large" from the clock
// value alone, so it's not a signal worth trusting for this.
//
// Back to re-deriving only on hasGame's false->true transition (plus the
// whiteIndex==-1 fallback for the very first game since boot). This
// accepts a narrower, rarer residual risk (a game that ends without
// classifyResultText() ever recognizing a result string) over a
// heuristic that was actively wrong during ordinary play.
static void applyEvent(const ChessConnectEvent &event, ClockState &state) {
  switch (event.type) {
    case CC_EVENT_CONNECTED:
      break;

    case CC_EVENT_DISCONNECTED:
      // Doesn't end the game by itself - the doc's own reconnect
      // behavior expects Chessconnect back within a few seconds. A brief
      // BLE blip shouldn't flash the display to idle; clocks/turn stay
      // exactly as last known until either traffic resumes or the game
      // genuinely ends with a result.
      snprintf(state.apiStatus, sizeof(state.apiStatus), "ChessConnect disconnected");
      state.apiOk = false;
      break;

    case CC_EVENT_SET_TIME: {
      bool wasNewGame = !state.hasGame;

      if (wasNewGame || whiteIndex == -1) {
        whiteIndex = event.leftRunning ? 0 : (event.rightRunning ? 1 : 0);
      }
      int blackIndex = 1 - whiteIndex;
      // setTime's left/right map directly to players[0]/players[1] -
      // matches the existing meIndex==-1 fallback already in
      // DisplayFunctions.cpp/LedFunctions.cpp (activeMyUsername() never
      // resolves for this source - see below), which already puts
      // players[0] on screen-left, players[1] on screen-right.

      state.hasGame = true;
      state.apiOk = true;
      snprintf(state.apiStatus, sizeof(state.apiStatus), "OK");

      strncpy(state.players[whiteIndex].username, "White", USERNAME_MAX_LEN - 1);
      state.players[whiteIndex].username[USERNAME_MAX_LEN - 1] = '\0';
      state.players[whiteIndex].rating = RATING_UNKNOWN;
      strncpy(state.players[blackIndex].username, "Black", USERNAME_MAX_LEN - 1);
      state.players[blackIndex].username[USERNAME_MAX_LEN - 1] = '\0';
      state.players[blackIndex].rating = RATING_UNKNOWN;

      state.players[0].clockMs = event.leftMs;
      state.players[1].clockMs = event.rightMs;
      state.clockBaselineMs[0] = event.leftMs;
      state.clockBaselineMs[1] = event.rightMs;
      state.clockBaselineAtMs = millis();

      if (event.leftRunning) state.activePlayerIndex = 0;
      else if (event.rightRunning) state.activePlayerIndex = 1;
      else state.activePlayerIndex = -1;

      // Never sent by this protocol - see DisplayFunctions.cpp's
      // MOVE_COUNT_UNKNOWN handling for how this renders.
      state.moveCount = MOVE_COUNT_UNKNOWN;

      if (wasNewGame) {
        state.resultDisplayUntilMs = 0;
        state.lastResultSummary[0] = '\0';
        state.lastGameOutcome = OUTCOME_NONE;
        state.chessConnectMoveTextUntilMs = 0;
        state.newGameStarted = true;  // pulse - see ClockState.h
      }
      break;
    }

    case CC_EVENT_END_DISPLAY:
      // Confirmed via real capture: does NOT mean game over, and fires
      // routinely mid-game whenever Chessconnect has nothing new to
      // push. Deliberately a no-op - leaving clocks/turn exactly as last
      // known avoids flashing the display on every one of these.
      break;

    case CC_EVENT_DISPLAY_TEXT: {
      ResultText result = classifyResultText(event.text);
      if (result != RESULT_NONE) {
        const char *summary = (result == RESULT_WHITE_WINS) ? "White wins"
                             : (result == RESULT_BLACK_WINS) ? "Black wins"
                             : "Draw";
        // Confirmed via real capture: the same result text can arrive
        // 2-3 times in a row within ~150ms. Applying it again is
        // harmless (idempotent fields) but skip the redundant work/log
        // once the game has already ended with this exact result.
        if (state.hasGame || strcmp(state.lastResultSummary, summary) != 0) {
          strncpy(state.lastResultSummary, summary, sizeof(state.lastResultSummary) - 1);
          state.lastResultSummary[sizeof(state.lastResultSummary) - 1] = '\0';
          // Can't resolve win/loss-for-you: this protocol never
          // discloses which side is the local player, only White/Black
          // - see whiteIndex's comment. OUTCOME_NONE routes through the
          // existing neutral fallback in drawResultContent()/
          // updateLEDs() (built originally for "myUsername not
          // configured" on chess.com/Lichess) rather than asserting a
          // win/loss we don't actually know.
          state.lastGameOutcome = OUTCOME_NONE;
          state.hasGame = false;
          state.resultDisplayUntilMs = millis() + resultDisplayDurationMs;
          whiteIndex = -1;  // re-detect for the next game
          Serial.printf("[ChessConnectBLE] Game finished: %s\n", summary);
        }
      } else {
        // A move (or castling "0-0"/"0-0-0") - first real UI use of this
        // data; chess.com/Lichess never sent move text at all. Shown
        // briefly in the existing bottom-bar slot (see
        // DisplayFunctions.cpp) instead of a whole new screen.
        strncpy(state.chessConnectMoveText, event.text, sizeof(state.chessConnectMoveText) - 1);
        state.chessConnectMoveText[sizeof(state.chessConnectMoveText) - 1] = '\0';
        state.chessConnectMoveTextUntilMs = millis() + CHESSCONNECT_MOVE_TEXT_DISPLAY_MS;

        // Real-hardware feedback (playing with a physical Chessnut board):
        // the board bridge withholds the official setTime side-switch
        // until the move is physically replayed on the board - the doc
        // itself confirms this ("no setTime is sent [while a text is
        // displayed]... cleared... once the player has replayed the
        // opponent's move on the physical board"), even though the real
        // game (chess.com/Lichess itself) already switched whose clock is
        // running the instant the move landed server-side. displayText
        // arriving IS that earlier signal - flip the active side and
        // restart local extrapolation from it now, rather than waiting
        // for the board to catch up. Purely a prediction: the next real
        // setTime (whenever the board bridge actually sends it) always
        // overwrites clockMs/activePlayerIndex/baselines wholesale, so a
        // wrong guess here self-corrects within about a second and never
        // compounds.
        //
        // Guarded against the same repeated-send pattern confirmed for
        // result text (identical displayText resent within ~150ms) -
        // without this, a duplicate would flip the side AGAIN, undoing
        // the correct prediction the first copy just made.
        static char lastMoveTextSeen[CC_TEXT_MAX_LEN] = "";
        bool isNewMoveText = strcmp(event.text, lastMoveTextSeen) != 0;

        if (isNewMoveText && (state.activePlayerIndex == 0 || state.activePlayerIndex == 1)) {
          int newActive = 1 - state.activePlayerIndex;
          state.activePlayerIndex = newActive;
          state.clockBaselineMs[newActive] = state.players[newActive].clockMs;
          state.clockBaselineAtMs = millis();
        }

        // Per the protocol doc, displayText that isn't a result IS always
        // the opponent's move - this is the only source with move
        // notation at all (chess.com/Lichess never send SAN, only a move
        // count - see GameDataSource.cpp's research notes), so it's also
        // the only source where "check" is cheaply detectable: SAN move
        // text ends in '+' for check ('#' for checkmate, deliberately not
        // handled here - that's already covered a moment later by the
        // game_end sound once the result text arrives). Gated on
        // isNewMoveText for the same reason the side-flip above is - the
        // same text can arrive 2-3 times within ~150ms.
        if (isNewMoveText) {
          playSoundEvent(SOUND_MOVE_OPPONENT);
          size_t textLen = strlen(event.text);
          if (textLen > 0 && event.text[textLen - 1] == '+') {
            playSoundEvent(SOUND_CHECK);
          }
        }

        strncpy(lastMoveTextSeen, event.text, sizeof(lastMoveTextSeen) - 1);
        lastMoveTextSeen[sizeof(lastMoveTextSeen) - 1] = '\0';
      }
      break;
    }
  }
}

void chessConnectBleLoop(ClockState &state) {
  if (!eventQueue) return;
  ChessConnectEvent event;
  while (xQueueReceive(eventQueue, &event, 0) == pdTRUE) {
    applyEvent(event, state);
  }
}
