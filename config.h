#ifndef CONFIG_H
#define CONFIG_H

// ---------------------------------------------------------------------------
// Default/fallback values - only used BEFORE anything is saved in flash
// (NVS). Once you fill in the admin page once, whatever is saved there
// takes over. Leave these blank - the admin page (hotspot or IP) is the
// right place to fill this in, not this file.
// ---------------------------------------------------------------------------
#define DEFAULT_WIFI_SSID     ""
#define DEFAULT_WIFI_PASSWORD ""
#define DEFAULT_PHPSESSID     ""

#define SETUP_AP_NAME "IvoChess-Setup"
#define WIFI_CONNECT_TIMEOUT_SECONDS 60
#define MDNS_HOSTNAME "ivochess"  // also reachable at http://ivochess.local/

#define PREFS_NAMESPACE "ivochess"
#define PREFS_KEY_PHPSESSID "phpsessid"
#define PHPSESSID_MAX_LEN 256
#define WIFI_SSID_MAX_LEN 33
#define WIFI_PASS_MAX_LEN 65

// ---------------------------------------------------------------------------
// DISPLAY (Waveshare 2.13" e-Paper V4, GxEPD2_213_B74 driver)
// ---------------------------------------------------------------------------
#define SCREEN_WIDTH  250
#define SCREEN_HEIGHT 122
#define DISPLAY_ROTATION 1  // confirmed by real hardware test - don't change without reason

#define EPD_CS    D9
#define EPD_DC    D4
#define EPD_RST   D3
#define EPD_BUSY  D5
#define SRAM_CS   -1

#define BATTERY_PIN D1
#define LED_PIN     D7
#define CONFIG_RESET_BUTTON_PIN D6  // optional external button (not the board's physical BOOT button)

#define FULL_REFRESH_INTERVAL_MS (5UL * 60UL * 1000UL)

// Alternates between the logo screen (clean) and the status screen every
// so often, to avoid e-paper burn-in.
#define SCREEN_CYCLE_INTERVAL_MS (30UL * 1000UL)

// ---------------------------------------------------------------------------
// CHESS.COM API
// ---------------------------------------------------------------------------
#define GAMES_ENDPOINT_URL "https://www.chess.com/service/play/games"
#define GAME_POLL_INTERVAL_MS 5000

// RSocket (live game data over WebSocket) - values match what the
// validated Python prototype used against the real chess.com server.
#define RSOCKET_KEEPALIVE_MS 8000
#define RSOCKET_LIFETIME_MS 90000

// How long the "game over" result stays on screen before returning to
// the waiting/status screen. DEFAULT only - configurable in the admin
// portal from then on (AdminPortal.cpp resultDisplayDurationMs).
#define DEFAULT_RESULT_DISPLAY_DURATION_SEC 15

// While a game is live: how often to redraw the clocks (unless a new
// move happens first, which also triggers an immediate redraw and
// resets this countdown - see IvoChess_Clock.ino). Real partial-window
// refresh caused visual corruption on real hardware (see
// updateGameClocksPartial() in DisplayFunctions.cpp), so for now this
// does a full-screen redraw instead of a true partial one.
#define GAME_CLOCK_REFRESH_INTERVAL_MS (10UL * 1000UL)

// ---------------------------------------------------------------------------
// ADMIN - a SINGLE portal/server, reachable via hotspot OR the normal
// network IP (not two separate systems). Standard HTTP port.
// ---------------------------------------------------------------------------
#define ADMIN_PORT 80

// ---------------------------------------------------------------------------
// LEDs (WS2812/NeoPixel) - all 8 LEDs always show the same color together
// (the case diffuses the light, individual LEDs aren't visible - see
// LedFunctions.cpp). Colors/brightness below are only the DEFAULTS seeded
// into Preferences the first time; from then on the admin portal's saved
// values (AdminPortal.cpp ledColor*/ledBrightness*) are what's actually
// used - edit them there, not here, once the device has booted once.
// ---------------------------------------------------------------------------
#define LED_COUNT 8
#define LED_DATA_PIN LED_PIN
#define DEFAULT_LED_BRIGHTNESS 100
#define DEFAULT_LED_BRIGHTNESS_NIGHT 20

#define DEFAULT_LED_NO_WIFI        "#0000ff"  // blue
#define DEFAULT_LED_LOW_BATTERY    "#ff8000"  // orange
#define DEFAULT_LED_WON            "#00ff00"  // green
#define DEFAULT_LED_LOST           "#ff1493"  // pink
#define DEFAULT_LED_DRAW           "#ffffff"  // white - also used for "who's on move" when that's not known yet
#define DEFAULT_LED_MY_TURN        "#0080ff"  // cyan
#define DEFAULT_LED_OPPONENT_TURN  "#ffff00"  // yellow

// ---------------------------------------------------------------------------
// BATTERY
// ---------------------------------------------------------------------------
#define BATTERY_FULL  4.2f
#define BATTERY_EMPTY 3.2f
#define VOLTAGE_DIVIDER_RATIO 0.395f
#define ADC_REF_VOLTAGE 3.3f
#define ADC_RESOLUTION 4095.0f

// ---------------------------------------------------------------------------
// TIME (NTP) - only used for the LEDs' night mode
// ---------------------------------------------------------------------------
#define NTP_SERVER "pool.ntp.org"
#define GMT_OFFSET_SEC (-3 * 3600)
#define DAYLIGHT_OFFSET_SEC 0
#define NIGHT_MODE_START_HOUR 22
#define NIGHT_MODE_END_HOUR   7

#endif  // CONFIG_H
