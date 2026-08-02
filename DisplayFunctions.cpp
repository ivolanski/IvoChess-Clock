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

// ---- top status bar: WiFi name+signal (left), battery bars (right) ----
// Shared by the waiting/status screen AND the game screen, for a
// consistent look between them (per the user's mockups).

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

// ---- shared bottom bar: site URL (left) + a right-aligned label:value ----
// (admin IP on the waiting screen, move count on the game screen) - used
// by both screens for a consistent look. The site doesn't exist yet
// (per the user: "e mais tipo uma propaganda mesmo" - it's advertising
// for a future site), so this is static text, not a working link.
#define BOTTOM_BAR_Y 104
#define BOTTOM_BAR_BASELINE 118
#define SITE_URL "ivochess.ivolanski.com"

static void drawBottomBar(const char *rightLabel, const char *rightValue) {
  display.drawFastHLine(0, BOTTOM_BAR_Y, SCREEN_WIDTH, GxEPD_BLACK);
  display.setFont(&FreeMonoBold9pt7b);
  display.setCursor(STATUS_BAR_MARGIN, BOTTOM_BAR_BASELINE);
  display.print(SITE_URL);

  char right[32];
  snprintf(right, sizeof(right), "%s%s", rightLabel, rightValue);
  printRightAligned(right, BOTTOM_BAR_BASELINE, STATUS_BAR_MARGIN);
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

// Like printCentered(), but centers within an arbitrary [xLeft, xLeft+width)
// span instead of the full screen - used for the game screen's two
// player halves.
static void printCenteredIn(const char *text, int xLeft, int width, int y) {
  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
  int x = xLeft + ((int)width - (int)w) / 2;
  if (x < xLeft) x = xLeft;  // clamp rather than let a too-long string spill past its half
  display.setCursor(x, y);
  display.print(text);
}

// Game screen layout: a vertical divider splits the area below the top
// bar into two halves - opponent on the LEFT, "me" on the RIGHT (per the
// user's mockup - matches sitting across a real board with "me" nearer).
// Each half gets its own centered name/rating/clock.
#define GAME_DIVIDER_X (SCREEN_WIDTH / 2)
#define GAME_CONTENT_TOP (STATUS_BAR_BASELINE + 6)  // just below the top bar's separator line
#define GAME_HALF_NAME_Y 34
#define GAME_HALF_RATING_Y 50
#define GAME_HALF_CLOCK_Y 90
#define GAME_HALF_MAX_CHARS 8  // conservative for a 125px half at 9pt bold mono - leaves margin so it can't crowd the divider

// Solid bar at the top of a half, marking whoever is currently on the
// clock - simpler and reads clearer at this width than a small icon
// squeezed next to a centered name.
static void drawActiveHighlight(int xLeft, int width) {
  display.fillRect(xLeft + 6, GAME_CONTENT_TOP + 1, width - 12, 3, GxEPD_BLACK);
}

static void drawPlayerHalf(int xLeft, int width, const PlayerInfo &player, bool isActive) {
  if (isActive) {
    drawActiveHighlight(xLeft, width);
  }

  display.setFont(&FreeMonoBold9pt7b);
  char nameBuf[GAME_HALF_MAX_CHARS + 1];
  strncpy(nameBuf, player.username, GAME_HALF_MAX_CHARS);
  nameBuf[GAME_HALF_MAX_CHARS] = '\0';
  printCenteredIn(nameBuf, xLeft, width, GAME_HALF_NAME_Y);

  char ratingBuf[16];
  snprintf(ratingBuf, sizeof(ratingBuf), "(%d)", player.rating);
  printCenteredIn(ratingBuf, xLeft, width, GAME_HALF_RATING_Y);

  int minutes = player.clockMs / 60000;
  int seconds = (player.clockMs / 1000) % 60;
  char clockBuf[8];
  snprintf(clockBuf, sizeof(clockBuf), "%02d:%02d", minutes, seconds);
  display.setFont(&FreeMonoBold18pt7b);
  printCenteredIn(clockBuf, xLeft, width, GAME_HALF_CLOCK_Y);
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
  display.drawFastHLine(0, 40, SCREEN_WIDTH, GxEPD_BLACK);

  printCenteredWrapped(statusHeadline(state), 66, 22, SCREEN_WIDTH - 20);

  drawBottomBar("WEBADMIN:", state.wifiConnected ? state.ipAddress : "-");
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
// with the logo (that would be confusing mid-game). Same top bar as the
// waiting screen (consistent look), a vertical divider splitting
// opponent (left) from "me" (right), and the same bottom-bar style
// (site URL left, move count right) as the waiting screen's IP line.
static void drawGameContent(const ClockState &state) {
  drawTopStatusBar(state);
  display.drawFastVLine(GAME_DIVIDER_X, GAME_CONTENT_TOP, BOTTOM_BAR_Y - GAME_CONTENT_TOP, GxEPD_BLACK);

  int meIndex = myPlayerIndex(state);
  int rightIndex = (meIndex == -1) ? 1 : meIndex;  // unknown - keep the pre-existing raw order
  int leftIndex = 1 - rightIndex;

  drawPlayerHalf(0, GAME_DIVIDER_X, state.players[leftIndex], state.activePlayerIndex == leftIndex);
  drawPlayerHalf(GAME_DIVIDER_X, SCREEN_WIDTH - GAME_DIVIDER_X, state.players[rightIndex], state.activePlayerIndex == rightIndex);

  char moveLabel[16];
  snprintf(moveLabel, sizeof(moveLabel), "%s: ", T(STR_MOVE));
  char moveCountStr[8];
  snprintf(moveCountStr, sizeof(moveCountStr), "%d", state.moveCount);
  drawBottomBar(moveLabel, moveCountStr);
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
