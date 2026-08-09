#include "ChessApiFunctions.h"
#include "config.h"
#include "AdminPortal.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

static const char *USER_AGENT =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36";

// Persistent across calls (not reset each fetchActiveGame()) so cookies
// behave like a real browser tab's jar - both PHPSESSID and
// CHESSCOM_REMEMBERME live here, and the ESP32 core's HTTPClient
// (HTTPClient.h: setCookieJar/Cookie/CookieJar) automatically sends
// whatever's in it and folds in any Set-Cookie from responses. Needed
// because collectHeaders()+header("Set-Cookie") only keeps the LAST of
// several same-named headers - chess.com sends ~9 Set-Cookie lines on
// one response, and both cookies we care about are commonly among them.
static CookieJar sessionCookieJar;
static bool jarSeeded = false;

static void seedJarCookie(const char *name, const char *value) {
  if (value[0] == '\0') return;
  Cookie c;
  c.name = name;
  c.value = value;
  c.domain = "chess.com";  // matches what HTTPClient::setCookie() derives for www.chess.com - see its domain-parsing fallback
  sessionCookieJar.push_back(c);
}

// Seeds the jar from Preferences (via AdminPortal's phpsessid/
// chessComRememberMe) exactly once per boot. After that the jar is the
// live source of truth - renewSession() below updates it AND the
// globals AND Preferences together, so they never drift apart.
static void seedCookieJarIfNeeded() {
  if (jarSeeded) return;
  jarSeeded = true;
  seedJarCookie("PHPSESSID", phpsessid);
  seedJarCookie("CHESSCOM_REMEMBERME", chessComRememberMe);
}

void refreshSessionCookies() {
  // Drop any existing PHPSESSID/CHESSCOM_REMEMBERME entries first so a
  // repeated call can't leave duplicates in the jar (HTTPClient's own
  // Set-Cookie handling matches by name+domain and replaces in place, but
  // this manual refresh path doesn't go through that), then reseed from
  // whatever's current in the globals.
  for (auto it = sessionCookieJar.begin(); it != sessionCookieJar.end();) {
    if (it->name == "PHPSESSID" || it->name == "CHESSCOM_REMEMBERME") {
      it = sessionCookieJar.erase(it);
    } else {
      ++it;
    }
  }
  seedJarCookie("PHPSESSID", phpsessid);
  seedJarCookie("CHESSCOM_REMEMBERME", chessComRememberMe);
  jarSeeded = true;  // in case this runs before the first fetchActiveGame() ever does
}

// Collapses any duplicate PHPSESSID/CHESSCOM_REMEMBERME entries down to
// the newest one, keeping the LAST occurrence (HTTPClient::setCookie()
// appends a second entry instead of replacing whenever the incoming
// Domain attribute doesn't string-match the one already in the jar). With
// duplicates present, generateCookieString() emits BOTH
// ("PHPSESSID=old ;PHPSESSID=new") and the server picks one - usually the
// stale one, which reads as a dead session no matter how fresh the real
// cookie is. Also pins the domain to the value chess.com actually uses
// (Domain=.chess.com -> "chess.com" after HTTPClient strips the dot) so
// future Set-Cookies replace in place instead of piling up.
static void dedupeSessionCookies() {
  static const char *const kSessionCookieNames[] = {"PHPSESSID", "CHESSCOM_REMEMBERME"};
  for (const char *name : kSessionCookieNames) {
    int lastIdx = -1;
    for (int i = 0; i < (int)sessionCookieJar.size(); i++) {
      if (sessionCookieJar[i].name == name) lastIdx = i;
    }
    if (lastIdx < 0) continue;
    sessionCookieJar[lastIdx].domain = "chess.com";
    // Never let the jar expire these two out from under us: chess.com
    // sends PHPSESSID as a pure session cookie (no Expires/Max-Age at
    // all), so an inherited/garbage expiry from a previous entry must not
    // survive here.
    sessionCookieJar[lastIdx].expires.valid = false;
    sessionCookieJar[lastIdx].max_age.valid = false;
    for (int i = (int)sessionCookieJar.size() - 1; i >= 0; i--) {
      if (i != lastIdx && sessionCookieJar[i].name == name) {
        sessionCookieJar.erase(sessionCookieJar.begin() + i);
        if (i < lastIdx) lastIdx--;
      }
    }
  }
}

// Copies the jar's current PHPSESSID/CHESSCOM_REMEMBERME into the shared
// globals (so LiveGameClient.cpp's next WebSocket connect automatically
// uses whatever's freshest) and persists them, but ONLY if either
// actually changed - avoids pointless flash writes on every poll.
//
// CRITICAL: this must run after EVERY request, not just after an explicit
// renewal. chess.com rotates both cookies during ordinary traffic, and
// whatever it rotates to lives only in the jar (RAM) until this copies it
// out. Two things break when that copy doesn't happen:
//   - a power cycle (the on/off switch cuts battery power) reseeds the jar
//     from flash, i.e. from a value the server retired long ago;
//   - CHESSCOM_REMEMBERME is single-use. Presenting a token the server has
//     already rotated past is not read as "expired" but as token theft,
//     and the standard response is to invalidate EVERY session for the
//     account - so a stale copy doesn't just fail to renew, it actively
//     kills the working session we still had.
static bool syncJarToGlobalsIfChanged() {
  dedupeSessionCookies();

  bool changed = false;
  for (const Cookie &c : sessionCookieJar) {
    if (c.name == "PHPSESSID" && c.value != String(phpsessid)) {
      if (c.value.length() >= PHPSESSID_MAX_LEN) {
        Serial.printf("[ChessAPI] PHPSESSID too long for buffer (%u >= %d) - NOT storing a truncated cookie.\n",
                      c.value.length(), PHPSESSID_MAX_LEN);
        continue;
      }
      c.value.toCharArray(phpsessid, PHPSESSID_MAX_LEN);
      changed = true;
      Serial.println("[ChessAPI] Server rotated PHPSESSID - saving the new one.");
    } else if (c.name == "CHESSCOM_REMEMBERME" && c.value != String(chessComRememberMe)) {
      if (c.value.length() >= CHESSCOM_REMEMBERME_MAX_LEN) {
        Serial.printf("[ChessAPI] CHESSCOM_REMEMBERME too long for buffer (%u >= %d) - NOT storing a truncated token.\n",
                      c.value.length(), CHESSCOM_REMEMBERME_MAX_LEN);
        continue;
      }
      c.value.toCharArray(chessComRememberMe, CHESSCOM_REMEMBERME_MAX_LEN);
      changed = true;
      Serial.println("[ChessAPI] Server rotated CHESSCOM_REMEMBERME - saving the new token (the old one is now dead).");
    }
  }
  if (changed) {
    persistSessionCookies();
  }
  return changed;
}

// This is the actual mechanism a browser tab uses to "stay signed in for
// months" - confirmed empirically (not guessed): a request to an API
// subroute like /service/play/games with an EXPIRED PHPSESSID plus a
// VALID CHESSCOM_REMEMBERME still 401s - that route doesn't participate
// in renewal. A plain page load (tried /home) does: chess.com's
// authentication middleware notices the session is gone, validates the
// long-lived remember-me token instead, and responds with a fresh
// PHPSESSID via Set-Cookie - and rotates CHESSCOM_REMEMBERME itself to a
// new token in the same response (typical remember-me security pattern:
// each use invalidates the old token, so re-using a stale captured value
// a second time correctly fails). No login form, no credentials beyond
// the one-time capture of this cookie - the same hands-off renewal a
// browser gets automatically.
//
// Deliberately NOT gating success on the HTTP status code: this route
// answered 200 in manual testing but 302 from the device (same account,
// same cookie) - redirects can carry Set-Cookie just as well as a 200,
// and HTTPClient's default is to NOT auto-follow them
// (HTTPC_DISABLE_FOLLOW_REDIRECTS), so whatever status came back, any
// Set-Cookie on THAT response is already in the jar by the time GET()
// returns. Only a true transport failure (code <= 0: no response at all)
// is treated as "couldn't even try". The real test of success is the
// caller's retry of the actual endpoint - if the jar didn't actually get
// a working PHPSESSID, that retry will just 401 again.
static bool renewSession() {
  if (chessComRememberMe[0] == '\0') {
    return false;  // nothing to renew with - user needs to recapture both cookies
  }

  // Remember what we're about to present, so we can tell afterwards
  // whether the server actually rotated us onto a new token.
  String presentedToken = chessComRememberMe;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setCookieJar(&sessionCookieJar);
  if (!http.begin(client, "https://www.chess.com/home")) {
    return false;
  }
  http.addHeader("Accept", "text/html,application/xhtml+xml");
  http.addHeader("User-Agent", USER_AGENT);
  http.setTimeout(10000);

  // /home answers 302 for a signed-out request, and the redirect target is
  // the only reliable way to tell "remember-me accepted" from
  // "remember-me refused" - both can come back as a 302 carrying
  // Set-Cookie headers, so the status code alone says nothing.
  static const char *LOCATION_HEADER = "Location";
  http.collectHeaders(&LOCATION_HEADER, 1);

  int code = http.GET();
  String location = http.header("Location");
  http.end();  // response fully read by now - any Set-Cookie already folded into sessionCookieJar

  if (code <= 0) {
    Serial.printf("[ChessAPI] Session renewal request failed outright (%d) - network/timeout issue, not a cookie problem.\n", code);
    return false;
  }

  Serial.printf("[ChessAPI] Session renewal returned HTTP %d%s\n", code,
                location.length() ? (" -> " + location).c_str() : "");

  syncJarToGlobalsIfChanged();
  if (presentedToken == chessComRememberMe && chessComRememberMe[0] != '\0') {
    // Worth noticing: a genuine remember-me renewal normally hands back a
    // rotated token. Getting the same one back means this response didn't
    // really re-authenticate us, whatever its status code was.
    Serial.println("[ChessAPI] Note: remember-me token was NOT rotated by this response.");
  }

  // Bounced to the login page = the token was refused. Keeping it around
  // is actively harmful: CHESSCOM_REMEMBERME is single-use, so re-sending
  // one the server has already rotated past looks like a stolen token
  // rather than an expired one, and the usual defence is to drop every
  // session for the account - which would keep killing sessions the user
  // pastes in by hand, exactly the "I pasted a fresh cookie and it died
  // again a few games later" symptom. Drop it and say plainly that BOTH
  // cookies have to be recaptured.
  if (location.indexOf("login") >= 0) {
    Serial.println("[ChessAPI] Renewal was refused (redirected to login) - this CHESSCOM_REMEMBERME is dead.");
    Serial.println("[ChessAPI] Discarding it so it can't be replayed. Recapture BOTH cookies in the webadmin.");
    chessComRememberMe[0] = '\0';
    refreshSessionCookies();
    persistSessionCookies();
    return false;
  }

  return true;  // "attempted and wasn't refused" - the caller's retry is the real success check
}

bool fetchActiveGame(GameInfo &game, char *statusOut, size_t statusOutLen) {
  if (WiFi.status() != WL_CONNECTED) {
    snprintf(statusOut, statusOutLen, "No WiFi");
    return false;
  }

  seedCookieJarIfNeeded();

  // No session at all yet - either the user just saved a new
  // CHESSCOM_REMEMBERME (which clears PHPSESSID on purpose) or this is a
  // first boot. Mint one straight away instead of spending a guaranteed
  // 401 to discover what we already know.
  if (phpsessid[0] == '\0' && chessComRememberMe[0] != '\0') {
    Serial.println("[ChessAPI] No PHPSESSID stored - minting one from CHESSCOM_REMEMBERME before polling.");
    renewSession();
  }

  if (phpsessid[0] == '\0' && chessComRememberMe[0] == '\0') {
    snprintf(statusOut, statusOutLen, "No chess.com cookie set");
    return false;
  }

  String body;
  bool gotBody = false;

  // Up to 2 attempts: if the first hits 401/403, try a session renewal
  // (see renewSession() above) and retry ONCE with whatever fresh
  // PHPSESSID that produced. Doesn't loop further - a renewal failure
  // means CHESSCOM_REMEMBERME itself needs recapturing, not a retry.
  for (int attempt = 0; attempt < 2 && !gotBody; attempt++) {
    WiFiClientSecure client;
    client.setInsecure();  // TODO: swap for a real Root CA once the flow is validated

    HTTPClient http;
    http.setCookieJar(&sessionCookieJar);
    if (!http.begin(client, GAMES_ENDPOINT_URL)) {
      snprintf(statusOut, statusOutLen, "begin() failed");
      return false;
    }
    http.addHeader("Accept", "application/json");
    http.addHeader("User-Agent", USER_AGENT);
    http.setTimeout(10000);

    int httpCode = http.GET();

    // Capture/persist any cookie the server rotated on THIS response,
    // before doing anything else with the result - see
    // syncJarToGlobalsIfChanged() for why losing one is what kills the
    // session (and, for the remember-me token, what can invalidate every
    // session on the account).
    syncJarToGlobalsIfChanged();

    if (httpCode <= 0) {
      snprintf(statusOut, statusOutLen, "Timeout/conn error (%d)", httpCode);
      http.end();
      return false;
    }

    if (httpCode == 401 || httpCode == 403) {
      http.end();
      if (attempt == 0 && renewSession()) {
        // A retry immediately after renewal (same second) was observed
        // to still 401 once, even though the new PHPSESSID demonstrably
        // worked moments later (confirmed on a subsequent boot with the
        // exact same cookie value) - looks like chess.com's session
        // store needs a brief moment to propagate a freshly-issued
        // session before it's readable by whichever backend handles the
        // very next request. A short pause before retrying avoids
        // depending on a slow poll cycle (or a reboot) to happen to
        // outlast that propagation window.
        Serial.println("[ChessAPI] Retrying /service/play/games after session renewal...");
        delay(1500);
        continue;
      }
      snprintf(statusOut, statusOutLen, "HTTP %d (session expired - recapture PHPSESSID/CHESSCOM_REMEMBERME)", httpCode);
      Serial.println("[ChessAPI] 401/403 and renewal via CHESSCOM_REMEMBERME didn't help - both cookies need recapturing in the browser.");
      return false;
    }

    if (httpCode != 200) {
      snprintf(statusOut, statusOutLen, "HTTP %d", httpCode);
      http.end();
      return false;
    }

    body = http.getString();
    http.end();
    gotBody = true;
  }

  if (!gotBody) {
    return false;  // shouldn't be reachable, but don't fall through into stale-data parsing if it ever is
  }

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

bool testChessComSession() {
  GameInfo dummy;
  char status[80];
  if (fetchActiveGame(dummy, status, sizeof(status))) return true;
  // fetchActiveGame() returns false both for a genuinely dead session
  // (401/403/no cookie/timeout/...) and for a perfectly valid one that
  // simply has no game running right now ("OK (no game)") - only the
  // latter counts as a working session here.
  return strncmp(status, "OK", 2) == 0;
}
