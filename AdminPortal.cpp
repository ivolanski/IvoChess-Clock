#include "AdminPortal.h"
#include "config.h"
#include "Translations.h"
#include "GameDataSource.h"

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>

char phpsessid[PHPSESSID_MAX_LEN] = DEFAULT_PHPSESSID;

static char wifiSSID[WIFI_SSID_MAX_LEN] = DEFAULT_WIFI_SSID;
static char wifiPassword[WIFI_PASS_MAX_LEN] = DEFAULT_WIFI_PASSWORD;

static Preferences prefs;
static WebServer server(ADMIN_PORT);
static DNSServer dnsServer;
static ClockState *g_state = nullptr;
static bool apMode = false;

static void loadConfig() {
  prefs.begin(PREFS_NAMESPACE, /*readOnly=*/true);
  prefs.getString(PREFS_KEY_PHPSESSID, DEFAULT_PHPSESSID).toCharArray(phpsessid, sizeof(phpsessid));
  prefs.getString("wifi_ssid", DEFAULT_WIFI_SSID).toCharArray(wifiSSID, sizeof(wifiSSID));
  prefs.getString("wifi_pass", DEFAULT_WIFI_PASSWORD).toCharArray(wifiPassword, sizeof(wifiPassword));
  currentLanguage = (Language)prefs.getInt("lang", LANG_EN);
  currentDataSource = (DataSourceType)prefs.getInt("datasrc", DATA_SOURCE_CHESSCOM_WIFI);
  prefs.end();
}

static void saveConfig() {
  prefs.begin(PREFS_NAMESPACE, /*readOnly=*/false);
  prefs.putString(PREFS_KEY_PHPSESSID, phpsessid);
  prefs.putString("wifi_ssid", wifiSSID);
  prefs.putString("wifi_pass", wifiPassword);
  prefs.putInt("lang", (int)currentLanguage);
  prefs.putInt("datasrc", (int)currentDataSource);
  prefs.end();
}

static const char *wifiQualityLabel(bool connected, int rssi) {
  if (!connected) return T(STR_DISCONNECTED);
  if (rssi > -60) return T(STR_EXCELLENT);
  if (rssi > -75) return T(STR_GOOD);
  return T(STR_POOR);
}

static void handleRoot() {
  String html = "<html><head><title>IvoChess Clock</title>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "</head><body style='font-family:sans-serif;max-width:420px;margin:auto;padding:10px;'>";
  html += "<h2>IvoChess Clock - Admin</h2>";

  if (apMode) {
    html += "<p><strong>Mode:</strong> Setup hotspot (not on a real WiFi network yet)</p>";
  } else {
    html += "<p><strong>WiFi:</strong> " + String(wifiSSID) + " (" + String(wifiQualityLabel(true, WiFi.RSSI())) + ")</p>";
    html += "<p><strong>IP:</strong> " + WiFi.localIP().toString() + " (also http://" + MDNS_HOSTNAME + ".local/)</p>";
  }

  html += "<form method='POST' action='/save'>";

  html += "<h3>WiFi network</h3>";
  html += "SSID: <input type='text' name='ssid' value='" + String(wifiSSID) + "' style='width:100%'><br><br>";
  html += "Password: <input type='password' name='pass' placeholder='(leave empty to keep current)' style='width:100%'><br>";

  html += "<h3>Language</h3>";
  html += "<select name='lang' style='width:100%'>";
  html += "<option value='0'"; html += (currentLanguage == LANG_EN ? " selected" : ""); html += ">English</option>";
  html += "<option value='1'"; html += (currentLanguage == LANG_PT ? " selected" : ""); html += ">Portugues</option>";
  html += "</select>";

  html += "<h3>Connection</h3>";
  html += "<select name='datasrc' style='width:100%'>";
  html += "<option value='0' selected>Chess.com</option>";
  html += "</select><br><br>";
  html += "PHPSESSID: <input type='password' name='phpsessid' placeholder='(leave empty to keep current)' style='width:100%'><br>";

  html += "<br><input type='submit' value='Save and restart' style='width:100%;padding:10px;'>";
  html += "</form>";
  html += "</body></html>";

  server.send(200, "text/html", html);
}

static void handleSave() {
  if (server.hasArg("ssid")) {
    server.arg("ssid").toCharArray(wifiSSID, sizeof(wifiSSID));
  }
  if (server.hasArg("pass") && server.arg("pass").length() > 0) {
    server.arg("pass").toCharArray(wifiPassword, sizeof(wifiPassword));
  }
  if (server.hasArg("phpsessid") && server.arg("phpsessid").length() > 0) {
    server.arg("phpsessid").toCharArray(phpsessid, sizeof(phpsessid));
  }
  if (server.hasArg("lang")) {
    currentLanguage = (Language)server.arg("lang").toInt();
  }
  if (server.hasArg("datasrc")) {
    currentDataSource = (DataSourceType)server.arg("datasrc").toInt();
  }

  saveConfig();

  server.send(200, "text/html", "<html><body><h3>Saved. Restarting...</h3></body></html>");
  delay(1000);
  ESP.restart();  // simplest and most reliable way to apply new WiFi/config
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

  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
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
