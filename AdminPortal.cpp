#include "AdminPortal.h"
#include "config.h"
#include "Translations.h"
#include "GameDataSource.h"
#include "ChessApiFunctions.h"
#include "LichessApiFunctions.h"
#include "OAuthPkce.h"
#include "Favicon.h"
#include "LedFunctions.h"

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

char phpsessid[PHPSESSID_MAX_LEN] = DEFAULT_PHPSESSID;
char chessComRememberMe[CHESSCOM_REMEMBERME_MAX_LEN] = DEFAULT_CHESSCOM_REMEMBERME;
char myUsername[USERNAME_MAX_LEN] = "";
char lichessToken[LICHESS_TOKEN_MAX_LEN] = DEFAULT_LICHESS_TOKEN;
char lichessUsername[USERNAME_MAX_LEN] = "";

uint8_t ledColorNoWifi[3];
uint8_t ledColorLowBattery[3];
uint8_t ledColorWon[3];
uint8_t ledColorLost[3];
uint8_t ledColorDraw[3];
uint8_t ledColorMyTurn[3];
uint8_t ledColorOpponentTurn[3];
uint8_t ledBrightnessDay = DEFAULT_LED_BRIGHTNESS;
uint8_t ledBrightnessNight = DEFAULT_LED_BRIGHTNESS_NIGHT;
uint16_t ledCount = LED_COUNT;
unsigned long resultDisplayDurationMs = DEFAULT_RESULT_DISPLAY_DURATION_SEC * 1000UL;

bool soundEnabled[SOUND_EVENT_COUNT];
char soundMelodyOverride[SOUND_EVENT_COUNT][SOUND_MELODY_MAX_LEN];
uint8_t soundVolume = DEFAULT_SOUND_VOLUME;

// Set by checkForFirmwareUpdate() (called periodically from loop() - see
// its own comment), read by pageHead() to show the "Update available"
// banner. Not persisted - re-checked fresh every boot/interval.
bool updateAvailable = false;
char latestVersionTag[24] = "";

// Short NVS-key suffix + webadmin label per event, indexed the same as
// soundEnabled[]/soundMelodyOverride[] - the one place that ties
// SoundEvent values to their on-disk/on-page identity, so adding an event
// later means touching only this table plus config.h's DEFAULT_SOUND_*.
struct SoundEventMeta {
  const char *key;    // NVS key suffix (kept short - Preferences keys cap at 15 chars) and the webadmin form field/route suffix
  const char *label;
};
static const SoundEventMeta SOUND_EVENT_META[SOUND_EVENT_COUNT] = {
  {"start", "Game started"},
  {"mvopp", "Opponent's move"},
  {"mvown", "Your move"},
  {"check", "Check (ChessConnect only)"},
  {"win",   "You won"},
  {"loss",  "You lost"},
  {"draw",  "Draw"},
  {"end",   "Game ended (result unknown)"},
};

static char wifiSSID[WIFI_SSID_MAX_LEN] = DEFAULT_WIFI_SSID;
static char wifiPassword[WIFI_PASS_MAX_LEN] = DEFAULT_WIFI_PASSWORD;
static char webAdminUser[WEBADMIN_USER_MAX_LEN] = DEFAULT_WEBADMIN_USER;
static char webAdminPass[WEBADMIN_PASS_MAX_LEN] = DEFAULT_WEBADMIN_PASS;

static Preferences prefs;
static WebServer server(ADMIN_PORT);
static DNSServer dnsServer;
static ClockState *g_state = nullptr;
static bool apMode = false;

// The PKCE verifier+state for the current in-flight "Connect to Lichess"
// attempt - RAM-only, generated once and reused across renders until
// handleLichessOAuthCallback() consumes it (success or failure), NOT
// regenerated on every page load - see the render site (handleRoot())
// for why that distinction matters (a naive "fresh pair every render"
// approach broke in practice: the browser's automatic /favicon.ico
// request re-renders this same page in the background and would
// silently invalidate the pending exchange before the user's actual
// click completes). Not persisted across a reboot: the whole flow
// (render link -> browser goes to Lichess -> consent -> browser
// redirected back here) is expected to complete within the same boot.
static PkceExchange pendingLichessPkce;
static bool havePendingLichessPkce = false;

// ---- small "#rrggbb" <-> {r,g,b} helpers, used for the LED color pickers ----
static void hexToRgb(const String &hex, uint8_t out[3]) {
  if (hex.length() != 7 || hex[0] != '#') {
    out[0] = out[1] = out[2] = 0;
    return;
  }
  out[0] = (uint8_t)strtoul(hex.substring(1, 3).c_str(), nullptr, 16);
  out[1] = (uint8_t)strtoul(hex.substring(3, 5).c_str(), nullptr, 16);
  out[2] = (uint8_t)strtoul(hex.substring(5, 7).c_str(), nullptr, 16);
}

static String rgbToHex(const uint8_t rgb[3]) {
  char buf[8];
  snprintf(buf, sizeof(buf), "#%02x%02x%02x", rgb[0], rgb[1], rgb[2]);
  return String(buf);
}

static void loadConfig() {
  prefs.begin(PREFS_NAMESPACE, /*readOnly=*/true);
  prefs.getString(PREFS_KEY_PHPSESSID, DEFAULT_PHPSESSID).toCharArray(phpsessid, sizeof(phpsessid));
  prefs.getString("remember_me", DEFAULT_CHESSCOM_REMEMBERME).toCharArray(chessComRememberMe, sizeof(chessComRememberMe));
  prefs.getString("my_username", "").toCharArray(myUsername, sizeof(myUsername));
  prefs.getString("lichess_tok", DEFAULT_LICHESS_TOKEN).toCharArray(lichessToken, sizeof(lichessToken));
  prefs.getString("lichess_usr", "").toCharArray(lichessUsername, sizeof(lichessUsername));
  prefs.getString("wifi_ssid", DEFAULT_WIFI_SSID).toCharArray(wifiSSID, sizeof(wifiSSID));
  prefs.getString("wifi_pass", DEFAULT_WIFI_PASSWORD).toCharArray(wifiPassword, sizeof(wifiPassword));
  prefs.getString("admin_user", DEFAULT_WEBADMIN_USER).toCharArray(webAdminUser, sizeof(webAdminUser));
  prefs.getString("admin_pass", DEFAULT_WEBADMIN_PASS).toCharArray(webAdminPass, sizeof(webAdminPass));
  currentLanguage = (Language)prefs.getInt("lang", LANG_EN);
  currentDataSource = (DataSourceType)prefs.getInt("datasrc", DATA_SOURCE_CHESSCOM_WIFI);

  hexToRgb(prefs.getString("led_nowifi", DEFAULT_LED_NO_WIFI), ledColorNoWifi);
  hexToRgb(prefs.getString("led_lowbatt", DEFAULT_LED_LOW_BATTERY), ledColorLowBattery);
  hexToRgb(prefs.getString("led_won", DEFAULT_LED_WON), ledColorWon);
  hexToRgb(prefs.getString("led_lost", DEFAULT_LED_LOST), ledColorLost);
  hexToRgb(prefs.getString("led_draw", DEFAULT_LED_DRAW), ledColorDraw);
  hexToRgb(prefs.getString("led_myturn", DEFAULT_LED_MY_TURN), ledColorMyTurn);
  hexToRgb(prefs.getString("led_oppturn", DEFAULT_LED_OPPONENT_TURN), ledColorOpponentTurn);
  ledBrightnessDay = (uint8_t)prefs.getUInt("led_bright_day", DEFAULT_LED_BRIGHTNESS);
  ledBrightnessNight = (uint8_t)prefs.getUInt("led_bright_night", DEFAULT_LED_BRIGHTNESS_NIGHT);
  ledCount = (uint16_t)prefs.getUInt("led_count", LED_COUNT);
  setLedCount(ledCount);  // applies the saved strip length now that initLEDs() has already run (setup()'s ordering - see IvoChess_Clock.ino)

  resultDisplayDurationMs = (unsigned long)prefs.getUInt("result_dur_s", DEFAULT_RESULT_DISPLAY_DURATION_SEC) * 1000UL;

  soundVolume = (uint8_t)prefs.getUInt("snd_volume", DEFAULT_SOUND_VOLUME);

  for (int i = 0; i < SOUND_EVENT_COUNT; i++) {
    char enKey[16], melKey[16];
    snprintf(enKey, sizeof(enKey), "snd_en_%s", SOUND_EVENT_META[i].key);
    snprintf(melKey, sizeof(melKey), "snd_mel_%s", SOUND_EVENT_META[i].key);
    soundEnabled[i] = prefs.getBool(enKey, true);  // all events on by default
    prefs.getString(melKey, "").toCharArray(soundMelodyOverride[i], SOUND_MELODY_MAX_LEN);
  }

  prefs.end();
}

static void saveConfig() {
  prefs.begin(PREFS_NAMESPACE, /*readOnly=*/false);
  prefs.putString(PREFS_KEY_PHPSESSID, phpsessid);
  prefs.putString("remember_me", chessComRememberMe);
  prefs.putString("my_username", myUsername);
  prefs.putString("lichess_tok", lichessToken);
  prefs.putString("lichess_usr", lichessUsername);
  prefs.putString("wifi_ssid", wifiSSID);
  prefs.putString("wifi_pass", wifiPassword);
  prefs.putString("admin_user", webAdminUser);
  prefs.putString("admin_pass", webAdminPass);
  prefs.putInt("lang", (int)currentLanguage);
  prefs.putInt("datasrc", (int)currentDataSource);

  prefs.putString("led_nowifi", rgbToHex(ledColorNoWifi));
  prefs.putString("led_lowbatt", rgbToHex(ledColorLowBattery));
  prefs.putString("led_won", rgbToHex(ledColorWon));
  prefs.putString("led_lost", rgbToHex(ledColorLost));
  prefs.putString("led_draw", rgbToHex(ledColorDraw));
  prefs.putString("led_myturn", rgbToHex(ledColorMyTurn));
  prefs.putString("led_oppturn", rgbToHex(ledColorOpponentTurn));
  prefs.putUInt("led_bright_day", ledBrightnessDay);
  prefs.putUInt("led_bright_night", ledBrightnessNight);
  prefs.putUInt("led_count", ledCount);

  prefs.putUInt("result_dur_s", (unsigned int)(resultDisplayDurationMs / 1000UL));

  prefs.putUInt("snd_volume", soundVolume);

  for (int i = 0; i < SOUND_EVENT_COUNT; i++) {
    char enKey[16], melKey[16];
    snprintf(enKey, sizeof(enKey), "snd_en_%s", SOUND_EVENT_META[i].key);
    snprintf(melKey, sizeof(melKey), "snd_mel_%s", SOUND_EVENT_META[i].key);
    prefs.putBool(enKey, soundEnabled[i]);
    prefs.putString(melKey, soundMelodyOverride[i]);
  }

  prefs.end();
}

void persistSessionCookies() {
  prefs.begin(PREFS_NAMESPACE, /*readOnly=*/false);
  prefs.putString(PREFS_KEY_PHPSESSID, phpsessid);
  prefs.putString("remember_me", chessComRememberMe);
  prefs.end();
}

static const char *wifiQualityLabel(bool connected, int rssi) {
  if (!connected) return T(STR_DISCONNECTED);
  if (rssi > -60) return T(STR_EXCELLENT);
  if (rssi > -75) return T(STR_GOOD);
  return T(STR_POOR);
}

// Shared page chrome: dark-mode-aware, no external assets (everything
// inline - this is served straight off the ESP32, no internet access
// needed to load it, including on the setup hotspot with no WiFi at all).
static const char PAGE_STYLE[] =
    "<style>"
    ":root{--bg:#f4f5f7;--card:#ffffff;--text:#1c1e21;--muted:#6b7280;--border:#e2e4e9;--accent:#2563eb;--accent-text:#ffffff;--ok:#16a34a;--warn:#d97706;--bad:#dc2626;}"
    "@media (prefers-color-scheme: dark){:root{--bg:#15161a;--card:#1f2126;--text:#e7e9ea;--muted:#9aa0a8;--border:#33363d;--accent:#3b82f6;--accent-text:#ffffff;--ok:#4ade80;--warn:#fbbf24;--bad:#f87171;}}"
    "*{box-sizing:border-box;}"
    "body{font-family:-apple-system,Segoe UI,Roboto,Helvetica,Arial,sans-serif;background:var(--bg);color:var(--text);margin:0;padding:16px;max-width:480px;margin-left:auto;margin-right:auto;}"
    "h1{font-size:1.3rem;margin:0 0 4px;}"
    "h2{font-size:0.95rem;text-transform:uppercase;letter-spacing:.04em;color:var(--muted);margin:22px 0 8px;}"
    ".sub{color:var(--muted);font-size:0.85rem;margin-bottom:18px;}"
    ".card{background:var(--card);border:1px solid var(--border);border-radius:12px;padding:14px 16px;margin-bottom:10px;}"
    ".row{display:flex;justify-content:space-between;align-items:center;padding:4px 0;font-size:0.9rem;}"
    ".row .v{color:var(--muted);}"
    "label{display:block;font-size:0.85rem;color:var(--muted);margin:10px 0 4px;}"
    "input[type=text],input[type=password],input[type=number],select{width:100%;padding:10px;border-radius:8px;border:1px solid var(--border);background:var(--bg);color:var(--text);font-size:0.95rem;}"
    "input[type=range]{width:100%;}"
    "input[type=color]{width:100%;height:40px;padding:2px;border-radius:8px;border:1px solid var(--border);background:var(--bg);}"
    ".colorrow{display:flex;align-items:center;gap:10px;margin:10px 0;}"
    ".colorrow label{margin:0;flex:1;}"
    ".colorrow input[type=color]{width:56px;height:36px;flex:none;}"
    "small{color:var(--muted);}"
    "a{color:var(--accent);}"
    "a:visited{color:var(--accent);}"
    "button,input[type=submit]{width:100%;padding:12px;border-radius:10px;border:none;background:var(--accent);color:var(--accent-text);font-size:1rem;font-weight:600;margin-top:18px;cursor:pointer;}"
    "button:active,input[type=submit]:active{opacity:.85;}"
    ".pill{display:inline-block;padding:2px 8px;border-radius:999px;font-size:0.75rem;font-weight:600;}"
    ".pill.ok{background:rgba(22,163,74,.15);color:var(--ok);}"
    ".pill.warn{background:rgba(217,119,6,.15);color:var(--warn);}"
    ".pill.bad{background:rgba(220,38,38,.15);color:var(--bad);}"
    ".tabs{display:flex;border-bottom:1px solid var(--border);margin-bottom:18px;}"
    ".tabs a{flex:1;text-align:center;padding:10px 2px;font-size:0.8rem;font-weight:600;color:var(--muted);"
    "text-decoration:none;border-bottom:2px solid transparent;margin-bottom:-1px;}"
    ".tabs a.active{color:var(--accent);border-bottom-color:var(--accent);}"
    "</style>";

// Test/Reset buttons for Sound and LED colors used to be formaction='...'
// submit buttons - simplest to write, but every click submitted (and thus
// reloaded) the WHOLE settings form, discarding any other field the user
// had typed but not yet saved (BUG FOUND VIA USER REPORT: type a new
// melody, click Test on a DIFFERENT event, the melody you were about to
// save is gone - the reload re-rendered the page from the last SAVED
// state). These fetch() calls hit the same /sound/test, /sound/reset,
// /led/test routes but never navigate the page at all, so nothing typed
// anywhere else on the form is ever at risk. resetSound() clears the
// now-blank field locally too, without a round trip, since the server
// already knows to fall back to the default once the override is cleared.
static const char PAGE_SCRIPT[] =
    "<script>"
    "function postForm(url,params){"
    "var pairs=[];"
    "for(var k in params){pairs.push(encodeURIComponent(k)+'='+encodeURIComponent(params[k]));}"
    "return fetch(url,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:pairs.join('&')});"
    "}"
    "function testSound(key){"
    "var mel=document.getElementById('mel_'+key).value;"
    "var params={sndevt:key};"
    "params['snd_mel_'+key]=mel;"
    "postForm('/sound/test',params);"
    "}"
    "function resetSound(key){"
    "postForm('/sound/reset',{sndevt:key}).then(function(){"
    "document.getElementById('mel_'+key).value='';"
    "});"
    "}"
    "function testLeds(){"
    "postForm('/led/test',{"
    "led_nowifi:document.getElementById('led_nowifi').value,"
    "led_lowbatt:document.getElementById('led_lowbatt').value,"
    "led_myturn:document.getElementById('led_myturn').value,"
    "led_oppturn:document.getElementById('led_oppturn').value,"
    "led_won:document.getElementById('led_won').value,"
    "led_lost:document.getElementById('led_lost').value,"
    "led_draw:document.getElementById('led_draw').value"
    "});"
    "}"
    "</script>";

// HTTP Basic Auth, checked on every admin route - guards WiFi
// credentials/session cookies from anyone who can just reach the
// device's IP (or the setup hotspot, which uses the same defaults until
// changed). Returns false (and already sent the 401 challenge) if the
// request isn't authenticated yet - callers must return immediately.
static bool checkAuth() {
  if (!server.authenticate(webAdminUser, webAdminPass)) {
    server.requestAuthentication(BASIC_AUTH, "IvoChess Clock");
    return false;
  }
  return true;
}

// Built from whatever host the browser actually used to reach this page
// (server.hostHeader() - the mDNS name, a raw IP, whatever), NOT
// hardcoded to MDNS_HOSTNAME. PKCE requires the exact same redirect_uri
// string in both the authorize URL and the token exchange - if the user
// loaded the page via IP but this were hardcoded to the mDNS name (or
// vice versa), the token exchange would fail with an opaque
// "invalid_grant" and nothing here would explain why.
static String lichessRedirectUri() {
  return "http://" + server.hostHeader() + LICHESS_OAUTH_CALLBACK_PATH;
}

// The webadmin used to be one single giant page/form - split into 4 tabs
// (main/connections/sounds/leds, each its own route and its own <form
// action='/save'>) once it got unwieldy on a phone. handleSave() stays a
// SINGLE shared handler for all of them (its per-field hasArg() guards
// already tolerate a POST body that only contains one tab's fields - see
// its own comments), it just needs to know which tab's page to redirect
// back to afterward, which is what the hidden 'tab' field (added by
// pageFoot() below) is for.
static String tabNav(const char *active) {
  String a = active;
  String html = "<div class='tabs'>";
  html += "<a href='/' class='" + String(a == "main" ? "active" : "") + "'>Main</a>";
  html += "<a href='/connections' class='" + String(a == "connections" ? "active" : "") + "'>Connections</a>";
  html += "<a href='/sounds' class='" + String(a == "sounds" ? "active" : "") + "'>Sounds</a>";
  html += "<a href='/leds' class='" + String(a == "leds" ? "active" : "") + "'>LEDs</a>";
  html += "</div>";
  return html;
}

static String tabPath(const String &tab) {
  if (tab == "connections") return "/connections";
  if (tab == "sounds") return "/sounds";
  if (tab == "leds") return "/leds";
  return "/";
}

// Head + status line + tab bar shared by all 4 tab pages. Callers still
// open their own <form method='POST' action='/save'> right after this -
// not folded in here since the LOGIN button needs to sit outside any
// <form> that includes it as a formaction target (see pageFoot()'s hidden
// 'tab' field comment for the matching close-out half).
static String pageHead(const char *activeTab) {
  String html = "<html><head><title>IvoChess Clock</title>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += PAGE_STYLE;
  html += "</head><body>";
  html += "<h1>IvoChess Clock <span style='font-size:0.6rem;font-weight:400;color:var(--muted)'>v" FIRMWARE_VERSION "</span></h1>";
  if (updateAvailable) {
    html += "<div class='row'><span class='pill warn'>Update available: " + String(latestVersionTag) +
            "</span></div><small>Flash the new version from the same page you used to flash this one - "
            "<a href='https://ivochessclock.com/build.html' target='_blank' rel='noopener'>ivochessclock.com/build.html</a></small>";
  }
  if (apMode) {
    html += "<div class='sub'>Setup hotspot <span class='pill warn'>not on a real network yet</span></div>";
  } else {
    html += "<div class='sub'>" + String(wifiSSID) + " &middot; " + String(wifiQualityLabel(true, WiFi.RSSI())) +
            " &middot; " + WiFi.localIP().toString() + " &middot; http://" + MDNS_HOSTNAME + ".local/</div>";
  }
  html += tabNav(activeTab);
  return html;
}

// Closes out the <form> opened by each tab page: a hidden field recording
// which tab this is (so handleSave() redirects back to the right one,
// instead of always bouncing to Main), the Save button, the test-button
// JS, and the closing tags.
static String pageFoot(const char *activeTab) {
  String html = "<input type='hidden' name='tab' value='" + String(activeTab) + "'>";
  html += "<input type='submit' value='Save'>";
  html += "</form>";
  html += PAGE_SCRIPT;
  html += "</body></html>";
  return html;
}

static void handleRoot() {
  if (!checkAuth()) return;

  String html = pageHead("main");
  html += "<form method='POST' action='/save'>";

  html += "<h2>WiFi</h2><div class='card'>";
  html += "<label>Network name (SSID)</label><input type='text' name='ssid' value='" + String(wifiSSID) + "'>";
  html += "<label>Password</label><input type='password' name='pass' placeholder='(leave empty to keep current)'>";
  html += "</div>";

  html += "<h2>Display</h2><div class='card'>";
  html += "<label>Language</label><select name='lang'>";
  html += "<option value='0'"; html += (currentLanguage == LANG_EN ? " selected" : ""); html += ">English</option>";
  html += "<option value='1'"; html += (currentLanguage == LANG_PT ? " selected" : ""); html += ">Portugues</option>";
  html += "</select>";
  html += "<label>Result screen duration (seconds)</label><input type='number' name='resultdur' min='3' max='120' value='" +
          String(resultDisplayDurationMs / 1000UL) + "'>";
  html += "</div>";

  html += "<h2>Webadmin access</h2><div class='card'>";
  html += "<label>Username</label><input type='text' name='adminuser' value='" + String(webAdminUser) + "' autocomplete='off'>";
  html += "<label>Password</label><input type='password' name='adminpass' placeholder='(leave empty to keep current)' autocomplete='new-password'>";
  html += "</div>";

  html += pageFoot("main");
  server.send(200, "text/html", html);
}

static void handleConnections() {
  if (!checkAuth()) return;

  String html = pageHead("connections");
  html += "<form method='POST' action='/save'>";

  html += "<h2>Chess.com account</h2><div class='card'>";
  // Tested live on every page load (a real request, not a cached guess) -
  // see testChessComSession(). Only actually run it when there's a WiFi
  // connection to run it over and a cookie worth testing - otherwise the
  // result would just be "No WiFi"/no-cookie noise dressed up as a broken
  // session.
  html += "<div class='row'>Session status ";
  if (apMode || WiFi.status() != WL_CONNECTED) {
    html += "<span class='pill warn'>No WiFi</span>";
  } else if (phpsessid[0] == '\0' && chessComRememberMe[0] == '\0') {
    html += "<span class='pill warn'>Not connected</span>";
  } else if (testChessComSession()) {
    html += "<span class='pill ok'>Valid</span>";
  } else {
    html += "<span class='pill bad'>Invalid - recapture cookie</span>";
  }
  html += "</div>";
  {
    // Surfaces the last time a renewal was actually REFUSED by chess.com
    // and CHESSCOM_REMEMBERME discarded because of it - persisted to
    // flash (getLastChessComSessionFailure()), so it's still visible here
    // even after a reboot wiped the Serial log that would otherwise be
    // the only record of it. Added after a real "worked all day, dead by
    // morning" recurrence (2026-08-11) that had no evidence left to
    // diagnose once the board was next power-cycled.
    char failReason[48];
    char failWhen[24];
    if (getLastChessComSessionFailure(failReason, sizeof(failReason), failWhen, sizeof(failWhen))) {
      html += "<div class='row'><small>Last drop: " + String(failReason) + " on " + String(failWhen) + "</small></div>";
    }
  }
  html += "<label>CHESSCOM_REMEMBERME cookie</label><input type='password' name='remembme' placeholder='(leave empty to keep current)' autocomplete='off'>";
  html += "<small>Step-by-step to get your remember me at: ivochessclock.com</small>";
  html += "<label style='margin-top:14px'>Your username</label><input type='text' name='myusername' value='" + String(myUsername) + "' placeholder='e.g. IVO-88'>";
  if (phpsessid[0] != '\0' || chessComRememberMe[0] != '\0') {
    // formaction/formmethod (HTML5), not a nested <form> - same reasoning
    // as the Lichess "Disconnect" button below. Separate from leaving the
    // cookie field empty on Save, which deliberately KEEPS the current
    // cookie - this is the explicit "throw it away" action instead.
    html += "<button type='submit' formaction='/chesscom/invalidate' formmethod='POST' style='margin-top:14px'>Forget cookie</button>";
  }
  html += "</div>";

  // Always rendered alongside the chess.com card, regardless of which
  // data source is currently active - switching datasrc back and forth
  // must never require reconnecting the OTHER service. handleSave()'s
  // per-field hasArg()+length>0 guards already guarantee this for form
  // fields; this card has no text fields to accidentally blank out in
  // the first place (connect = OAuth redirect, disconnect = its own
  // separate route), so there's nothing to lose here either way.
  html += "<h2>Lichess account</h2><div class='card'>";
  if (lichessToken[0] != '\0') {
    html += "<div class='row'>Connected as <span class='v'>" + String(lichessUsername) + "</span></div>";
    // formaction/formmethod (HTML5), NOT a nested <form> - this whole
    // block sits inside this tab's one <form action='/save'> (opened
    // above, closed by pageFoot() below). A <form> cannot legally nest
    // inside another one; an earlier version had exactly that, and
    // browsers respond to it by silently closing the OUTER form right
    // here, orphaning everything after it. formaction/formmethod is the
    // standards-correct way for one button in a shared form to submit to
    // a different endpoint.
    html += "<button type='submit' formaction='/oauth/lichess/disconnect' formmethod='POST' style='margin-top:10px'>Disconnect</button>";
  } else {
    // Reuse an already-pending PKCE pair instead of generating a new one
    // on every render. Regenerating unconditionally broke the flow in
    // practice: browsers auto-request /favicon.ico right after loading
    // any page, that request falls through onNotFound() to handleRoot()
    // (checkAuth() passes - same-origin, cached Basic Auth), and used to
    // silently overwrite the pending exchange with a second PKCE pair
    // moments after the real page (and the "Connect" link the user
    // actually sees and clicks) had already been sent with the first one
    // - guaranteeing a state mismatch on the callback. Only generating a
    // fresh pair when none is pending makes the link stay valid across
    // any number of incidental extra page loads (favicon, a second tab,
    // a reload) until it's actually used (success or failure both clear
    // havePendingLichessPkce - see handleLichessOAuthCallback()).
    if (!havePendingLichessPkce) {
      pkceGenerate(pendingLichessPkce);
      havePendingLichessPkce = true;
    }
    String authorizeUrl = lichessBuildAuthorizeUrl(pendingLichessPkce, lichessRedirectUri());
    html += "<a href='" + authorizeUrl + "' style='display:block;text-align:center;padding:12px;border-radius:10px;"
            "background:var(--accent);color:var(--accent-text);font-size:1rem;font-weight:600;"
            "text-decoration:none;margin-top:18px'>Connect to Lichess</a>";
    html += "<small>Opens Lichess's own login/consent page - the clock never sees your Lichess password.</small>";
  }
  html += "</div>";

  html += "<h2>Data source</h2><div class='card'>";
  html += "<select name='datasrc'>";
  html += "<option value='0'"; html += (currentDataSource == DATA_SOURCE_CHESSCOM_WIFI ? " selected" : ""); html += ">Chess.com (live, over WiFi)</option>";
  // No WiFi credentials to check here - ChessConnect is a BLE peripheral,
  // always selectable (see GameDataSource.cpp/ChessConnectBLE.cpp). Shows
  // "White"/"Black" instead of real names/ratings - this protocol never
  // discloses either.
  html += "<option value='1'"; html += (currentDataSource == DATA_SOURCE_CHESSCONNECT_BLE ? " selected" : ""); html += ">ChessConnect (Bluetooth, DGT3000 Gateway)</option>";
  // Only selectable once actually connected - picking a source with no
  // credentials would just show "No Lichess token set" (see
  // LichessApiFunctions.cpp's lichessFetchActiveGame()) instead of doing
  // anything useful.
  html += "<option value='2'";
  html += (currentDataSource == DATA_SOURCE_LICHESS_WIFI ? " selected" : "");
  html += (lichessToken[0] == '\0' ? " disabled" : "");
  html += ">Lichess (live, over WiFi)" + String(lichessToken[0] == '\0' ? " - connect above first" : "") + "</option>";
  html += "</select>";
  html += "</div>";

  html += pageFoot("connections");
  server.send(200, "text/html", html);
}

static void handleSounds() {
  if (!checkAuth()) return;

  String html = pageHead("sounds");
  html += "<form method='POST' action='/save'>";

  html += "<h2>Sound</h2><div class='card'>";
  html += "<small>Each event: enable/mute, and an optional custom melody. Paste in an "
          "<b>RTTTL</b> ringtone (e.g. <code>Nokia:d=4,o=5,b=125:8e6,8d6,2f#,2g#,8c#6,...</code>) - "
          "try the <a href='https://rtttl.skully.tech/' target='_blank' rel='noopener'>RTTTL composer</a> "
          "to make your own, or the <a href='https://ringtone.vulc.in/' target='_blank' rel='noopener'>"
          "ringtone library</a> for thousands of free ready-made melodies. Or use comma-separated "
          "<code>freqHz:durationMs</code> notes (e.g. <code>523:100,659:100,0:50,784:150</code>, "
          "<code>0</code> = silence/rest). Leave blank to use the built-in default shown as a placeholder.</small>";
  html += "<label>Volume - <span id='snd_volume_pct'>" + String(soundVolume) +
          "</span>% (0 = mute - handy if you're somewhere quiet, like a library)</label>";
  html += "<input type='range' id='snd_volume' name='snd_volume' min='0' max='100' value='" + String(soundVolume) +
          "' oninput=\"document.getElementById('snd_volume_pct').textContent=this.value\">";
  html += "<small>This is the loudest a bare piezo speaker driven straight off the ESP32 can go - there's no "
          "amplifier, so raising it above 100% isn't possible in software.</small>";
  for (int i = 0; i < SOUND_EVENT_COUNT; i++) {
    const SoundEventMeta &meta = SOUND_EVENT_META[i];
    html += "<div class='colorrow' style='align-items:flex-start;margin-top:16px'>";
    html += "<label style='flex:none;width:20px;margin-top:8px'><input type='checkbox' name='snd_en_" + String(meta.key) + "'";
    html += (soundEnabled[i] ? " checked" : "");
    html += "></label>";
    html += "<div style='flex:1'>";
    html += "<label style='margin:0 0 4px'>" + String(meta.label) + "</label>";
    html += "<input type='text' id='mel_" + String(meta.key) + "' name='snd_mel_" + String(meta.key) + "' value='" + String(soundMelodyOverride[i]) +
            "' placeholder='" + String(soundDefaultMelody((SoundEvent)i)) + "' maxlength='" +
            String(SOUND_MELODY_MAX_LEN - 1) + "'>";
    html += "<div style='display:flex;gap:8px;margin-top:6px'>";
    html += "<button type='button' onclick=\"testSound('" + String(meta.key) +
            "')\" style='padding:8px;font-size:0.8rem;font-weight:500'>&#9658; Test</button>";
    html += "<button type='button' onclick=\"resetSound('" + String(meta.key) +
            "')\" style='padding:8px;font-size:0.8rem;font-weight:500'>Reset to default</button>";
    html += "</div>";
    html += "</div></div>";
  }
  html += "</div>";

  html += pageFoot("sounds");
  server.send(200, "text/html", html);
}

static void handleLeds() {
  if (!checkAuth()) return;

  String html = pageHead("leds");
  html += "<form method='POST' action='/save'>";

  html += "<h2>LED colors</h2><div class='card'>";
  html += "<label>Number of LEDs</label><input type='number' id='led_count' name='led_count' min='1' max='60' value='" + String(ledCount) + "'>";
  html += "<small>Must match how many LEDs are actually wired on the strip - only matters if your build uses a "
          "different strip length than the default.</small>";
  html += "<div class='colorrow'><label>No WiFi</label><input type='color' id='led_nowifi' name='led_nowifi' value='" + rgbToHex(ledColorNoWifi) + "'></div>";
  html += "<div class='colorrow'><label>Low battery (blinks)</label><input type='color' id='led_lowbatt' name='led_lowbatt' value='" + rgbToHex(ledColorLowBattery) + "'></div>";
  html += "<div class='colorrow'><label>Your turn</label><input type='color' id='led_myturn' name='led_myturn' value='" + rgbToHex(ledColorMyTurn) + "'></div>";
  html += "<div class='colorrow'><label>Opponent's turn</label><input type='color' id='led_oppturn' name='led_oppturn' value='" + rgbToHex(ledColorOpponentTurn) + "'></div>";
  html += "<div class='colorrow'><label>You won</label><input type='color' id='led_won' name='led_won' value='" + rgbToHex(ledColorWon) + "'></div>";
  html += "<div class='colorrow'><label>You lost</label><input type='color' id='led_lost' name='led_lost' value='" + rgbToHex(ledColorLost) + "'></div>";
  html += "<div class='colorrow'><label>Draw / unknown</label><input type='color' id='led_draw' name='led_draw' value='" + rgbToHex(ledColorDraw) + "'></div>";
  html += "<label>Brightness - day (0-255)</label><input type='number' name='led_bright_day' min='0' max='255' value='" + String(ledBrightnessDay) + "'>";
  html += "<label>Brightness - night (0-255)</label><input type='number' name='led_bright_night' min='0' max='255' value='" + String(ledBrightnessNight) + "'>";
  html += "<button type='button' onclick='testLeds()' style='margin-top:10px;padding:10px;"
          "font-size:0.85rem;font-weight:500'>&#9658; Test LED colors</button>";
  html += "<small>Cycles the strip through each color above (as currently typed, even if not saved yet), "
          "about 1.5s each.</small>";
  html += "</div>";

  html += pageFoot("leds");
  server.send(200, "text/html", html);
}

static void handleSave() {
  if (!checkAuth()) return;

  bool wifiChanged = false;

  if (server.hasArg("ssid") && server.arg("ssid") != String(wifiSSID)) {
    server.arg("ssid").toCharArray(wifiSSID, sizeof(wifiSSID));
    wifiChanged = true;
  }
  if (server.hasArg("pass") && server.arg("pass").length() > 0) {
    server.arg("pass").toCharArray(wifiPassword, sizeof(wifiPassword));
    wifiChanged = true;
  }
  // Only CHESSCOM_REMEMBERME is asked for. PHPSESSID is derived from it
  // (GET /home with a valid remember-me answers 200 and issues a fresh
  // PHPSESSID via Set-Cookie - verified against the live server), and the
  // derivation only works in that direction, so the remember-me is the one
  // that actually has to come from the user.
  //
  // Having a single field also removes an entire failure mode by
  // construction: it used to be possible to paste a fresh PHPSESSID while
  // leaving a stale remember-me in place. That token is single-use, so
  // replaying one the server has already rotated past reads as a replayed
  // token rather than an expired one, and the usual response is to
  // invalidate every session on the account - killing the very cookie that
  // had just been pasted. With one field that combination cannot be
  // entered at all.
  bool sessionCookiesChanged = false;
  if (server.hasArg("remembme") && server.arg("remembme").length() > 0) {
    server.arg("remembme").toCharArray(chessComRememberMe, sizeof(chessComRememberMe));
    // Any PHPSESSID we were holding belongs to the previous token's
    // session. Clear it so the next poll goes straight to a renewal and
    // mints one that genuinely matches this token.
    phpsessid[0] = '\0';
    sessionCookiesChanged = true;
    Serial.println("[AdminPortal] New CHESSCOM_REMEMBERME saved - clearing old PHPSESSID, a fresh one will be minted on the next poll.");
  }
  if (server.hasArg("myusername")) {
    server.arg("myusername").toCharArray(myUsername, sizeof(myUsername));
  }
  if (server.hasArg("adminuser") && server.arg("adminuser").length() > 0) {
    server.arg("adminuser").toCharArray(webAdminUser, sizeof(webAdminUser));
  }
  if (server.hasArg("adminpass") && server.arg("adminpass").length() > 0) {
    server.arg("adminpass").toCharArray(webAdminPass, sizeof(webAdminPass));
  }
  if (server.hasArg("lang")) {
    currentLanguage = (Language)server.arg("lang").toInt();
  }
  if (server.hasArg("datasrc")) {
    currentDataSource = (DataSourceType)server.arg("datasrc").toInt();
  }
  if (server.hasArg("resultdur")) {
    // Server-side bounds matching the form's min/max - the HTML
    // attributes are only a UI hint, a raw POST could send anything.
    long secs = constrain(server.arg("resultdur").toInt(), 3, 120);
    resultDisplayDurationMs = (unsigned long)secs * 1000UL;
  }
  if (server.hasArg("snd_volume")) {
    soundVolume = (uint8_t)constrain(server.arg("snd_volume").toInt(), 0, 100);
  }

  if (server.hasArg("led_nowifi")) hexToRgb(server.arg("led_nowifi"), ledColorNoWifi);
  if (server.hasArg("led_lowbatt")) hexToRgb(server.arg("led_lowbatt"), ledColorLowBattery);
  if (server.hasArg("led_won")) hexToRgb(server.arg("led_won"), ledColorWon);
  if (server.hasArg("led_lost")) hexToRgb(server.arg("led_lost"), ledColorLost);
  if (server.hasArg("led_draw")) hexToRgb(server.arg("led_draw"), ledColorDraw);
  if (server.hasArg("led_myturn")) hexToRgb(server.arg("led_myturn"), ledColorMyTurn);
  if (server.hasArg("led_oppturn")) hexToRgb(server.arg("led_oppturn"), ledColorOpponentTurn);
  if (server.hasArg("led_bright_day")) ledBrightnessDay = (uint8_t)constrain(server.arg("led_bright_day").toInt(), 0, 255);
  if (server.hasArg("led_bright_night")) ledBrightnessNight = (uint8_t)constrain(server.arg("led_bright_night").toInt(), 0, 255);
  if (server.hasArg("led_count")) {
    ledCount = (uint16_t)constrain(server.arg("led_count").toInt(), 1, 60);
    setLedCount(ledCount);  // applied immediately - no reboot needed, same as the color/brightness fields above
  }

  // Checkboxes are only present in the POST body when checked - unlike the
  // hasArg()+non-empty guards above (which preserve the current value when
  // a field is left blank), a MISSING snd_en_* genuinely means the user
  // unchecked it, so presence alone (not content) is what toggles it. BUT
  // that only holds when the Sounds tab's form is what was actually
  // submitted - every OTHER tab's form has no snd_en_* fields at all
  // (they're not on that page), and would otherwise read as "every event
  // just got unchecked" on every save from Main/Connections/LEDs. Gated on
  // the hidden 'tab' field pageFoot() puts in every form.
  if (server.arg("tab") == "sounds") {
    for (int i = 0; i < SOUND_EVENT_COUNT; i++) {
      char enKey[16], melKey[16];
      snprintf(enKey, sizeof(enKey), "snd_en_%s", SOUND_EVENT_META[i].key);
      snprintf(melKey, sizeof(melKey), "snd_mel_%s", SOUND_EVENT_META[i].key);
      soundEnabled[i] = server.hasArg(enKey);
      if (server.hasArg(melKey)) {
        server.arg(melKey).toCharArray(soundMelodyOverride[i], SOUND_MELODY_MAX_LEN);
      }
    }
  }

  saveConfig();

  if (sessionCookiesChanged) {
    // Without this, ChessApiFunctions.cpp's session cookie jar keeps
    // using whatever was seeded at boot - a credential-only save doesn't
    // restart the device, so the globals above would silently drift out
    // of sync with the jar until the next reboot.
    refreshSessionCookies();
  }

  if (wifiChanged) {
    // Only a WiFi network/password change actually needs a reboot (to
    // re-run WiFi.begin() cleanly) - everything else (LED colors,
    // username, phpsessid, language, result duration) already took
    // effect in the running globals above, no restart needed. This
    // matters because a restart tears down any in-progress live game
    // connection for no reason.
    server.send(200, "text/html", "<html><body><h3>Saved. Restarting to apply new WiFi settings...</h3></body></html>");
    delay(1000);
    ESP.restart();
  } else {
    server.send(200, "text/html",
                "<html><head><meta http-equiv='refresh' content='1;url=" + tabPath(server.arg("tab")) + "'></head>"
                "<body><h3>Saved.</h3></body></html>");
  }
}

// GET /oauth/lichess/callback?code=...&state=... - Lichess redirects the
// user's browser HERE after consent. MUST stay behind checkAuth() like
// every other route: without it, anyone on the LAN who catches this URL
// during the ~seconds the code is valid could bind their own Lichess
// account to the device. Because this is a same-origin redirect back to
// a host the browser already authenticated against moments earlier (to
// load the "Connect" link in the first place), the cached Basic Auth
// credentials are expected to just be resent automatically - but that's
// exactly the kind of "works in Chrome desktop, breaks in Safari iOS"
// behavior that needs confirming on real hardware/devices, not assumed.
static void handleLichessOAuthCallback() {
  if (!checkAuth()) return;

  if (!havePendingLichessPkce) {
    server.send(400, "text/html", "<html><body><h3>No Lichess connection was in progress. Go back and click Connect to Lichess again.</h3></body></html>");
    return;
  }
  // Consumed unconditionally from here on (success or failure) - a stale
  // pending exchange must never be reusable for a second attempt.
  havePendingLichessPkce = false;

  if (!server.hasArg("code") || !server.hasArg("state")) {
    server.send(400, "text/html", "<html><body><h3>Lichess didn't return an authorization code. Go back and try again.</h3></body></html>");
    return;
  }
  String state = server.arg("state");
  if (state != String(pendingLichessPkce.state)) {
    Serial.println("[AdminPortal] Lichess OAuth callback: state mismatch - discarding (stale or forged attempt).");
    server.send(400, "text/html", "<html><body><h3>This Lichess connection attempt expired or doesn't match. Go back and click Connect to Lichess again.</h3></body></html>");
    return;
  }

  String code = server.arg("code");  // never logged - see LichessApiFunctions.cpp's redaction discipline
  char freshToken[LICHESS_TOKEN_MAX_LEN];
  bool ok = lichessExchangeCodeForToken(code.c_str(), pendingLichessPkce.verifier, lichessRedirectUri(),
                                         freshToken, sizeof(freshToken));
  if (!ok) {
    server.send(502, "text/html", "<html><body><h3>Lichess didn't accept that connection attempt. Go back and try again.</h3></body></html>");
    return;
  }

  strncpy(lichessToken, freshToken, sizeof(lichessToken) - 1);
  lichessToken[sizeof(lichessToken) - 1] = '\0';

  char freshUsername[USERNAME_MAX_LEN];
  if (lichessFetchUsername(lichessToken, freshUsername, sizeof(freshUsername))) {
    strncpy(lichessUsername, freshUsername, sizeof(lichessUsername) - 1);
    lichessUsername[sizeof(lichessUsername) - 1] = '\0';
  }
  // else: token is saved either way - the "connected as ___" line just
  // stays blank until the next successful fetch (e.g. next boot), rather
  // than treating a username-lookup hiccup as if the connection itself
  // had failed.

  saveConfig();
  Serial.printf("[AdminPortal] Lichess connected as %s.\n", lichessUsername);

  server.send(200, "text/html",
              "<html><head><meta http-equiv='refresh' content='1;url=/connections'></head>"
              "<body><h3>Connected to Lichess.</h3></body></html>");
}

// POST /chesscom/invalidate - explicit "forget this session" action,
// distinct from the Save button leaving the cookie field empty (which
// deliberately KEEPS whatever's currently stored - see handleSave()). This
// is a LOCAL wipe only (e.g. before handing/selling the physical clock to
// someone else) - chess.com has no cookie-revocation endpoint to call, so
// the remember-me token itself is still whatever it was server-side; the
// point here is just that this device no longer holds a copy of it. The
// next fetchActiveGame()/testChessComSession() naturally reports "No
// chess.com cookie set" until a new one is pasted.
static void handleChessComInvalidate() {
  if (!checkAuth()) return;

  phpsessid[0] = '\0';
  chessComRememberMe[0] = '\0';
  refreshSessionCookies();
  saveConfig();
  if (currentDataSource == DATA_SOURCE_CHESSCOM_WIFI) {
    // No credentials left to poll with - fall back rather than leave the
    // clock stuck showing a chess.com-flavored error forever. Mirrors the
    // same fallback handleLichessDisconnect() does the other way around.
    currentDataSource = DATA_SOURCE_LICHESS_WIFI;
  }
  Serial.println("[AdminPortal] Chess.com cookie forgotten locally (no server-side revocation - chess.com has no such endpoint).");

  server.send(200, "text/html",
              "<html><head><meta http-equiv='refresh' content='1;url=/connections'></head>"
              "<body><h3>Forgotten.</h3></body></html>");
}

// POST /oauth/lichess/disconnect - forgets the token locally. Lichess's
// API has no documented token-revocation endpoint (verified against its
// OpenAPI spec), so this can't reach out and invalidate it server-side;
// the webadmin text next to "Connect" should make clear that a user who
// wants it fully revoked needs lichess.org/account/oauth/token.
static void handleLichessDisconnect() {
  if (!checkAuth()) return;

  lichessToken[0] = '\0';
  lichessUsername[0] = '\0';
  if (currentDataSource == DATA_SOURCE_LICHESS_WIFI) {
    // No credentials left to poll with - fall back rather than leave the
    // clock stuck showing a Lichess-flavored error forever.
    currentDataSource = DATA_SOURCE_CHESSCOM_WIFI;
  }
  saveConfig();
  Serial.println("[AdminPortal] Lichess disconnected (token forgotten locally).");

  server.send(200, "text/html",
              "<html><head><meta http-equiv='refresh' content='1;url=/connections'></head>"
              "<body><h3>Disconnected.</h3></body></html>");
}

// Browsers automatically request this right after loading any page.
// Without an explicit route it fell through onNotFound() to handleRoot()
// - which was the actual trigger for the PKCE-overwrite bug fixed above
// (see pendingLichessPkce's comment) - and served no icon either.
// Serving the real favicon (Favicon.h, same source image as the
// website's) here fixes both: the browser tab gets a proper icon, and
// the request no longer reaches handleRoot() at all. No checkAuth() -
// there's nothing here worth protecting, and browsers request this
// before any login has necessarily happened.
// POST /sound/reset - clears one event's custom melody back to the
// compiled-in default (SoundFunctions.cpp's soundDefaultMelody()). Only
// resets soundMelodyOverride, not soundEnabled - "reset to default sound"
// shouldn't also silently re-enable an event the user deliberately muted.
static void handleSoundReset() {
  if (!checkAuth()) return;

  String key = server.arg("sndevt");
  for (int i = 0; i < SOUND_EVENT_COUNT; i++) {
    if (key == SOUND_EVENT_META[i].key) {
      soundMelodyOverride[i][0] = '\0';
      saveConfig();
      Serial.printf("[AdminPortal] Sound '%s' reset to default melody.\n", SOUND_EVENT_META[i].key);
      break;
    }
  }

  // Only ever called via the Sounds tab's fetch() (see PAGE_SCRIPT) - no
  // navigation happens, so no meta-refresh needed, just a cheap ack.
  server.send(200, "text/plain", "OK");
}

// POST /sound/test - plays one event's melody immediately, using whatever
// is CURRENTLY TYPED in that event's field on the submitted form (even if
// not saved yet) so a pasted RTTTL string can be previewed before
// committing it - falls back to the saved override/default only if the
// field was left empty. Deliberately doesn't call saveConfig(): this is a
// preview, not a save.
static void handleSoundTest() {
  if (!checkAuth()) return;

  String key = server.arg("sndevt");
  for (int i = 0; i < SOUND_EVENT_COUNT; i++) {
    if (key == SOUND_EVENT_META[i].key) {
      char melKey[24];
      snprintf(melKey, sizeof(melKey), "snd_mel_%s", SOUND_EVENT_META[i].key);
      String typed = server.arg(melKey);
      const char *toPlay = typed.length() ? typed.c_str()
                          : (soundMelodyOverride[i][0] != '\0') ? soundMelodyOverride[i]
                          : soundDefaultMelody((SoundEvent)i);
      playMelodyNow(toPlay);
      Serial.printf("[AdminPortal] Testing sound '%s'.\n", SOUND_EVENT_META[i].key);
      break;
    }
  }

  // Only ever called via the Sounds tab's fetch() (see PAGE_SCRIPT) - no
  // navigation happens, so no meta-refresh needed, just a cheap ack.
  server.send(200, "text/plain", "OK");
}

// POST /led/test - cycles the strip through the 7 CURRENTLY TYPED LED
// colors from the submitted form (even if not saved yet), one at a time -
// see LedFunctions.h's startLedTest(). Same "preview before you save"
// reasoning as handleSoundTest() above.
static void handleLedTest() {
  if (!checkAuth()) return;

  uint8_t colors[7][3];
  hexToRgb(server.arg("led_nowifi"), colors[0]);
  hexToRgb(server.arg("led_lowbatt"), colors[1]);
  hexToRgb(server.arg("led_myturn"), colors[2]);
  hexToRgb(server.arg("led_oppturn"), colors[3]);
  hexToRgb(server.arg("led_won"), colors[4]);
  hexToRgb(server.arg("led_lost"), colors[5]);
  hexToRgb(server.arg("led_draw"), colors[6]);
  startLedTest(colors, 7);
  Serial.println("[AdminPortal] Testing LED colors.");

  // Only ever called via the LEDs tab's fetch() (see PAGE_SCRIPT) - no
  // navigation happens, so no meta-refresh needed, just a cheap ack.
  server.send(200, "text/plain", "OK");
}

static void handleFavicon() {
  server.send_P(200, PSTR("image/png"), (PGM_P)FAVICON_PNG, FAVICON_PNG_LEN);
}

static void startApMode() {
  apMode = true;
  WiFi.mode(WIFI_AP);
  WiFi.softAP(SETUP_AP_NAME);
  dnsServer.start(53, "*", WiFi.softAPIP());
  Serial.printf("[AdminPortal] Setup hotspot '%s' - http://%s/\n", SETUP_AP_NAME, WiFi.softAPIP().toString().c_str());
}

static bool tryConnectSavedWifi() {
  if (strlen(wifiSSID) == 0) {
    return false;
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSSID, wifiPassword);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < (WIFI_CONNECT_TIMEOUT_SECONDS * 1000UL)) {
    delay(250);
  }
  return WiFi.status() == WL_CONNECTED;
}

void initAdminPortal(ClockState *statePtr) {
  g_state = statePtr;
  loadConfig();

  if (tryConnectSavedWifi()) {
    apMode = false;
    Serial.printf("[AdminPortal] WiFi connected. IP: %s\n", WiFi.localIP().toString().c_str());
    if (MDNS.begin(MDNS_HOSTNAME)) {
      Serial.printf("[AdminPortal] mDNS: http://%s.local/\n", MDNS_HOSTNAME);
    }
  } else {
    startApMode();
  }

  // WebServer's authenticate() only sees the Authorization header once the
  // library's internal header-collection list has been set up
  // (collectHeaders()/collectAllHeaders()) - without this, it silently
  // stays unset until something else happens to trigger it (observed on
  // real hardware: right after a fresh boot, authenticate() rejected the
  // CORRECT credentials every time - it wasn't even seeing the header to
  // compare against). Registering it explicitly here means login works
  // from the very first request, not just "eventually".
  static const char *kAuthHeaders[] = {"Authorization"};
  server.collectHeaders(kAuthHeaders, 1);

  server.on("/", handleRoot);
  server.on("/connections", handleConnections);
  server.on("/sounds", handleSounds);
  server.on("/leds", handleLeds);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/chesscom/invalidate", HTTP_POST, handleChessComInvalidate);
  server.on("/sound/reset", HTTP_POST, handleSoundReset);
  server.on("/sound/test", HTTP_POST, handleSoundTest);
  server.on("/led/test", HTTP_POST, handleLedTest);
  server.on("/favicon.ico", handleFavicon);
  server.on(LICHESS_OAUTH_CALLBACK_PATH, handleLichessOAuthCallback);
  server.on("/oauth/lichess/disconnect", HTTP_POST, handleLichessDisconnect);
  server.onNotFound(handleRoot);  // any unknown URL falls back here - helps the phone "find" the captive portal
  server.begin();
}

void handleAdminPortal() {
  if (apMode) {
    dnsServer.processNextRequest();
  }
  server.handleClient();
}

void updateWiFiStatus(ClockState &state) {
  state.apMode = apMode;
  state.wifiConnected = !apMode && (WiFi.status() == WL_CONNECTED);
  state.wifiStrength = state.wifiConnected ? WiFi.RSSI() : 0;
  strncpy(state.wifiSSID, wifiSSID, sizeof(state.wifiSSID) - 1);
  state.wifiSSID[sizeof(state.wifiSSID) - 1] = '\0';

  const char *quality = wifiQualityLabel(state.wifiConnected, state.wifiStrength);
  strncpy(state.wifiQuality, quality, sizeof(state.wifiQuality) - 1);
  state.wifiQuality[sizeof(state.wifiQuality) - 1] = '\0';

  String ip = apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  ip.toCharArray(state.ipAddress, sizeof(state.ipAddress));
}

#define UPDATE_CHECK_INTERVAL_MS (24UL * 60UL * 60UL * 1000UL)  // once a day is plenty

void checkForFirmwareUpdate() {
  static unsigned long lastCheckMs = 0;  // 0 = never checked yet, so the very first loop() after boot checks immediately
  unsigned long now = millis();
  if (lastCheckMs != 0 && (now - lastCheckMs) < UPDATE_CHECK_INTERVAL_MS) return;
  if (apMode || WiFi.status() != WL_CONNECTED) return;  // try again next tick rather than commit lastCheckMs to a check that never happened
  lastCheckMs = now;

  WiFiClientSecure client;
  client.setInsecure();  // same "no pinned Root CA yet" tradeoff as the chess.com/Lichess API calls
  HTTPClient https;
  if (!https.begin(client, "https://api.github.com/repos/ivolanski/IvoChess-Clock/releases/latest")) {
    return;
  }
  https.addHeader("User-Agent", "IvoChessClock-FirmwareUpdateCheck");  // GitHub's API 403s any request with no User-Agent at all
  https.setTimeout(10000);

  int httpCode = https.GET();
  if (httpCode != 200) {
    Serial.printf("[AdminPortal] Firmware update check failed: HTTP %d\n", httpCode);
    https.end();
    return;
  }

  String payload = https.getString();
  https.end();

  DynamicJsonDocument doc(2048);  // GitHub's release JSON has a lot of fields we don't need; only tag_name is read below
  if (deserializeJson(doc, payload) != DeserializationError::Ok) {
    Serial.println("[AdminPortal] Firmware update check: couldn't parse GitHub's response.");
    return;
  }

  const char *tag = doc["tag_name"] | "";
  if (tag[0] == '\0') return;

  strncpy(latestVersionTag, tag, sizeof(latestVersionTag) - 1);
  latestVersionTag[sizeof(latestVersionTag) - 1] = '\0';

  // Tags are "vX.Y.Z"; FIRMWARE_VERSION (config.h) is "X.Y.Z" with no
  // leading 'v' - strip it before comparing so a device actually running
  // the latest release doesn't show itself a false "update available".
  const char *latestVersion = (tag[0] == 'v' || tag[0] == 'V') ? tag + 1 : tag;
  updateAvailable = (strcmp(latestVersion, FIRMWARE_VERSION) != 0);
  Serial.printf("[AdminPortal] Firmware update check: running %s, latest release %s.%s\n",
                FIRMWARE_VERSION, latestVersionTag, updateAvailable ? " Update available." : "");
}
