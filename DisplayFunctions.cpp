#include "DisplayFunctions.h"
#include "config.h"
#include "Translations.h"
#include "AdminPortal.h"

#include <GxEPD2_BW.h>
#include <Fonts/FreeMonoBold9pt7b.h>
#include <Fonts/FreeMonoBold12pt7b.h>
#include <Fonts/FreeMonoBold18pt7b.h>

GxEPD2_BW<GxEPD2_213_B74, GxEPD2_213_B74::HEIGHT> display(
    GxEPD2_213_B74(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

void initDisplay() {
  display.init(115200, true, 2, false);
  display.setRotation(DISPLAY_ROTATION);
  display.setTextWrap(false);  // avoids any text near the edge wrapping on its own
  Serial.println("[Display] Initialized (GxEPD2_213_B74).");
}

static void drawLogoContent() {
  display.setFont(&FreeMonoBold12pt7b);
  const char *text = T(STR_APP_NAME);
  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((SCREEN_WIDTH - w) / 2, (SCREEN_HEIGHT + h) / 2 - 5);
  display.print(text);
}

void drawStartupScreen() {
  display.setFullWindow();
  display.fillScreen(GxEPD_WHITE);
  display.setTextColor(GxEPD_BLACK);
  drawLogoContent();
  display.display(false);
}

// Right-aligned text, ending 'marginRight' px before the right edge -
// uses getTextBounds so nothing risks getting clipped/disappearing.
static void printRightAligned(const char *text, int y, int marginRight = 5) {
  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
  display.setCursor(SCREEN_WIDTH - w - marginRight, y);
  display.print(text);
}

// Horizontally centered text at height y.
static void printCentered(const char *text, int y) {
  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((SCREEN_WIDTH - w) / 2, y);
  display.print(text);
}

// Like printCentered(), but wraps to a second centered line (at y +
// lineHeight) if the text doesn't fit maxWidth on one line - splits at
// the space closest to the middle of the string, so both halves come
// out reasonably balanced instead of one long line + one short one.
// Only wraps once (2 lines total) - plenty for the short status phrases
// this is used for; not a general paragraph-wrapping engine.
static void printCenteredWrapped(const char *text, int y, int lineHeight, int maxWidth) {
  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
  if (w <= (uint16_t)maxWidth) {
    printCentered(text, y);
    return;
  }

  int len = strlen(text);
  int mid = len / 2;
  int splitAt = -1;
  for (int offset = 0; offset <= mid; offset++) {
    if (mid - offset >= 0 && text[mid - offset] == ' ') {
      splitAt = mid - offset;
      break;
    }
    if (mid + offset < len && text[mid + offset] == ' ') {
      splitAt = mid + offset;
      break;
    }
  }

  if (splitAt < 0) {
    // No space to break at (single long word) - print as-is rather than
    // fabricate a mid-word break.
    printCentered(text, y);
    return;
  }

  char line1[48], line2[48];
  int n1 = splitAt < (int)sizeof(line1) - 1 ? splitAt : (int)sizeof(line1) - 1;
  strncpy(line1, text, n1);
  line1[n1] = '\0';
  strncpy(line2, text + splitAt + 1, sizeof(line2) - 1);  // +1 skips the space itself
  line2[sizeof(line2) - 1] = '\0';

  printCentered(line1, y);
  printCentered(line2, y + lineHeight);
}

static void drawBatteryLabel(int percentage) {
  char batBuf[16];
  snprintf(batBuf, sizeof(batBuf), "Batt: %d%%", percentage);
  display.setFont(&FreeMonoBold9pt7b);
  printRightAligned(batBuf, 12);
}

// ---- top status bar: WiFi name+signal (left), battery bars (right) ----
// Used by the waiting/status screen only (the game screen has its own
// compact "Batt: N%" label - drawBatteryLabel above - since it needs the
// space for player names/clocks instead).

// 0-4 bars, ~25% each, ceiling-rounded so any nonzero charge shows at
// least 1 bar instead of looking dead-empty.
static int batteryBarCount(int percentage) {
  if (percentage <= 0) return 0;
  int bars = (percentage + 24) / 25;
  return (bars > 4) ? 4 : bars;
}

// 0-4 bars from RSSI - finer-grained than wifiQualityLabel()'s 3-tier
// Poor/Good/Excellent, purely for this icon.
static int wifiBarCount(int rssi) {
  if (rssi >= -55) return 4;
  if (rssi >= -65) return 3;
  if (rssi >= -75) return 2;
  if (rssi >= -85) return 1;
  return 0;
}

// Ascending-height bars (like a phone signal icon) - x is the left edge.
// Returns the total width drawn, so callers can lay out what follows.
static int drawSignalBars(int x, int baselineY, int filled, int total) {
  const int barW = 3, gap = 1, maxH = 8;
  for (int i = 0; i < total; i++) {
    int h = maxH * (i + 1) / total;
    int bx = x + i * (barW + gap);
    int by = baselineY - h;
    if (i < filled) {
      display.fillRect(bx, by, barW, h, GxEPD_BLACK);
    } else {
      display.drawRect(bx, by, barW, h, GxEPD_BLACK);
    }
  }
  return total * barW + (total - 1) * gap;
}

// Equal-height gauge bars (battery) - x is the left edge of the group.
static int drawGaugeBars(int x, int baselineY, int filled, int total) {
  const int barW = 4, gap = 1, h = 7;
  int by = baselineY - h;
  for (int i = 0; i < total; i++) {
    int bx = x + i * (barW + gap);
    if (i < filled) {
      display.fillRect(bx, by, barW, h, GxEPD_BLACK);
    } else {
      display.drawRect(bx, by, barW, h, GxEPD_BLACK);
    }
  }
  return total * barW + (total - 1) * gap;
}

#define STATUS_BAR_BASELINE 11
#define STATUS_BAR_MARGIN 4

static void drawTopStatusBar(const ClockState &state) {
  display.setFont(&FreeMonoBold9pt7b);

  // Left: WiFi. Disconnected shows as plain text (no point in signal
  // bars for a network we're not even on) - connected shows a short
  // SSID plus signal bars right after it.
  display.setCursor(STATUS_BAR_MARGIN, STATUS_BAR_BASELINE);
  if (!state.wifiConnected) {
    char buf[24];
    snprintf(buf, sizeof(buf), "%s %s", T(STR_WIFI), T(STR_DISCONNECTED));
    display.print(buf);
  } else {
    char ssidShort[11];
    strncpy(ssidShort, state.wifiSSID, 10);
    ssidShort[10] = '\0';
    display.print(ssidShort);

    int16_t x1, y1;
    uint16_t w, h;
    display.getTextBounds(ssidShort, 0, 0, &x1, &y1, &w, &h);
    int barsX = STATUS_BAR_MARGIN + w + 6;
    drawSignalBars(barsX, STATUS_BAR_BASELINE, wifiBarCount(state.wifiStrength), 4);
  }

  // Right: battery, as bars only (no percentage number - config asked
  // for an at-a-glance gauge here, the exact number is still on the game
  // screen via drawBatteryLabel).
  const int battTotalW = 4 * 4 + 3 * 1;  // matches drawGaugeBars(total=4) geometry
  int battX = SCREEN_WIDTH - STATUS_BAR_MARGIN - battTotalW;
  drawGaugeBars(battX, STATUS_BAR_BASELINE, batteryBarCount(state.batteryPercentage), 4);

  display.drawFastHLine(0, STATUS_BAR_BASELINE + 5, SCREEN_WIDTH, GxEPD_BLACK);
}

// Short, human, ALWAYS-fits-on-one-line status headline for the middle
// of the waiting screen - deliberately not the raw apiStatus text (which
// can be a long technical string like "HTTP 403 (session expired -
// recapture PHPSESSID/CHESSCOM_REMEMBERME)" meant for Serial/debugging,
// not a small e-paper screen with wrapping disabled - see initDisplay()).
static const char *statusHeadline(const ClockState &state) {
  if (!state.wifiConnected) return T(STR_WIFI_CONNECTING);
  if (state.apiOk) return T(STR_WAITING_FOR_GAME);
  if (strstr(state.apiStatus, "401") || strstr(state.apiStatus, "403")) return T(STR_SESSION_EXPIRED);
  return T(STR_CONNECTION_ERROR);
}

// Fixed box reserved for each clock's text (right-aligned, big font) -
// used for TRUE partial-window refreshes (setPartialWindow), so only
// this small area redraws each tick instead of the whole screen. Some
// e-paper controllers want the window x/width byte-aligned (multiple of
// 8), so we round for safety.
#define CLOCK_BOX_WIDTH 90
#define CLOCK_BOX_HEIGHT 32

static int clockBoxX() {
  int x = SCREEN_WIDTH - CLOCK_BOX_WIDTH;
  return (x / 8) * 8;
}

static int clockBoxWidthAligned() {
  int w = SCREEN_WIDTH - clockBoxX();
  return ((w + 7) / 8) * 8;
}

static void drawClockText(long clockMs, int baselineY) {
  int minutes = clockMs / 60000;
  int seconds = (clockMs / 1000) % 60;
  char clockBuf[8];
  snprintf(clockBuf, sizeof(clockBuf), "%02d:%02d", minutes, seconds);

  display.setFont(&FreeMonoBold18pt7b);
  printRightAligned(clockBuf, baselineY);
}

// Rating always goes on its own line (never squeezed onto the same line
// as the name - simpler, and avoids the clock ever getting close to the
// text). If the username is longer than NAME_LINE1_MAX_CHARS, the
// overflow moves down to join the rating on line 2.
#define NAME_LINE1_MAX_CHARS 10
#define NAME_LINE2_GAP 16          // vertical gap between the name line and the rating line
#define CLOCK_BASELINE_OFFSET 12   // clock sits a bit lower, roughly centered against the two text lines
#define NAME_TEXT_X 16             // fixed left margin - leaves room for the "on move" icon and keeps both players' text aligned whether or not the icon is shown

// Small filled right-pointing triangle ("on move" indicator) shown next
// to whoever is currently on the clock. Drawn as a shape rather than the
// unicode "▶" character, since the built-in GFX fonts don't include
// that glyph (it would print as garbage/blank).
static void drawPlayIcon(int textBaselineY) {
  int cy = textBaselineY - 6;  // roughly vertically centered on the name text
  display.fillTriangle(3, cy - 5, 3, cy + 5, 12, cy, GxEPD_BLACK);
}

static void drawPlayerLine(int y, const PlayerInfo &player, bool isActive) {
  display.setFont(&FreeMonoBold9pt7b);

  if (isActive) {
    drawPlayIcon(y);
  }

  // Always truncate to NAME_LINE1_MAX_CHARS and always put the rating on
  // its own line below - simpler than measuring/wrapping text, and gives
  // a consistent layout regardless of name length.
  char nameBuf[NAME_LINE1_MAX_CHARS + 1];
  strncpy(nameBuf, player.username, NAME_LINE1_MAX_CHARS);
  nameBuf[NAME_LINE1_MAX_CHARS] = '\0';

  display.setCursor(NAME_TEXT_X, y);
  display.print(nameBuf);

  char ratingBuf[16];
  snprintf(ratingBuf, sizeof(ratingBuf), "(%d)", player.rating);
  display.setCursor(NAME_TEXT_X, y + NAME_LINE2_GAP);
  display.print(ratingBuf);

  drawClockText(player.clockMs, y + CLOCK_BASELINE_OFFSET);
}

// The logo/status alternation ONLY applies while waiting for a game
// (avoids burn-in during long idle periods). Once a game starts, the
// game screen is shown continuously - checked by the caller before
// calling isLogoPhase() at all.
static bool isLogoPhase() {
  unsigned long cyclePos = millis() % (SCREEN_CYCLE_INTERVAL_MS * 2);
  return cyclePos < SCREEN_CYCLE_INTERVAL_MS;
}

// Setup (AP) mode screen: hotspot name + IP to connect to.
static void drawSetupModeContent(const ClockState &state) {
  display.setFont(&FreeMonoBold9pt7b);
  printCentered(T(STR_SETUP_MODE), 30);
  printCentered(SETUP_AP_NAME, 55);
  char ipLine[40];
  snprintf(ipLine, sizeof(ipLine), "%s %s", T(STR_IP), state.ipAddress);
  printCentered(ipLine, 80);
}

// Waiting-for-game status screen: top status bar (WiFi name+signal bars
// left, battery bars right), the app name centered right below it (its
// own line - putting it INLINE in the top bar collides with a long WiFi
// SSID, no room guaranteed for both), one big centered headline in the
// middle (either "Waiting for game" or a short error - see
// statusHeadline() - wraps to 2 lines if it doesn't fit on one), and the
// IP address at the bottom.
static void drawWaitingStatusContent(const ClockState &state) {
  drawTopStatusBar(state);

  display.setFont(&FreeMonoBold12pt7b);
  printCentered(T(STR_APP_NAME), 34);

  printCenteredWrapped(statusHeadline(state), 66, 22, SCREEN_WIDTH - 20);

  display.setFont(&FreeMonoBold9pt7b);
  char ipLine[24];
  snprintf(ipLine, sizeof(ipLine), "%s %s", T(STR_IP), state.wifiConnected ? state.ipAddress : "-");
  printCentered(ipLine, 114);
}

// Dedicated "game over" screen - shown for resultDisplayDurationMs
// (admin-portal configurable) instead of squeezing the result into the
// regular status screen's last line, per how prominent an outcome
// deserves to be. Falls back to the neutral compact summary
// (LiveGameClient.cpp's lastResultSummary) when myUsername isn't
// configured and win/loss couldn't be resolved.
static void drawResultContent(const ClockState &state) {
  const char *headline = nullptr;
  if (state.lastGameOutcome == OUTCOME_WIN) headline = T(STR_YOU_WON);
  else if (state.lastGameOutcome == OUTCOME_LOSS) headline = T(STR_YOU_LOST);
  else if (state.lastGameOutcome == OUTCOME_DRAW) headline = T(STR_DRAW);

  if (headline == nullptr) {
    display.setFont(&FreeMonoBold9pt7b);
    printCentered(T(STR_LAST_RESULT), 35);
    printCentered(state.lastResultSummary, 60);
    return;
  }

  display.setFont(&FreeMonoBold18pt7b);
  printCentered(headline, 42);

  display.setFont(&FreeMonoBold9pt7b);
  if (state.lastResultReason[0] != '\0') {
    char reasonLine[48];
    snprintf(reasonLine, sizeof(reasonLine), "(%s)", state.lastResultReason);
    printCentered(reasonLine, 64);
  }
  if (state.lastOpponentUsername[0] != '\0') {
    char vsLine[48];
    snprintf(vsLine, sizeof(vsLine), "vs %s", state.lastOpponentUsername);
    printCentered(vsLine, 84);
  }
  if (state.lastRatingKnown) {
    char ratingLine[24];
    snprintf(ratingLine, sizeof(ratingLine), "%+d", state.lastRatingDelta);
    printCentered(ratingLine, 106);
  }
}

// Which of state.players[0]/[1] is "me" (matches myUsername), so we can
// always draw ourselves in the bottom row and the opponent on top - like
// sitting across a real board. -1 if myUsername isn't set or doesn't
// match either player (falls back to chess.com's own order: [0] top,
// [1] bottom, same as before this existed).
static int myPlayerIndex(const ClockState &state) {
  if (myUsername[0] == '\0') return -1;
  for (int i = 0; i < 2; i++) {
    if (strcasecmp(state.players[i].username, myUsername) == 0) return i;
  }
  return -1;
}

// Game screen: always shown while a game is active, never alternates
// with the logo (that would be confusing mid-game).
static void drawGameContent(const ClockState &state) {
  display.setFont(&FreeMonoBold9pt7b);
  drawBatteryLabel(state.batteryPercentage);

  char header[32];
  snprintf(header, sizeof(header), "%s %d", T(STR_MOVE), state.moveCount);
  display.setCursor(5, 12);
  display.print(header);

  int bottomIndex = myPlayerIndex(state);
  if (bottomIndex == -1) bottomIndex = 1;  // unknown - keep the pre-existing order
  int topIndex = 1 - bottomIndex;

  drawPlayerLine(45, state.players[topIndex], state.activePlayerIndex == topIndex);
  drawPlayerLine(90, state.players[bottomIndex], state.activePlayerIndex == bottomIndex);
}

// Redraws ONLY the two clock boxes, using a REAL partial window
// (setPartialWindow). INTENTION was to avoid the full-screen flash when
// ticking the clock every second, but on real hardware this caused
// visual corruption (garbled characters, a distorted column on the
// left) - likely the partial-window coordinates not lining up correctly
// with DISPLAY_ROTATION on this panel/library combo. NOT CALLED for now
// (see IvoChess_Clock.ino - falls back to periodic full refresh
// instead). Left here for future debugging if we want to revisit true
// partial refresh.
//
// NOTE if reviving this: it still assumes raw index 0 -> top(45),
// index 1 -> bottom(90). drawGameContent() no longer does - it now
// places whichever player matches myUsername at the bottom (see
// myPlayerIndex()), so this would draw the wrong player's clock in the
// wrong box whenever "me" isn't raw index 1. Thread topIndex/bottomIndex
// through here the same way before re-enabling.
void updateGameClocksPartial(const ClockState &state) {
  const int baselineY[2] = {45 + CLOCK_BASELINE_OFFSET, 90 + CLOCK_BASELINE_OFFSET};  // matches drawPlayerLine(45/90, ...)
  int boxX = clockBoxX();
  int boxW = clockBoxWidthAligned();

  for (int i = 0; i < 2; i++) {
    int boxY = baselineY[i] - 24;

    display.setPartialWindow(boxX, boxY, boxW, CLOCK_BOX_HEIGHT);
    display.fillScreen(GxEPD_WHITE);
    display.setTextColor(GxEPD_BLACK);
    drawClockText(state.players[i].clockMs, baselineY[i]);
    display.display(true);  // partial - only this small window redraws, no full-screen flash
  }
}

void updateDisplay(bool fullRefresh, const ClockState &state) {
  display.setFullWindow();
  display.fillScreen(GxEPD_WHITE);
  display.setTextColor(GxEPD_BLACK);

  bool showingResult = (millis() < state.resultDisplayUntilMs);

  if (state.hasGame) {
    // Never alternates to the logo while a game is in progress.
    drawGameContent(state);
  } else if (state.apMode) {
    drawSetupModeContent(state);
  } else if (showingResult) {
    // Dedicated result screen has priority over the logo for its
    // (admin-portal configurable) window.
    drawResultContent(state);
  } else if (isLogoPhase()) {
    drawLogoContent();
  } else {
    drawWaitingStatusContent(state);
  }

  display.display(!fullRefresh);
}
