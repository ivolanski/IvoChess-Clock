#ifndef CONFIG_H
#define CONFIG_H

// ---------------------------------------------------------------------------
// FIRMWARE VERSION - shown in the webadmin and compared against GitHub's
// latest release tag (AdminPortal.cpp's update-check) to flag when a newer
// build is available. Bump this (and tag/release to match - see
// N:\documents\AI\claude\CLAUDE.md's release procedure) with every push to
// master that a user would actually want to know about.
// ---------------------------------------------------------------------------
#define FIRMWARE_VERSION "2.0.3"

// ---------------------------------------------------------------------------
// Default/fallback values - only used BEFORE anything is saved in flash
// (NVS). Once you fill in the admin page once, whatever is saved there
// takes over. Leave these blank - the admin page (hotspot or IP) is the
// right place to fill this in, not this file.
// ---------------------------------------------------------------------------
#define DEFAULT_WIFI_SSID     ""
#define DEFAULT_WIFI_PASSWORD ""
#define DEFAULT_PHPSESSID     ""
#define DEFAULT_CHESSCOM_REMEMBERME ""

#define SETUP_AP_NAME "IvoChess-Setup"
#define WIFI_CONNECT_TIMEOUT_SECONDS 60
#define MDNS_HOSTNAME "ivochessclock"  // also reachable at http://ivochessclock.local/

#define PREFS_NAMESPACE "ivochess"
#define PREFS_KEY_PHPSESSID "phpsessid"
#define PHPSESSID_MAX_LEN 256
#define CHESSCOM_REMEMBERME_MAX_LEN 256
#define WIFI_SSID_MAX_LEN 33
#define WIFI_PASS_MAX_LEN 65

// HTTP Basic Auth on the admin portal itself - protects WiFi/session
// cookies/etc from anyone who can reach the device's IP (or the setup
// hotspot). Change these from the portal's own "Webadmin access" section
// once logged in with the defaults.
#define DEFAULT_WEBADMIN_USER "ivochess"
#define DEFAULT_WEBADMIN_PASS "checkmate"
#define WEBADMIN_USER_MAX_LEN 33
#define WEBADMIN_PASS_MAX_LEN 33

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

// ---------------------------------------------------------------------------
// PHYSICAL BUTTON (top of the clock) - momentary switch, D0 to GND, internal
// pull-up, no external resistor needed. Dual purpose depending on
// currentDataSource (see ButtonFunctions.cpp): resumes game search
// (chess.com/Lichess) or sends ChessConnect's buttonEvent ("press clock to
// transmit move" - see project_details/dgt3000-gateway-protocol.md).
// ---------------------------------------------------------------------------
#define BUTTON_PIN D0
#define BUTTON_DEBOUNCE_MS 40

// ---------------------------------------------------------------------------
// SPEAKER (bare passive driver, PWM via LEDC + a one-transistor amplifier -
// see project_details/ivochess_clock_2.0_resources/wiring_instructions.md).
// Melody format: comma-separated "freqHz:durationMs" pairs, freq=0 = rest.
// These are only the compiled-in DEFAULTS - the admin portal lets each event
// be overridden with a custom melody string, which may also be pasted in as
// an RTTTL ringtone (auto-detected - see SoundFunctions.cpp).
// ---------------------------------------------------------------------------
#define SPEAKER_PIN D2
// 320 (not 200) so a full-length RTTTL ringtone pasted from the internet -
// e.g. themes/full songs, not just short jingles - fits without truncation.
#define SOUND_MELODY_MAX_LEN 320

// LEDC resolution used for the speaker channel (both initSoundTask()'s
// ledcAttach() and SoundFunctions.cpp's volume-scaled duty writes must
// agree on this). Volume is implemented as PWM duty cycle: a square wave's
// loudness on a bare piezo is roughly proportional to its duty cycle, so
// 100% volume = the old fixed 50% duty (max loudness this hardware can put
// out - there's no amplifier to go louder than that), scaling down toward
// 0% = silent (mute).
#define SPEAKER_PWM_RESOLUTION_BITS 10
#define DEFAULT_SOUND_VOLUME 100

// The 1-100 volume slider (0 is a separate hard-mute case, see
// speakerTone() in SoundFunctions.cpp) is mapped onto a dB range instead
// of linearly onto PWM duty - human hearing perceives loudness roughly
// logarithmically, so a straight linear duty ramp makes every low
// setting sound disproportionately loud relative to its neighbor (e.g.
// 2% sounding about twice as loud as 1%), while the top of the slider
// barely changes anything. Spreading a fixed dB range evenly across all
// 100 steps instead makes every adjacent 1% step the same *ratio*
// louder than the last, all the way from 1% to 100% - what a real
// "audio taper" volume knob does. 100% always stays 0dB (today's
// unchanged max loudness); this only sets how quiet 1% is relative to
// that. More negative = quieter low end. Tune this if the curve doesn't
// feel right on real hardware.
#define SOUND_VOLUME_MIN_DB -40.0f

#define DEFAULT_SOUND_MOVE_OPPONENT "880:60"
#define DEFAULT_SOUND_MOVE_OWN      "659:60"
#define DEFAULT_SOUND_GAME_START    "523:100,659:100,784:100,1047:150"
#define DEFAULT_SOUND_CHECK         "1200:80,0:40,1200:80"
#define DEFAULT_SOUND_GAME_WIN      "523:120,659:120,784:120,1047:120,1319:250"
#define DEFAULT_SOUND_GAME_LOSS     "392:200,349:200,294:200,220:400"
#define DEFAULT_SOUND_GAME_DRAW     "440:150,440:150,440:300"
#define DEFAULT_SOUND_GAME_END      "659:120,523:250"

#define FULL_REFRESH_INTERVAL_MS (5UL * 60UL * 1000UL)

// Alternates between the logo screen (clean) and the status screen every
// so often, to avoid e-paper burn-in.
#define SCREEN_CYCLE_INTERVAL_MS (30UL * 1000UL)

// How long after a game was last active to keep SUPPRESSING the logo
// screen (state.lastGameActiveAt in ClockState.h) - a live-connection
// blip mid-game briefly drops hasGame while it reconnects
// (gameDataSourceFastTick() in GameDataSource.cpp), and that's not the
// same as genuinely idle: the game screen already refreshes on every
// clock tick, so it doesn't need anti-burn-in cycling, and showing the
// decorative logo during a reconnect blip looked like a frozen clock.
#define GAME_RECONNECT_GRACE_MS (2UL * 60UL * 1000UL)

// ---------------------------------------------------------------------------
// CHESS.COM API
// ---------------------------------------------------------------------------
#define GAMES_ENDPOINT_URL "https://www.chess.com/service/play/games"
#define GAME_POLL_INTERVAL_MS 5000

// If nobody starts a game for this long while the clock sits idle, stop
// polling /service/play/games entirely instead of hitting it every
// GAME_POLL_INTERVAL_MS forever - a clock left on 24/7 with no one
// playing has no business generating requests indefinitely. Resumed by a
// press of the physical button (see ButtonFunctions.cpp/
// GameDataSource.cpp's resumeGameSearch()) rather than a timer that quietly
// starts polling again on its own - the point is to force a conscious
// "yes, I'm about to play" action, not just a longer sleep. Used to require
// a full physical restart before the button existed; 5 minutes (rather than
// the old 10) since a button press is now a one-touch way to resume.
#define WAITING_FOR_GAME_TIMEOUT_MS (5UL * 60UL * 1000UL)

// ---------------------------------------------------------------------------
// LICHESS API (OAuth 2.0 + PKCE - see OAuthPkce.h/.cpp, LichessApiFunctions.h/.cpp)
// ---------------------------------------------------------------------------
// "Choose any unique client id" - Lichess explicitly supports unregistered
// public clients, no registration/approval step needed (unlike chess.com's
// OAuth, which is a separate pending application).
#define LICHESS_OAUTH_CLIENT_ID "ivochess-clock"
#define LICHESS_AUTHORIZE_URL "https://lichess.org/oauth"
#define LICHESS_TOKEN_URL "https://lichess.org/api/token"
#define LICHESS_OAUTH_SCOPE "board:play"
#define LICHESS_OAUTH_CALLBACK_PATH "/oauth/lichess/callback"

#define LICHESS_PLAYING_ENDPOINT_URL "https://lichess.org/api/account/playing"
// %s is the gameId, filled in at request time.
#define LICHESS_GAME_STREAM_URL_FMT "https://lichess.org/api/board/game/stream/%s"
#define LICHESS_GAME_POLL_INTERVAL_MS 5000  // matches GAME_POLL_INTERVAL_MS's cadence/reasoning

// Access tokens are long-lived (~1 year per Lichess's own docs) and
// Lichess does NOT support refresh tokens at all - so unlike
// CHESSCOM_REMEMBERME_MAX_LEN there is no separate "renewal token" size
// to define here, just the one bearer token.
#define LICHESS_TOKEN_MAX_LEN 256
#define DEFAULT_LICHESS_TOKEN ""

// RSocket (live game data over WebSocket) - values match what the
// validated Python prototype used against the real chess.com server.
#define RSOCKET_KEEPALIVE_MS 8000
#define RSOCKET_LIFETIME_MS 90000

// How long the "game over" result stays on screen before returning to
// the waiting/status screen. DEFAULT only - configurable in the admin
// portal from then on (AdminPortal.cpp resultDisplayDurationMs).
#define DEFAULT_RESULT_DISPLAY_DURATION_SEC 15

// While a game is live: how often to redraw the clocks (unless a new
// move happens first, which also triggers an immediate redraw and resets
// this countdown - see IvoChess_Clock.ino). Matches the outer 1s gate
// updateGameData()'s caller already runs behind, so this is effectively
// "every time that gate opens" - a true per-second ticking clock. Made
// possible by drawGameClockPartial() (DisplayFunctions.cpp), a real
// partial-window refresh - a first attempt at this corrupted the display
// on real hardware (see git history, commit e470d66), root-caused and
// fixed (see that function's own comment) and validated on real hardware
// via tests/epaper_partial_refresh_poc/ before being ported into the
// real game screen.
#define GAME_CLOCK_REFRESH_INTERVAL_MS (1UL * 1000UL)

// e-paper "ghosting" (faint traces of previous digits) builds up over
// repeated partial refreshes of the same region - expected panel
// behavior, not a bug. GAME_CLOCK_PARTIAL_REFRESH_MAX_STREAK bounds it: a
// full refresh is forced after this many consecutive clock-only partial
// ticks, regardless of FULL_REFRESH_INTERVAL_MS's 5-minute watchdog
// above (which alone would let a slow-thinking player's move accumulate
// hundreds of uncorrected partial ticks). Manufacturer guidance for this
// panel is roughly every 5-10 partial updates; validated on real hardware
// up to 60 with no visible ghosting. In normal play this rarely even
// matters - every actual move already forces a full redraw via
// needsFullRefresh in IvoChess_Clock.ino, which resets this streak too.
#define GAME_CLOCK_PARTIAL_REFRESH_MAX_STREAK 60

// A full e-paper refresh isn't instant - by the time it actually finishes
// and the new frame is visible, this much real time has already passed
// since the value being drawn was computed. Only matters for the LOCAL
// extrapolation fallback (IvoChess_Clock.ino, between real RSocket
// updates) - it consistently showed ~2s behind the real clock, not
// growing over time, which is exactly this fixed draw latency, not
// drift. A real RSocket clock value is always exact and must be shown
// as-is - this offset is never applied to it, only to the extrapolated
// fallback.
//
// Still valid after display rendering moved to its own FreeRTOS task
// (DisplayFunctions.cpp's startDisplayTask()/requestDisplayUpdate()):
// this measures "decision to visible" time, which is essentially
// unchanged for the common case (the display task is idle, waiting on
// its queue, the moment a request arrives) - the difference is that
// loop() itself no longer blocks for that same window.
#define DISPLAY_REFRESH_LATENCY_MS 2000

// Same idea as DISPLAY_REFRESH_LATENCY_MS above, but for the (now much
// more common) case where the periodic clock tick goes through
// requestGameClockPartialRefresh() instead of a full redraw - a true
// partial-window refresh measured ~500ms "decision to visible" on real
// hardware (tests/epaper_partial_refresh_poc/), vs ~2-3.6s for a full
// one, so reusing the full-refresh latency here would over-correct the
// extrapolated clock by roughly a second and a half for no reason.
#define DISPLAY_REFRESH_LATENCY_PARTIAL_MS 500

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
#define DEFAULT_LED_BRIGHTNESS 200
#define DEFAULT_LED_BRIGHTNESS_NIGHT 100
#define LOW_BATTERY_THRESHOLD_PERCENT 15

#define DEFAULT_LED_NO_WIFI        "#800080"  // purple
#define DEFAULT_LED_LOW_BATTERY    "#ff0000"  // red
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
