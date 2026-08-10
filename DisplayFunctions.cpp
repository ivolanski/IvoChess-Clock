#include "DisplayFunctions.h"
#include "config.h"
#include "Translations.h"
#include "AdminPortal.h"
#include "GameDataSource.h"
#include "ChessConnectBLE.h"

#include <ctype.h>

#include <GxEPD2_BW.h>
#include <Fonts/FreeMonoBold9pt7b.h>
#include <Fonts/FreeMonoBold12pt7b.h>
#include <Fonts/FreeMonoBold18pt7b.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

// The site doesn't exist yet (per the user: "e mais tipo uma propaganda
// mesmo" - it's advertising for a future site), so this is static text
// everywhere it's used, not a working link.
#define SITE_URL "ivochess.ivolanski.com"

// static: the display object, and every function that touches it, are
// now used EXCLUSIVELY from the dedicated display task started by
// startDisplayTask() below - nothing else may call display.* directly.
// A full e-paper refresh takes ~3.6s on real hardware (confirmed against
// GxEPD2's own busy-wait timing constants); running it inline in the
// main loop() blocked the live game connection (WebSocket/NDJSON stream)
// and the admin webserver for that whole window, causing the on-screen
// clock to visibly lag the real game by several seconds. See
// requestDisplayUpdate()'s doc comment (DisplayFunctions.h) for the
// non-blocking hand-off this task consumes from.
static GxEPD2_BW<GxEPD2_213_B74, GxEPD2_213_B74::HEIGHT> display(
    GxEPD2_213_B74(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

static void initDisplay() {
  display.init(115200, true, 2, false);
  display.setRotation(DISPLAY_ROTATION);
  display.setTextWrap(false);  // avoids any text near the edge wrapping on its own
  Serial.println("[Display] Initialized (GxEPD2_213_B74).");
}

// Defined further down (needs printCentered(), declared below).
static void drawLogoContent();

static void drawStartupScreen() {
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

// Row of alternating filled/empty squares, like a strip cut from a chess
// board - purely decorative, themes the burn-in screen without needing
// any image asset (a bitmap would cost flash and a conversion step; this
// is just fillRect calls). Centered horizontally.
#define LOGO_SQUARE 16
#define LOGO_SQUARES 8
static void drawChessStrip(int yTop) {
  int totalW = LOGO_SQUARE * LOGO_SQUARES;
  int xStart = (SCREEN_WIDTH - totalW) / 2;
  for (int i = 0; i < LOGO_SQUARES; i++) {
    if (i % 2 == 0) {
      display.fillRect(xStart + i * LOGO_SQUARE, yTop, LOGO_SQUARE, LOGO_SQUARE, GxEPD_BLACK);
    }
  }
}

// Burn-in-avoidance screen - alternates with the status screen while
// idle (see isLogoPhaseNow()). Deliberately laid out to NOT reuse the
// waiting screen's pixel positions (chess-strip decoration instead of
// status bars, a big 2-line all-caps title instead of one 12pt line,
// different row heights throughout) so the two screens' ink doesn't
// keep landing on the same pixels over thousands of cycles - some
// overlap is unavoidable on a screen this small, but this is far more
// different than the old single centered line was.
static void drawLogoContent() {
  drawChessStrip(6);
  drawChessStrip(100);

  display.setFont(&FreeMonoBold18pt7b);
  const char *name = T(STR_APP_NAME);  // "IvoChess Clock" - same in every language
  const char *space = strchr(name, ' ');
  if (space != nullptr) {
    char line1[16], line2[16];
    size_t n1 = space - name;
    if (n1 >= sizeof(line1)) n1 = sizeof(line1) - 1;
    for (size_t i = 0; i < n1; i++) line1[i] = toupper((unsigned char)name[i]);
    line1[n1] = '\0';

    size_t n2 = strlen(space + 1);
    if (n2 >= sizeof(line2)) n2 = sizeof(line2) - 1;
    for (size_t i = 0; i < n2; i++) line2[i] = toupper((unsigned char)space[1 + i]);
    line2[n2] = '\0';

    printCentered(line1, 48);
    printCentered(line2, 76);
  } else {
    printCentered(name, 62);
  }

  display.setFont(&FreeMonoBold9pt7b);
  printCentered(SITE_URL, 92);
}

// Like printCentered(), but (a) vertically centers within [topY, bottomY]
// - using the real measured glyph height (getTextBounds), not a guessed
// baseline - rather than printing at a fixed y, and (b) wraps to a second
// centered line if the text doesn't fit maxWidth on one line, splitting
// at the space closest to the middle of the string so both halves come
// out reasonably balanced. Only wraps once (2 lines total) - plenty for
// the short status phrases this is used for; not a general
// paragraph-wrapping engine.
static void printCenteredWrapped(const char *text, int topY, int bottomY, int lineHeight, int maxWidth) {
  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);

  if (w <= (uint16_t)maxWidth) {
    int baseline = (topY + bottomY) / 2 - y1 - (int)h / 2;
    printCentered(text, baseline);
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
    int baseline = (topY + bottomY) / 2 - y1 - (int)h / 2;
    printCentered(text, baseline);
    return;
  }

  char line1[48], line2[48];
  int n1 = splitAt < (int)sizeof(line1) - 1 ? splitAt : (int)sizeof(line1) - 1;
  strncpy(line1, text, n1);
  line1[n1] = '\0';
  strncpy(line2, text + splitAt + 1, sizeof(line2) - 1);  // +1 skips the space itself
  line2[sizeof(line2) - 1] = '\0';

  int blockHeight = lineHeight + (int)h;
  int baseline1 = (topY + bottomY) / 2 - blockHeight / 2 - y1;
  printCentered(line1, baseline1);
  printCentered(line2, baseline1 + lineHeight);
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
  int cursorX;
  if (!state.wifiConnected) {
    char buf[24];
    snprintf(buf, sizeof(buf), "%s %s", T(STR_WIFI), T(STR_DISCONNECTED));
    display.print(buf);
    int16_t x1, y1;
    uint16_t w, h;
    display.getTextBounds(buf, 0, 0, &x1, &y1, &w, &h);
    cursorX = STATUS_BAR_MARGIN + (int)w;
  } else {
    char ssidShort[11];
    strncpy(ssidShort, state.wifiSSID, 10);
    ssidShort[10] = '\0';
    display.print(ssidShort);

    int16_t x1, y1;
    uint16_t w, h;
    display.getTextBounds(ssidShort, 0, 0, &x1, &y1, &w, &h);
    int barsX = STATUS_BAR_MARGIN + w + 6;
    int barsW = drawSignalBars(barsX, STATUS_BAR_BASELINE, wifiBarCount(state.wifiStrength), 4);
    cursorX = barsX + barsW;
  }

  // ChessConnect only: a "BT" indicator right after the WiFi block -
  // WiFi here is just for the admin portal/NTP, the actual game data
  // comes over Bluetooth, so it's worth its own at-a-glance status.
  // "BT(P)" = advertising, waiting to pair; "BT" + bars once a central
  // is actually connected. Real per-connection RSSI isn't straightforward
  // to read from this BLE stack, so the bars show full/empty as a
  // connected/not indicator rather than a true variable signal strength
  // (unlike the WiFi bars, which are real RSSI) - worth knowing if this
  // ever looks static compared to the WiFi ones.
  if (currentDataSource == DATA_SOURCE_CHESSCONNECT_BLE) {
    cursorX += 8;
    display.setCursor(cursorX, STATUS_BAR_BASELINE);
    bool connected = chessConnectBleIsConnected();
    const char *btLabel = connected ? "BT" : "BT(P)";
    display.print(btLabel);
    if (connected) {
      int16_t x1, y1;
      uint16_t w, h;
      display.getTextBounds(btLabel, 0, 0, &x1, &y1, &w, &h);
      drawSignalBars(cursorX + (int)w + 4, STATUS_BAR_BASELINE, 4, 4);
    }
  }

  // Right: battery, as bars only (no percentage number - config asked
  // for an at-a-glance gauge here, the exact number is still on the game
  // screen via drawBatteryLabel).
  const int battTotalW = 4 * 4 + 3 * 1;  // matches drawGaugeBars(total=4) geometry
  int battX = SCREEN_WIDTH - STATUS_BAR_MARGIN - battTotalW;
  drawGaugeBars(battX, STATUS_BAR_BASELINE, batteryBarCount(state.batteryPercentage), 4);

  display.drawFastHLine(0, STATUS_BAR_BASELINE + 5, SCREEN_WIDTH, GxEPD_BLACK);
}

// ---- shared bottom bar: a small right-aligned note (admin IP on the ----
// waiting screen, move count on the game screen) just above the
// separator line, then the footer itself is just the site URL alone on
// its own row. The site URL (22 chars) alone is already ~97% of the
// screen width at the smallest bold font available (FreeMonoBold9pt7b,
// 11px/char) - it and the note CANNOT share one row at any usable size
// without overlapping, so the note gets its own (visibly smaller) row.
#define FOOTER_NOTE_GAP 4  // gap between the small note's bottom edge and the separator line below it
#define FOOTER_Y_ADJUST 6  // shifts the whole footer row (note + site URL) down slightly for visual balance
#define BOTTOM_SEP_Y 100
// The separator LINE sits independently of the note/URL zone above it
// (BOTTOM_SEP_Y stays their reference point) - kept as its own constant,
// rather than just moving BOTTOM_SEP_Y, so the note/URL don't drift along
// with it if the line's position changes again.
#define FOOTER_LINE_Y (BOTTOM_SEP_Y + 4)
#define MESSAGE_ZONE_BOTTOM 84  // leaves room above the footer note for its height + FOOTER_NOTE_GAP

// Small right-aligned note, using the GFX library's built-in font
// (setFont(NULL)) rather than one of the "Free" bold fonts - deliberately
// smaller than the site URL below it, and there's no smaller custom font
// installed to reach for. NOTE: unlike the custom "Free" fonts used
// everywhere else in this file, the GFX built-in font anchors text at its
// TOP-LEFT corner, not its baseline - bottomY here is where we want the
// text's bottom edge to land, and it's converted to a top-left cursor
// position accordingly (getting this backwards is what made the note
// touch the separator line below it).
static void printSmallRightAligned(const char *text, int bottomY) {
  display.setFont(NULL);
  display.setTextSize(1);
  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
  display.setCursor(SCREEN_WIDTH - (int)w - STATUS_BAR_MARGIN, bottomY - (int)h - (int)y1);
  display.print(text);
}

// Vertically centered in the gap between the separator and the screen's
// bottom edge (rather than a guessed fixed baseline), same technique as
// printCenteredWrapped() - measure, then center around the real glyph box
// instead of the nominal font size. Factored out of drawBottomBar() so
// drawGameMovePartial() below can redraw it too - its partial window now
// reaches far enough down to overlap the top of this text (see that
// function's comment), so it has to redraw this or risk erasing part of
// it without ever putting it back.
static void drawSiteUrl() {
  display.setFont(&FreeMonoBold9pt7b);
  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(SITE_URL, 0, 0, &x1, &y1, &w, &h);
  int zoneTop = BOTTOM_SEP_Y + 2;
  int zoneBottom = SCREEN_HEIGHT - STATUS_BAR_MARGIN;
  int baseline = (zoneTop + zoneBottom) / 2 - y1 - (int)h / 2 + FOOTER_Y_ADJUST;
  display.setCursor(STATUS_BAR_MARGIN, baseline);
  display.print(SITE_URL);
}

static void drawBottomBar(const char *noteLabel, const char *noteValue) {
  char note[32];
  snprintf(note, sizeof(note), "%s%s", noteLabel, noteValue);
  printSmallRightAligned(note, BOTTOM_SEP_Y - FOOTER_NOTE_GAP + FOOTER_Y_ADJUST);

  display.drawFastHLine(0, FOOTER_LINE_Y, SCREEN_WIDTH, GxEPD_BLACK);

  drawSiteUrl();
}

// Short, human, ALWAYS-fits-on-one-line status headline for the middle
// of the waiting screen - deliberately not the raw apiStatus text (which
// can be a long technical string like "HTTP 403 (session expired -
// recapture PHPSESSID/CHESSCOM_REMEMBERME)" meant for Serial/debugging,
// not a small e-paper screen with wrapping disabled - see initDisplay()).
static const char *statusHeadline(const ClockState &state) {
  if (!state.wifiConnected) return T(STR_WIFI_CONNECTING);
  // Checked before apiOk/apiStatus: once GameDataSource.cpp gives up
  // polling after WAITING_FOR_GAME_TIMEOUT_MS idle, this is the ONLY
  // thing worth telling the user - a restart is required, no other
  // status (stale "OK", old error) is still meaningful once polling has
  // actually stopped.
  if (state.waitingTimedOut) return T(STR_RESTART_FOR_GAME);
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
#define GAME_HALF_CLOCK_Y 86  // leaves a clear gap above the footer note row
// Rating sits in the gap between the name and the clock, not at a fixed
// y - GAME_NAME_BOTTOM clears the name's descenders (9pt, ~5px past its
// baseline) plus a bit of extra breathing room, and GAME_CLOCK_TOP is the
// 18pt clock font's own ascent (~21px above its baseline), so the zone
// tracks both neighbors exactly.
#define GAME_NAME_DESCENDER_CLEARANCE 5
#define GAME_NAME_EXTRA_GAP 6
#define GAME_NAME_BOTTOM (GAME_HALF_NAME_Y + GAME_NAME_DESCENDER_CLEARANCE + GAME_NAME_EXTRA_GAP)
#define GAME_CLOCK_TOP (GAME_HALF_CLOCK_Y - 21)
#define GAME_HALF_MAX_CHARS 8  // conservative for a 125px half at 9pt bold mono - leaves margin so it can't crowd the divider

// "On move" indicator - a small filled right-pointing triangle (the
// original single-column layout's icon; a solid highlight bar was tried
// for this split layout but read as just "meh" in practice). cx/cy is
// where the icon should be CENTERED (cx = the clock's first digit - see
// drawPlayerHalf; cy = the rating row's own vertical center).
static void drawPlayIcon(int cx, int cy) {
  display.fillTriangle(cx - 5, cy - 5, cx - 5, cy + 5, cx + 4, cy, GxEPD_BLACK);
}

// GxEPD2's built-in font (setFont(NULL)) - the smallest one available (no
// custom "Free" font installed goes below 9pt - see printSmallRightAligned()'s
// comment) - unlike the "Free" fonts used everywhere else in this file, it
// anchors text at its TOP-LEFT corner, not its baseline, so centering it
// vertically needs its own helper rather than reusing printCenteredIn().
static void printCenteredInSmall(const char *text, int xLeft, int width, int centerY, uint8_t size) {
  display.setFont(NULL);
  display.setTextSize(size);
  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
  int x = xLeft + ((int)width - (int)w) / 2;
  if (x < xLeft) x = xLeft;
  display.setCursor(x, centerY - y1 - (int)h / 2);
  display.print(text);
  display.setTextSize(1);  // restore the default every other caller of the built-in font (printSmallRightAligned) assumes
}

// Formats + draws just the clock digits for one player's half, centered
// within [xLeft, xLeft+width) at the fixed GAME_HALF_CLOCK_Y baseline -
// factored out of drawPlayerHalf() so the full game-screen redraw and the
// partial-window clock-only tick (drawGameClockPartial() below) draw
// pixel-identically instead of two copies of the same formatting/font/
// centering logic drifting apart over time. Returns the x position used
// (drawPlayerHalf() needs it to also place the "on move" triangle
// relative to the clock's first digit).
static int drawClockDigits(int xLeft, int width, long clockMs) {
  int minutes = clockMs / 60000;
  int seconds = (clockMs / 1000) % 60;
  char clockBuf[8];
  snprintf(clockBuf, sizeof(clockBuf), "%02d:%02d", minutes, seconds);
  display.setFont(&FreeMonoBold18pt7b);
  int16_t cx1, cy1;
  uint16_t cw, ch;
  display.getTextBounds(clockBuf, 0, 0, &cx1, &cy1, &cw, &ch);
  int clockX = xLeft + ((int)width - (int)cw) / 2;
  if (clockX < xLeft) clockX = xLeft;
  display.setCursor(clockX, GAME_HALF_CLOCK_Y);
  display.print(clockBuf);
  return clockX;
}

// Rating + "on move" triangle + clock digits - everything in a player's
// half EXCEPT the name, which is the only piece that's truly static once
// a game has started (rating/clock/turn all change as the game
// progresses). Factored out of drawPlayerHalf() so the full game-screen
// redraw and the partial-window move update (drawGameMovePartial() below)
// draw pixel-identically instead of two copies drifting apart.
// Pulls the rating/triangle row up from the exact midpoint between the
// name and the clock - dead center crowds the clock above it (GAME_NAME_BOTTOM
// already accounts for the opposite problem, crowding the name - see its
// own comment).
#define GAME_RATING_Y_BIAS 5
// The built-in font at size 1 read as too small on real hardware - size 2
// is the closest bigger step available (there's no custom font between
// the built-in one and 9pt - see printCenteredInSmall()'s comment).
#define GAME_RATING_TEXT_SIZE 2

static void drawRatingAndClock(int xLeft, int width, const PlayerInfo &player, bool isActive) {
  int ratingCenterY = (GAME_NAME_BOTTOM + GAME_CLOCK_TOP) / 2 - GAME_RATING_Y_BIAS;
  // ChessConnect never sends a rating (see RATING_UNKNOWN) - hide the row
  // entirely rather than show a placeholder "(?)", per direct feedback
  // from real-hardware testing.
  if (player.rating != RATING_UNKNOWN) {
    char ratingBuf[16];
    snprintf(ratingBuf, sizeof(ratingBuf), "(%d)", player.rating);
    printCenteredInSmall(ratingBuf, xLeft, width, ratingCenterY, GAME_RATING_TEXT_SIZE);
  }

  int clockX = drawClockDigits(xLeft, width, player.clockMs);

  if (isActive) {
    // Centered above the clock's first digit - one 18pt mono glyph is
    // ~21px wide, so +10 lands on that digit's own center.
    drawPlayIcon(clockX + 10, ratingCenterY);
  }
}

static void drawPlayerHalf(int xLeft, int width, const PlayerInfo &player, bool isActive) {
  display.setFont(&FreeMonoBold9pt7b);
  char nameBuf[GAME_HALF_MAX_CHARS + 1];
  strncpy(nameBuf, player.username, GAME_HALF_MAX_CHARS);
  nameBuf[GAME_HALF_MAX_CHARS] = '\0';
  printCenteredIn(nameBuf, xLeft, width, GAME_HALF_NAME_Y);

  drawRatingAndClock(xLeft, width, player, isActive);
}

// The logo/status alternation ONLY applies while genuinely idle - not
// mid-game (game screen already refreshes on every clock tick, it
// doesn't need anti-burn-in cycling) and not for a while after a game
// was last active (a live-connection blip briefly drops hasGame while it
// reconnects - see GAME_RECONNECT_GRACE_MS - showing the logo during
// that looked like the clock had frozen). The single source of truth for
// this decision - IvoChess_Clock.ino calls it too, for its own
// full-refresh scheduling, so the two never disagree about what's
// currently on screen.
bool isLogoPhaseNow(const ClockState &state) {
  if (state.hasGame) return false;
  if (millis() < state.resultDisplayUntilMs) return false;
  if (state.lastGameActiveAt != 0 && (millis() - state.lastGameActiveAt) < GAME_RECONNECT_GRACE_MS) return false;

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

  // All-caps - this is the most important text on the screen, it should
  // read that way (the font's already bold).
  char headline[48];
  strncpy(headline, statusHeadline(state), sizeof(headline) - 1);
  headline[sizeof(headline) - 1] = '\0';
  for (char *p = headline; *p; p++) *p = toupper((unsigned char)*p);

  printCenteredWrapped(headline, 44, MESSAGE_ZONE_BOTTOM, 22, SCREEN_WIDTH - 20);

  drawBottomBar("WEBADMIN:", state.wifiConnected ? state.ipAddress : "-");
}

// Dedicated "game over" screen - shown for resultDisplayDurationMs
// (admin-portal configurable) instead of squeezing the result into the
// regular status screen's last line, per how prominent an outcome
// deserves to be. Falls back to the neutral compact summary
// (LiveGameClient.cpp's lastResultSummary) when activeMyUsername() isn't
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
    char vsLine[56];
    if (state.lastPlayerRatingsKnown) {
      snprintf(vsLine, sizeof(vsLine), "vs %s (%d)", state.lastOpponentUsername, state.lastOpponentRating);
    } else {
      snprintf(vsLine, sizeof(vsLine), "vs %s", state.lastOpponentUsername);
    }
    printCentered(vsLine, 84);
  }
  if (state.lastPlayerRatingsKnown) {
    char ratingLine[32];
    if (state.lastRatingKnown) {
      snprintf(ratingLine, sizeof(ratingLine), "You: %d (%+d)", state.lastMyRating, state.lastRatingDelta);
    } else {
      snprintf(ratingLine, sizeof(ratingLine), "You: %d", state.lastMyRating);
    }
    printCentered(ratingLine, 106);
  }
}

// Which of state.players[0]/[1] is "me" (matches activeMyUsername() -
// whichever service's own username is currently relevant, see
// GameDataSource.h), so we can always draw ourselves in the bottom row
// and the opponent on top - like sitting across a real board. -1 if it
// isn't set or doesn't match either player (falls back to the server's
// own order: [0] top, [1] bottom, same as before this existed).
static int myPlayerIndex(const ClockState &state) {
  const char *me = activeMyUsername();
  if (me[0] == '\0') return -1;
  for (int i = 0; i < 2; i++) {
    if (strcasecmp(state.players[i].username, me) == 0) return i;
  }
  return -1;
}

// Game screen: always shown while a game is active, never alternates
// with the logo (that would be confusing mid-game). Same top bar as the
// waiting screen (consistent look), a vertical divider splitting
// opponent (left) from "me" (right), and the same bottom-bar style
// (site URL left, move count right) as the waiting screen's IP line.
// Shared between the full redraw (drawGameContent) and the partial-window
// move update (drawGameMovePartial) so both render pixel-identically -
// see that function's own comment for why keeping them in sync matters.
// ChessConnect's opponent move text (state.chessConnectMoveText) briefly
// takes over this slot instead of the move count when present - the
// first real UI use of that data (chess.com/Lichess never send move
// text at all). Outside that window, MOVE_COUNT_UNKNOWN (ChessConnect
// never sends a real count either) hides the slot entirely rather than
// showing a placeholder "?", per direct feedback from real-hardware
// testing.
static void bottomBarMoveContent(const ClockState &state, char *labelOut, size_t labelOutLen,
                                  char *valueOut, size_t valueOutLen) {
  if (state.chessConnectMoveTextUntilMs != 0 && millis() < state.chessConnectMoveTextUntilMs) {
    snprintf(labelOut, labelOutLen, "%s: ", T(STR_OPP_MOVE));
    snprintf(valueOut, valueOutLen, "%s", state.chessConnectMoveText);
    return;
  }
  if (state.moveCount == MOVE_COUNT_UNKNOWN) {
    labelOut[0] = '\0';
    valueOut[0] = '\0';
    return;
  }
  snprintf(labelOut, labelOutLen, "%s: ", T(STR_MOVE));
  snprintf(valueOut, valueOutLen, "%d", state.moveCount);
}

static void drawGameContent(const ClockState &state) {
  drawTopStatusBar(state);
  display.drawFastVLine(GAME_DIVIDER_X, GAME_CONTENT_TOP, FOOTER_LINE_Y - GAME_CONTENT_TOP, GxEPD_BLACK);

  int meIndex = myPlayerIndex(state);
  int rightIndex = (meIndex == -1) ? 1 : meIndex;  // unknown - keep the pre-existing raw order
  int leftIndex = 1 - rightIndex;

  drawPlayerHalf(0, GAME_DIVIDER_X, state.players[leftIndex], state.activePlayerIndex == leftIndex);
  drawPlayerHalf(GAME_DIVIDER_X, SCREEN_WIDTH - GAME_DIVIDER_X, state.players[rightIndex], state.activePlayerIndex == rightIndex);

  char moveLabel[16];
  char moveCountStr[24];
  bottomBarMoveContent(state, moveLabel, sizeof(moveLabel), moveCountStr, sizeof(moveCountStr));
  drawBottomBar(moveLabel, moveCountStr);
}

// ---------------------------------------------------------------------------
// True partial-window clock tick (drawGameClockPartial). Validated on real
// hardware first via a standalone POC (tests/epaper_partial_refresh_poc/)
// before being ported here - see that sketch's header comment for the full
// writeup. Two things that POC (and re-reading the deleted first attempt
// at this, git commit e470d66) established that matter here:
//
//   1. setPartialWindow() must get PLAIN logical coordinates, never
//      manually byte-rounded by the caller - it rotates them into the
//      panel's physical coordinates internally and does its own alignment
//      AFTER that (GxEPD2_BW.h). The deleted attempt hand-aligned the X
//      axis, which is only the axis that matters for rotation 0/2 - this
//      project uses DISPLAY_ROTATION 1, where Y/H is the one that needs
//      alignment, so the old code aligned the wrong axis on top of (not
//      instead of) the library's own correct handling. Likely the actual
//      cause of the corruption seen back then.
//   2. The window must be a FIXED rectangle, identical on every tick, not
//      recomputed from that tick's specific digits. A monospace font's
//      per-string INK bounding box can still vary by a couple pixels
//      between different digit combinations (e.g. "11:11" vs "88:88")
//      even though the ADVANCE width is fixed - re-measuring per tick and
//      shrinking the window for a narrower value would leave the previous,
//      wider frame's pixels un-erased at the edges. clockBoxes[] below is
//      measured ONCE from a reference string and reused forever.
// ---------------------------------------------------------------------------

// [0] = left half's clock box, [1] = right half's - x/w only; the y-range
// reuses GAME_CLOCK_TOP/GAME_HALF_CLOCK_Y directly (the same fixed zone
// boundary drawPlayerHalf() already reserves for the clock text, which by
// construction can't collide with the rating/"on move" triangle above it).
struct ClockBox { int16_t x; uint16_t w; };
static ClockBox clockBoxes[2];
static bool clockBoxesMeasured = false;

#define CLOCK_PARTIAL_X_MARGIN 6       // absorbs the per-frame ink-width wobble noted above

// setPartialWindow()'s Y/H (this project uses DISPLAY_ROTATION 1, so Y/H
// is the axis that lands on the panel's physical byte-addressed axis -
// see the block comment on drawGameMovePartial() below for the full
// derivation) get silently ROUNDED to an 8px boundary by the library
// AFTER rotation: if the caller's Y/H aren't already multiples of 8, the
// library EXPANDS the window outward to the nearest ones, redrawing (and,
// worse, ERASING via fillScreen()) more than the caller asked for -
// including neighboring content the caller never intended to touch and
// won't redraw. Chosen here as the smallest 8-aligned window that still
// fully contains the clock's actual ink (GAME_CLOCK_TOP..GAME_HALF_CLOCK_Y,
// currently 65..86), so the library renders EXACTLY this window with no
// expansion. Re-derive these two if GAME_CLOCK_TOP/GAME_HALF_CLOCK_Y ever
// change.
#define CLOCK_PARTIAL_WINDOW_Y 64
#define CLOCK_PARTIAL_WINDOW_H 24

static void measureClockBoxes() {
  display.setFont(&FreeMonoBold18pt7b);
  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds("88:88", 0, 0, &x1, &y1, &w, &h);
  const int xLeftOf[2] = {0, GAME_DIVIDER_X};
  const int widthOf[2] = {GAME_DIVIDER_X, SCREEN_WIDTH - GAME_DIVIDER_X};
  for (int i = 0; i < 2; i++) {
    int x = xLeftOf[i] + ((int)widthOf[i] - (int)w) / 2;
    if (x < xLeftOf[i]) x = xLeftOf[i];
    clockBoxes[i].x = (int16_t)x;
    clockBoxes[i].w = w;
  }
  clockBoxesMeasured = true;
}

// Redraws ONLY the currently active player's clock digits, via a real
// partial-window refresh - see the block comment above. No-op if there's
// no game or activePlayerIndex isn't a valid side (nothing to tick).
static void drawGameClockPartial(const ClockState &state) {
  if (!state.hasGame) return;
  int idx = state.activePlayerIndex;
  if (idx != 0 && idx != 1) return;

  if (!clockBoxesMeasured) measureClockBoxes();

  // Same left/right assignment as drawGameContent() - "me" always on the
  // right, matching the full redraw, so the partial tick never draws the
  // active clock on the wrong side of the divider.
  int meIndex = myPlayerIndex(state);
  int rightIndex = (meIndex == -1) ? 1 : meIndex;
  int leftIndex = 1 - rightIndex;
  int side = (idx == leftIndex) ? 0 : 1;
  int xLeft = (side == 0) ? 0 : GAME_DIVIDER_X;
  int width = (side == 0) ? GAME_DIVIDER_X : (SCREEN_WIDTH - GAME_DIVIDER_X);

  const ClockBox &box = clockBoxes[side];
  int windowX = (int)box.x - CLOCK_PARTIAL_X_MARGIN;
  int windowW = (int)box.w + 2 * CLOCK_PARTIAL_X_MARGIN;

  display.setPartialWindow(windowX, CLOCK_PARTIAL_WINDOW_Y, windowW, CLOCK_PARTIAL_WINDOW_H);
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setTextColor(GxEPD_BLACK);
    drawClockDigits(xLeft, width, state.players[idx].clockMs);
  } while (display.nextPage());
}

// True partial-window refresh of everything a MOVE can change: both
// halves' rating/"on move" triangle/clock zone, the move count, and the
// small stretch of the vertical divider line that passes through that
// zone - but NOT the name row above it (truly static once a game starts)
// or the top status bar (WiFi/battery, unrelated to moves). One combined
// window spanning the full width, rather than one window per element -
// simpler, and avoids firing off several back-to-back partial-mode LUT
// updates for a single move (a GxEPD2/SSD1680 combination elsewhere was
// found, during research for this feature, to behave oddly when a
// partial update's power-on sequence runs back-to-back like that - this
// sidesteps the question entirely by only ever doing one partial update
// per move).
//
// Window Y/H MUST be multiples of 8 - same reason as
// CLOCK_PARTIAL_WINDOW_Y/H above (this project's rotation puts Y/H on the
// panel's physical byte-addressed axis). GAME_NAME_BOTTOM/FOOTER_LINE_Y
// themselves aren't multiples of 8, so this does NOT use them directly -
// GAME_MOVE_PARTIAL_WINDOW_Y/H below are the smallest 8-aligned window
// that still fully contains everything this needs to draw, including
// FOOTER_LINE_Y (currently 40..112). Because this window reaches down to
// FOOTER_LINE_Y, it also reaches into the top of the site URL text just
// below it, so that has to be redrawn here too, not just the two lines.
#define GAME_MOVE_PARTIAL_WINDOW_Y 40
#define GAME_MOVE_PARTIAL_WINDOW_H 72

static void drawGameMovePartial(const ClockState &state) {
  if (!state.hasGame) return;

  display.setPartialWindow(0, GAME_MOVE_PARTIAL_WINDOW_Y, SCREEN_WIDTH, GAME_MOVE_PARTIAL_WINDOW_H);
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setTextColor(GxEPD_BLACK);
    // The static divider line, the horizontal separator, and (per the
    // 8-alignment note above) the top of the site URL all fall within
    // this window - fillScreen() just erased whatever was there, so all
    // three need redrawing, same as the full redraw draws them
    // (drawGameContent()/drawBottomBar()). The vertical line's real
    // extent stops AT FOOTER_LINE_Y (matching drawGameContent()) even
    // though the window itself reaches further - redrawing past that
    // would extend it where it was never meant to be.
    display.drawFastVLine(GAME_DIVIDER_X, GAME_MOVE_PARTIAL_WINDOW_Y, FOOTER_LINE_Y - GAME_MOVE_PARTIAL_WINDOW_Y, GxEPD_BLACK);
    display.drawFastHLine(0, FOOTER_LINE_Y, SCREEN_WIDTH, GxEPD_BLACK);
    drawSiteUrl();

    int meIndex = myPlayerIndex(state);
    int rightIndex = (meIndex == -1) ? 1 : meIndex;
    int leftIndex = 1 - rightIndex;
    drawRatingAndClock(0, GAME_DIVIDER_X, state.players[leftIndex], state.activePlayerIndex == leftIndex);
    drawRatingAndClock(GAME_DIVIDER_X, SCREEN_WIDTH - GAME_DIVIDER_X, state.players[rightIndex], state.activePlayerIndex == rightIndex);

    char moveLabel[16];
    char moveCountStr[24];
    bottomBarMoveContent(state, moveLabel, sizeof(moveLabel), moveCountStr, sizeof(moveCountStr));
    char note[40];
    snprintf(note, sizeof(note), "%s%s", moveLabel, moveCountStr);
    printSmallRightAligned(note, BOTTOM_SEP_Y - FOOTER_NOTE_GAP + FOOTER_Y_ADJUST);
  } while (display.nextPage());
}

static void updateDisplay(bool fullRefresh, const ClockState &state) {
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
  } else if (isLogoPhaseNow(state)) {
    drawLogoContent();
  } else {
    drawWaitingStatusContent(state);
  }

  display.display(!fullRefresh);
}

// ---------------------------------------------------------------------------
// Display task: owns 'display' exclusively. A length-1 FreeRTOS queue
// with xQueueOverwrite() is the hand-off - "always keep just the latest
// request, silently replace whatever's pending" - so if the game state
// changes again before this task gets around to rendering the previous
// request, that stale one is simply gone and the NEXT thing rendered is
// always the most current state, never something already out of date by
// the time it would appear on screen. No mutex anywhere: every writer of
// ClockState (GameDataSource.cpp, LiveGameClient.cpp,
// LichessLiveClient.cpp, BatteryFunctions.cpp, AdminPortal.cpp's
// updateWiFiStatus(), IvoChess_Clock.ino itself) already runs inside the
// SAME main loop() task - requestDisplayUpdate() below is called from
// that same single-threaded context, so the snapshot copy it makes can
// never race with any of those writers. This task only ever touches its
// own local copy pulled off the queue, never the live ClockState.
// ---------------------------------------------------------------------------
struct DisplayUpdateRequest {
  ClockState state;
  bool fullRefresh;
  bool clockPartialOnly;  // when true, ignore fullRefresh/movePartialOnly and do a true partial-window redraw of just the active player's clock digits (drawGameClockPartial())
  bool movePartialOnly;   // when true, ignore fullRefresh and do a true partial-window redraw of everything a move can change (drawGameMovePartial()) - mutually exclusive with clockPartialOnly
};

static QueueHandle_t displayQueue = nullptr;

// True only right after a FULL (non-partial) render that actually drew the
// game screen - i.e. it's currently safe to layer a clock/move partial on
// top, because the rest of the screen is known to already show correct
// game content. A subtle race made this necessary: the queue is "latest
// wins" (xQueueOverwrite), which was perfectly safe when every request
// fully repainted the screen - skipping a superseded one never left
// anything stale. Now that clock/move requests only touch a small window,
// that's no longer true: a full refresh takes up to ~3.6s, so if it's
// still mid-render when the NEXT 1s tick queues a clock-partial request,
// that overwrite can bump a FULL refresh that was never actually shown
// yet - e.g. a phase change (burn-in logo -> game) - in favor of a
// partial one, which would then paint a small window on top of the STILL
// STALE old screen (reported live as leftover burn-in-screen garbage
// after a game started). Rather than trying to prevent that race on the
// producer side (which would mean blocking/rate-limiting the very
// non-blocking design this queue exists for), the consumer here just
// self-corrects: any partial request arriving while this is false gets
// upgraded to a real full refresh instead of trusting the assumption that
// doesn't currently hold.
static bool gameScreenBaseValid = false;

// The moveCount actually reflected on screen as of the last full or
// move-partial render (both of those redraw the "on move" triangle;
// drawGameClockPartial() never does). The SAME race gameScreenBaseValid
// guards against also applies one level down: a move-partial request
// (which would move the triangle) can itself get superseded in the
// "latest wins" queue by a LATER clock-tick request before the display
// task ever renders it, if a move happens while the task is still busy
// with something else. The clock-tick request still carries the
// up-to-date activePlayerIndex (state is always a fresh snapshot), so the
// digits DO show the new active player if rendered as a plain tick - just
// the triangle never moved, because a tick-only partial doesn't touch it.
// Comparing the tick request's moveCount against this catches exactly
// that case and upgrades it to a move-partial instead. Starts at -1 (no
// real game ever has a negative move count) so it can't accidentally
// match before anything has actually been rendered.
static int lastRenderedMoveCount = -1;

static void displayTaskMain(void *) {
  initDisplay();
  drawStartupScreen();

  DisplayUpdateRequest req;
  for (;;) {
    // Blocks here (no CPU spent) until requestDisplayUpdate() has
    // something for us - the whole point of moving this to its own
    // task: this wait, and the ~3.6s a real refresh below can take,
    // never delay the main loop task's networking/admin work again.
    if (xQueueReceive(displayQueue, &req, portMAX_DELAY) == pdTRUE) {
      if (!gameScreenBaseValid) {
        // Never trust ANY partial request until a full render has
        // actually confirmed the base on-screen content - see
        // gameScreenBaseValid's own comment.
        updateDisplay(true, req.state);
        gameScreenBaseValid = req.state.hasGame;
        lastRenderedMoveCount = req.state.moveCount;
      } else if (req.clockPartialOnly && req.state.moveCount == lastRenderedMoveCount) {
        // Safe: nothing that would move the "on move" triangle happened
        // since it was last confirmed correct on screen.
        drawGameClockPartial(req.state);
      } else if (req.clockPartialOnly || req.movePartialOnly) {
        // Either a genuine move update, or a clock-tick that arrived
        // with a moveCount that's moved on since our last confirmed
        // render - see lastRenderedMoveCount's comment above for why
        // that means a move-partial got superseded before ever
        // rendering. drawGameMovePartial() redraws everything a move can
        // change from the CURRENT state snapshot, so it's the correct
        // fix in both cases.
        drawGameMovePartial(req.state);
        lastRenderedMoveCount = req.state.moveCount;
      } else {
        updateDisplay(req.fullRefresh, req.state);
        gameScreenBaseValid = req.state.hasGame;
        lastRenderedMoveCount = req.state.moveCount;
      }
    }
  }
}

void startDisplayTask() {
  // Length 1: xQueueOverwrite() (used by requestDisplayUpdate()) requires
  // exactly this, and it's exactly the "latest wins" semantics wanted
  // here anyway - see the block comment above.
  displayQueue = xQueueCreate(1, sizeof(DisplayUpdateRequest));

  // Priority one below whatever the calling (main loop) task is
  // currently running at - not a hardcoded number, so this stays correct
  // regardless of what the Arduino core's default loop-task priority
  // actually is. Guarantees the scheduler always favors network/admin
  // work over display rendering when both are ready to run, layered on
  // top of the fact that GxEPD2's own busy-wait already yields roughly
  // every 1ms (verified in GxEPD2_EPD.cpp's _waitWhileBusy() - delay(1)
  // + yield() per poll) - this priority gap is belt-and-suspenders, not
  // load-bearing on its own.
  UBaseType_t callerPriority = uxTaskPriorityGet(NULL);
  UBaseType_t displayPriority = (callerPriority > 0) ? callerPriority - 1 : 0;

  // 8192 bytes: comfortably above the stock FreeRTOS example tasks'
  // 2048 (those do nothing but print), sized for GxEPD2's font rendering
  // (FreeMonoBold9/12/18pt7b) plus this file's largest locals
  // (char[128]-ish buffers in the drawing helpers). The main loop task
  // separately needed 16KB (see IvoChess_Clock.ino's
  // SET_LOOP_TASK_STACK_SIZE) for TLS/WiFi/HTTP work this task never
  // does, so a smaller stack here is expected, not a shortcut - worth
  // confirming with uxTaskGetStackHighWaterMark() during real hardware
  // testing rather than treated as certainly correct.
  xTaskCreate(displayTaskMain, "display", 8192, nullptr, displayPriority, nullptr);
}

void requestDisplayUpdate(bool fullRefresh, const ClockState &state) {
  if (displayQueue == nullptr) return;  // startDisplayTask() not called yet - shouldn't happen, defensive only
  DisplayUpdateRequest req;
  req.state = state;  // plain struct copy, no pointers in ClockState - cheap and safe across the task boundary
  req.fullRefresh = fullRefresh;
  req.clockPartialOnly = false;
  req.movePartialOnly = false;
  xQueueOverwrite(displayQueue, &req);
}

void requestGameClockPartialRefresh(const ClockState &state) {
  if (displayQueue == nullptr) return;
  DisplayUpdateRequest req;
  req.state = state;
  req.fullRefresh = false;  // unused when clockPartialOnly is set
  req.clockPartialOnly = true;
  req.movePartialOnly = false;
  xQueueOverwrite(displayQueue, &req);
}

void requestGameMovePartialRefresh(const ClockState &state) {
  if (displayQueue == nullptr) return;
  DisplayUpdateRequest req;
  req.state = state;
  req.fullRefresh = false;  // unused when movePartialOnly is set
  req.clockPartialOnly = false;
  req.movePartialOnly = true;
  xQueueOverwrite(displayQueue, &req);
}
