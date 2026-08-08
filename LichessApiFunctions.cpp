#include "LichessApiFunctions.h"
#include "config.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

static const char *USER_AGENT =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36";

// Percent-encodes a value for safe use inside a URL query string or an
// application/x-www-form-urlencoded body (RFC 3986 unreserved characters
// pass through as-is, everything else becomes %XX). Small and local to
// this file - this project has no existing URL-encoding utility to reuse
// (chess.com's fixed-shape URLs never needed one).
static String urlEncode(const String &value) {
  String out;
  out.reserve(value.length() * 3);
  const char *hex = "0123456789ABCDEF";
  for (size_t i = 0; i < value.length(); i++) {
    uint8_t c = (uint8_t)value[i];
    bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                       (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
    if (unreserved) {
      out += (char)c;
    } else {
      out += '%';
      out += hex[(c >> 4) & 0xF];
      out += hex[c & 0xF];
    }
  }
  return out;
}

String lichessBuildAuthorizeUrl(const PkceExchange &pkce, const String &redirectUri) {
  String url = String(LICHESS_AUTHORIZE_URL) + "?response_type=code";
  url += "&client_id=" + urlEncode(LICHESS_OAUTH_CLIENT_ID);
  url += "&redirect_uri=" + urlEncode(redirectUri);
  url += "&scope=" + urlEncode(LICHESS_OAUTH_SCOPE);
  url += "&code_challenge_method=S256";
  url += "&code_challenge=" + urlEncode(pkce.challenge);
  url += "&state=" + urlEncode(pkce.state);
  return url;
}

bool lichessExchangeCodeForToken(const char *code, const char *verifier, const String &redirectUri,
                                  char *tokenOut, size_t tokenOutLen) {
  WiFiClientSecure client;
  client.setInsecure();  // TODO: swap for a real Root CA once this flow is validated - matches the existing chess.com TODO (ChessApiFunctions.cpp)
  HTTPClient http;
  if (!http.begin(client, LICHESS_TOKEN_URL)) {
    return false;
  }
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  http.addHeader("User-Agent", USER_AGENT);
  http.setTimeout(10000);

  String body = "grant_type=authorization_code";
  body += "&code=" + urlEncode(code);
  body += "&redirect_uri=" + urlEncode(redirectUri);
  body += "&client_id=" + urlEncode(LICHESS_OAUTH_CLIENT_ID);
  body += "&code_verifier=" + urlEncode(verifier);

  // Never log 'body' - it contains the authorization code and the PKCE
  // verifier, both credential-adjacent (same redaction discipline as the
  // chess.com cookie handling).
  int httpCode = http.POST(body);
  if (httpCode != 200) {
    Serial.printf("[LichessAPI] Token exchange failed: HTTP %d\n", httpCode);
    http.end();
    return false;
  }

  String respBody = http.getString();
  http.end();

  DynamicJsonDocument doc(1024);
  DeserializationError parseError = deserializeJson(doc, respBody);
  if (parseError) {
    Serial.printf("[LichessAPI] Token exchange response JSON error: %s\n", parseError.c_str());
    return false;
  }

  const char *accessToken = doc["access_token"] | "";
  if (accessToken[0] == '\0') {
    Serial.println("[LichessAPI] Token exchange response had no access_token.");
    return false;
  }
  if (strlen(accessToken) >= tokenOutLen) {
    Serial.printf("[LichessAPI] Access token too long for buffer (%u >= %u) - NOT storing a truncated token.\n",
                  (unsigned)strlen(accessToken), (unsigned)tokenOutLen);
    return false;
  }
  strncpy(tokenOut, accessToken, tokenOutLen - 1);
  tokenOut[tokenOutLen - 1] = '\0';
  Serial.printf("[LichessAPI] Token exchange succeeded (token len=%u).\n", (unsigned)strlen(accessToken));
  return true;
}

bool lichessFetchUsername(const char *token, char *usernameOut, size_t usernameOutLen) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, "https://lichess.org/api/account")) {
    return false;
  }
  String auth = "Bearer " + String(token);
  http.addHeader("Authorization", auth);
  http.addHeader("User-Agent", USER_AGENT);
  http.setTimeout(10000);

  int httpCode = http.GET();
  if (httpCode != 200) {
    Serial.printf("[LichessAPI] /api/account failed: HTTP %d\n", httpCode);
    http.end();
    return false;
  }

  String body = http.getString();
  http.end();

  DynamicJsonDocument doc(2048);
  DeserializationError parseError = deserializeJson(doc, body);
  if (parseError) {
    Serial.printf("[LichessAPI] /api/account JSON error: %s\n", parseError.c_str());
    return false;
  }

  const char *username = doc["username"] | "";
  strncpy(usernameOut, username, usernameOutLen - 1);
  usernameOut[usernameOutLen - 1] = '\0';
  return username[0] != '\0';
}

bool lichessFetchActiveGame(const char *token, LichessGameInfo &game,
                             char *statusOut, size_t statusOutLen) {
  if (WiFi.status() != WL_CONNECTED) {
    snprintf(statusOut, statusOutLen, "No WiFi");
    return false;
  }
  if (token[0] == '\0') {
    snprintf(statusOut, statusOutLen, "No Lichess token set");
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, LICHESS_PLAYING_ENDPOINT_URL)) {
    snprintf(statusOut, statusOutLen, "begin() failed");
    return false;
  }
  String auth = "Bearer " + String(token);
  http.addHeader("Authorization", auth);
  http.addHeader("Accept", "application/json");
  http.addHeader("User-Agent", USER_AGENT);
  http.setTimeout(10000);

  int httpCode = http.GET();
  if (httpCode <= 0) {
    snprintf(statusOut, statusOutLen, "Timeout/conn error (%d)", httpCode);
    http.end();
    return false;
  }
  if (httpCode == 401) {
    // Unlike chess.com's PHPSESSID, there is no renewal path here - a
    // Lichess access token that 401s has simply been revoked (by the
    // user, or by Lichess). The webadmin's "Connected as X" state needs
    // to flip back to "not connected" so the user knows to reconnect -
    // GameDataSource.cpp is what surfaces this, this function just
    // reports it plainly.
    snprintf(statusOut, statusOutLen, "HTTP 401 (token revoked - reconnect in webadmin)");
    http.end();
    return false;
  }
  if (httpCode != 200) {
    snprintf(statusOut, statusOutLen, "HTTP %d", httpCode);
    http.end();
    return false;
  }

  String body = http.getString();
  http.end();

  DynamicJsonDocument doc(4096);
  DeserializationError parseError = deserializeJson(doc, body);
  if (parseError) {
    snprintf(statusOut, statusOutLen, "JSON error: %s", parseError.c_str());
    return false;
  }

  JsonArray games = doc["nowPlaying"].as<JsonArray>();
  if (games.isNull() || games.size() == 0) {
    snprintf(statusOut, statusOutLen, "OK (no game)");
    return false;
  }

  JsonObject g = games[0];
  strncpy(game.id, g["gameId"] | "", sizeof(game.id) - 1);
  game.id[sizeof(game.id) - 1] = '\0';

  const char *color = g["color"] | "white";
  game.myColorIsWhite = (strcmp(color, "white") == 0);

  JsonObject opp = g["opponent"];
  const char *oppUsername = opp["username"] | "?";
  strncpy(game.opponent.username, oppUsername, USERNAME_MAX_LEN - 1);
  game.opponent.username[USERNAME_MAX_LEN - 1] = '\0';
  game.opponent.rating = opp["rating"] | 0;
  game.opponent.clockMs = 0;  // not carried by this endpoint - only the live stream has real clocks

  game.myRating = g["rating"] | 0;

  snprintf(statusOut, statusOutLen, "OK (1 game)");
  Serial.printf("[LichessAPI] Game found: me(%s, %d) vs %s(%d) - id=%s\n",
                game.myColorIsWhite ? "white" : "black", game.myRating,
                game.opponent.username, game.opponent.rating, game.id);
  return true;
}
