#include "ChessApiFunctions.h"
#include "config.h"
#include "AdminPortal.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

bool fetchActiveGame(GameInfo &game, char *statusOut, size_t statusOutLen) {
  if (WiFi.status() != WL_CONNECTED) {
    snprintf(statusOut, statusOutLen, "No WiFi");
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();  // TODO: swap for a real Root CA once the flow is validated

  HTTPClient http;
  if (!http.begin(client, GAMES_ENDPOINT_URL)) {
    snprintf(statusOut, statusOutLen, "begin() failed");
    return false;
  }

  char cookieHeader[PHPSESSID_MAX_LEN + 32];
  snprintf(cookieHeader, sizeof(cookieHeader), "PHPSESSID=%s", phpsessid);
  http.addHeader("Cookie", cookieHeader);
  http.addHeader("Accept", "application/json");
  http.addHeader("User-Agent",
                 "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
                 "(KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36");
  http.setTimeout(10000);

  int httpCode = http.GET();
  if (httpCode <= 0) {
    snprintf(statusOut, statusOutLen, "Timeout/conn error (%d)", httpCode);
    http.end();
    return false;
  }
  if (httpCode != 200) {
    snprintf(statusOut, statusOutLen, "HTTP %d", httpCode);
    http.end();
    if (httpCode == 401 || httpCode == 403) {
      Serial.println("[ChessAPI] 401/403 - PHPSESSID is probably expired, recapture it in the browser.");
    }
    return false;
  }

  String body = http.getString();
  http.end();

  DynamicJsonDocument doc(8192);
  DeserializationError parseError = deserializeJson(doc, body);
  if (parseError) {
    snprintf(statusOut, statusOutLen, "JSON error: %s", parseError.c_str());
    return false;
  }

  JsonArray games = doc["games"].as<JsonArray>();
  if (games.isNull() || games.size() == 0) {
    snprintf(statusOut, statusOutLen, "OK (no game)");
    return false;
  }

  JsonObject g = games[0];

  strncpy(game.id, g["id"] | "", sizeof(game.id) - 1);
  game.id[sizeof(game.id) - 1] = '\0';

  game.legacyId = g["legacyId"] | 0L;

  const char *rsocketUrl = g["transports"]["rsocket"]["url"] | "";
  strncpy(game.rsocketUrl, rsocketUrl, sizeof(game.rsocketUrl) - 1);
  game.rsocketUrl[sizeof(game.rsocketUrl) - 1] = '\0';

  const char *watchRoute = g["transports"]["rsocket"]["routes"]["watch"] | "";
  strncpy(game.watchRoute, watchRoute, sizeof(game.watchRoute) - 1);
  game.watchRoute[sizeof(game.watchRoute) - 1] = '\0';

  for (int i = 0; i < 2; i++) {
    game.players[i].username[0] = '\0';
    game.players[i].rating = 0;
    game.players[i].clockMs = 0;
  }

  JsonArray playersDetails = g["playersDetails"].as<JsonArray>();
  for (int i = 0; i < 2 && i < (int)playersDetails.size(); i++) {
    JsonObject p = playersDetails[i];
    const char *username = p["username"] | "?";
    strncpy(game.players[i].username, username, USERNAME_MAX_LEN - 1);
    game.players[i].username[USERNAME_MAX_LEN - 1] = '\0';
    game.players[i].rating = p["rating"] | 0;
  }

  snprintf(statusOut, statusOutLen, "OK (1 game)");
  Serial.printf("[ChessAPI] Game found: %s(%d) vs %s(%d) - id=%s\n",
                game.players[0].username, game.players[0].rating,
                game.players[1].username, game.players[1].rating,
                game.id);

  return true;
}
