#include "ChessApiFunctions.h"
#include "config.h"
#include "AdminPortal.h"
#include "SystemLog.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <time.h>

static const char *USER_AGENT =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36";

// Forensic record of the last time CHESSCOM_REMEMBERME got discarded,
// surviving a reboot (Serial history doesn't - the 2026-08-11 "session
// expired overnight" recurrence was only diagnosable by INFERRING the
// token was already empty at boot from an absence of log lines, since the
// actual refusal that cleared it had happened before this capture ever
// started and left no trace once the device was next power-cycled).
// Own small NVS namespace, separate from AdminPortal's PREFS_NAMESPACE
// settings blob, so this never collides with a settings key by accident.
#define SESSION_FAIL_PREFS_NAMESPACE "ivochess_dbg"
static void persistSessionFailure(const char *reason) {
  char whenStr[24] = "unknown time";
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 0)) {
    strftime(whenStr, sizeof(whenStr), "%Y-%m-%d %H:%M", &timeinfo);
  }
  Preferences dbgPrefs;
  dbgPrefs.begin(SESSION_FAIL_PREFS_NAMESPACE, /*readOnly=*/false);
  dbgPrefs.putString("last_fail_why", reason);
  dbgPrefs.putString("last_fail_when", whenStr);
  dbgPrefs.end();
}

bool getLastChessComSessionFailure(char *reasonOut, size_t reasonLen, char *whenOut, size_t whenLen) {
  Preferences dbgPrefs;
  dbgPrefs.begin(SESSION_FAIL_PREFS_NAMESPACE, /*readOnly=*/true);
  bool hasRecord = dbgPrefs.isKey("last_fail_why");
  if (hasRecord) {
    dbgPrefs.getString("last_fail_why", "").toCharArray(reasonOut, reasonLen);
    dbgPrefs.getString("last_fail_when", "").toCharArray(whenOut, whenLen);
  }
  dbgPrefs.end();
  return hasRecord;
}

// Companion to the failure record above, for a question a live serial
// capture genuinely cannot answer: a capture can only span the time the
// device is actually powered, so it can prove what happened right up to
// the moment power was cut, but never the power-off window itself (no
// power, no logging - not a tooling gap, a physical one). This sidesteps
// that entirely by stamping flash, not Serial, every time
// CHESSCOM_REMEMBERME is written with a real (non-empty) value - see the
// call in AdminPortal.cpp's persistSessionCookies(). Whenever a future
// "found empty at boot" failure fires, it can then report not just WHEN
// it was discovered empty but WHEN IT WAS LAST KNOWN GOOD, regardless of
// whether the gap between them was 40 minutes or 40 hours - works for a
// test spanning a full overnight power-off exactly as well as five
// minutes, since neither end of the measurement depends on anyone
// having a live serial session open at the right moment.
void recordGoodCookieWrite() {
  char whenStr[24] = "unknown time";
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 0)) {
    strftime(whenStr, sizeof(whenStr), "%Y-%m-%d %H:%M", &timeinfo);
  }
  Preferences dbgPrefs;
  dbgPrefs.begin(SESSION_FAIL_PREFS_NAMESPACE, /*readOnly=*/false);
  dbgPrefs.putString("last_good_when", whenStr);
  dbgPrefs.end();
}

static bool getLastGoodCookieWrite(char *whenOut, size_t whenLen) {
  Preferences dbgPrefs;
  dbgPrefs.begin(SESSION_FAIL_PREFS_NAMESPACE, /*readOnly=*/true);
  bool hasRecord = dbgPrefs.isKey("last_good_when");
  if (hasRecord) {
    dbgPrefs.getString("last_good_when", "").toCharArray(whenOut, whenLen);
  }
  dbgPrefs.end();
  return hasRecord;
}

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
// out. If that copy doesn't happen, a power cycle (the on/off switch cuts
// battery power) reseeds the jar from flash, i.e. from a value the server
// has since moved past - so it's worth keeping flash current even though
// presenting an older CHESSCOM_REMEMBERME is not reliably fatal by itself
// (see the 2026-08-19 note on renewSession()'s discard path below - a
// live retest that day showed the SAME token successfully re-authenticate
// twice, hours apart, contradicting the "single-use, replay = account-wide
// invalidation" assumption this comment used to make here).
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
      char preview[24];
      redactedPreview(chessComRememberMe, preview, sizeof(preview));
      systemLog("chess.com: remember-me rotated by server (%s)", preview);
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
// PHPSESSID via Set-Cookie - and usually also rotates CHESSCOM_REMEMBERME
// itself to a new token in the same response. No login form, no
// credentials beyond the one-time capture of this cookie - the same
// hands-off renewal a browser gets automatically.
//
// Whether the OLD token still works after that rotation is genuinely
// unclear, not "single-use" as earlier versions of this comment assumed.
// The cookie's own Set-Cookie carries Max-Age=31536000 (1 year), and a
// live test on 2026-08-19 reused an already-rotated-past token roughly two
// hours later and it authenticated again cleanly (fresh PHPSESSID, another
// rotation) - a real replay, not a guess. So don't assume presenting an
// older copy is instantly fatal; treat "refused" as evidence of that
// specific attempt, not proof the token is unrecoverably dead (see the
// two-strikes-before-discard logic below, which exists for exactly this
// reason).
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
enum RenewAttemptResult { RENEW_OK, RENEW_REFUSED, RENEW_TRANSPORT_FAILED };

static RenewAttemptResult renewSessionAttempt() {
  // Remember what we're about to present, so we can tell afterwards
  // whether the server actually rotated us onto a new token.
  String presentedToken = chessComRememberMe;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setCookieJar(&sessionCookieJar);
  if (!http.begin(client, "https://www.chess.com/home")) {
    return RENEW_TRANSPORT_FAILED;
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
    return RENEW_TRANSPORT_FAILED;
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

  if (location.indexOf("login") >= 0) {
    return RENEW_REFUSED;
  }
  return RENEW_OK;
}

// Chess.com sits behind Cloudflare (confirmed live - Server: cloudflare,
// cf-cache-status, etc. on every response), and this device's request
// doesn't look like a real browser in ways that matter to bot-detection
// (WiFiClientSecure's TLS stack has a different fingerprint than an
// actual Chrome/Edge TLS handshake, no Sec-Fetch-*/browser-only headers).
// A single ambiguous "redirected to login" could plausibly be a one-off
// WAF challenge rather than genuine proof the token is dead - retrying
// once before permanently discarding a token that two independently
// captured real-browser copies proved is otherwise perfectly valid
// (verified live 2026-08-11: a second, never-before-used browser's token
// authenticated successfully) costs one extra request and meaningfully
// lowers the odds of discarding a token that would have worked.
// Shared by both persistSessionFailure() call sites below (empty-at-entry
// and refused-twice-so-discarded) - a single guard, not one independent
// bool per branch. Real bug found 2026-08-20: with two separate guards,
// a genuine "refused twice, discarding" event correctly persisted its
// true reason, but the very next poll cycle (~5s later, GAME_POLL_INTERVAL_MS)
// hit the now-empty buffer, saw ITS OWN guard unset, and immediately
// overwrote that true reason with a generic "Empty at boot" one - hiding
// every refusal-triggered discard behind a misleading message, on every
// single occurrence, with no way to tell the two apart from the webadmin.
// One guard fixes it: whichever branch writes first each boot wins, and
// the other is correctly suppressed instead of clobbering it.
static bool alreadyLoggedFailureThisBoot = false;

static bool renewSession() {
  if (chessComRememberMe[0] == '\0') {
    // Silent until now - this branch firing with nothing else printed
    // around it is how we discovered CHESSCOM_REMEMBERME was already
    // empty on a "session expired overnight" recurrence (2026-08-11):
    // the log jumped straight from boot to the 401 failure with no
    // "renewal returned HTTP.../refused" line in between, which only
    // happens if renewSession() bailed out right here. Logging it removes
    // the need to infer that from an absence of other log lines next time.
    Serial.println("[ChessAPI] Renewal skipped - no CHESSCOM_REMEMBERME stored (already empty before this attempt).");
    // Persist this too, not just the refused-twice case below - otherwise
    // the webadmin's "Last drop" line stays blank and looks like nothing
    // ever happened, when in fact the cookie is sitting empty right now.
    // This exact gap is what made a real 2026-08-19 incident (device
    // rebooted with CHESSCOM_REMEMBERME already blank) look untraceable
    // from the webadmin alone. Cause still genuinely unconfirmed as of
    // 2026-08-20 - an interrupted flash write shortly after saving was
    // the leading guess, but a live retest that day waited 5+ minutes
    // between saving and powering off (ample time for any in-flight
    // write to settle) and it still reproduced, which doesn't fit a
    // narrow post-save race window. Not treating that guess as
    // confirmed - see recordGoodCookieWrite()/getLastGoodCookieWrite()
    // below, added specifically so the NEXT occurrence reports the gap
    // since the last known-good write instead of needing another guess.
    //
    // Guarded to once per boot (shared with the refused-twice branch
    // below - see alreadyLoggedFailureThisBoot's comment): this branch is
    // hit on every poll cycle (every GAME_POLL_INTERVAL_MS, currently 5s)
    // for as long as the cookie stays empty, and persistSessionFailure()
    // is a flash write - writing it every 5s indefinitely would be
    // needless wear, and ironically raises the odds of causing the exact
    // kind of interrupted-write corruption once suspected here.
    if (!alreadyLoggedFailureThisBoot) {
      alreadyLoggedFailureThisBoot = true;
      char lastGoodWhen[24] = "";
      char reasonBuf[64];
      if (getLastGoodCookieWrite(lastGoodWhen, sizeof(lastGoodWhen))) {
        snprintf(reasonBuf, sizeof(reasonBuf), "Empty at boot - last good write %s", lastGoodWhen);
      } else {
        snprintf(reasonBuf, sizeof(reasonBuf), "Empty at boot - never had a good write");
      }
      persistSessionFailure(reasonBuf);
      systemLog("chess.com: %s", reasonBuf);
    }
    return false;  // nothing to renew with - user needs to recapture the cookie (webadmin only has the one field - see handleSave())
  }

  for (int attempt = 0; attempt < 2; attempt++) {
    RenewAttemptResult result = renewSessionAttempt();
    if (result == RENEW_OK) return true;
    if (result == RENEW_TRANSPORT_FAILED) return false;  // network issue, not a cookie problem - don't burn the retry on it

    // RENEW_REFUSED
    if (attempt == 0) {
      Serial.println("[ChessAPI] Renewal refused - retrying once before concluding the token is actually dead (could be a one-off WAF/bot-check, not just a replayed token).");
      systemLog("chess.com: renewal refused, retrying once");
      delay(2000);
      continue;
    }

    // Bounced to the login page twice in a row. Earlier versions of this
    // comment asserted CHESSCOM_REMEMBERME is single-use and that
    // re-presenting an already-rotated one reads as token theft and drops
    // every session on the account - that was never independently
    // verified, and a live retest on 2026-08-19 directly contradicts the
    // "single-use" half of it (the same token re-authenticated cleanly
    // hours after its first use). What IS verified: the cookie's own
    // Set-Cookie carries a real 1-year Max-Age, so it's not meant to be
    // this fragile by design. Two refusals in a row still isn't proof of
    // WHY - could be a genuinely dead/superseded token (e.g. chess.com
    // issued a newer one to another session on the account, which would
    // orphan this copy without ever touching it), could be something else
    // entirely. Discarding after two strikes is a defensive choice, not a
    // confirmed diagnosis - keeping a token that fails twice in a row
    // around forever isn't better, but don't treat "why" as settled.
    Serial.println("[ChessAPI] Renewal was refused twice (redirected to login both times) - treating this CHESSCOM_REMEMBERME as dead.");
    Serial.println("[ChessAPI] Discarding it. Recapture the cookie in the webadmin (Connections tab - one field, PHPSESSID is derived automatically).");
    // Set the shared guard BEFORE clearing the buffer below - the very
    // next poll cycle will see chessComRememberMe empty and hit the
    // branch at the top of this function, which must NOT be allowed to
    // overwrite this genuinely more informative reason with a generic
    // "Empty at boot" one (see alreadyLoggedFailureThisBoot's comment).
    alreadyLoggedFailureThisBoot = true;
    persistSessionFailure("refused during renewal (twice)");
    systemLog("chess.com: renewal refused twice - discarding remember-me token");
    chessComRememberMe[0] = '\0';
    refreshSessionCookies();
    persistSessionCookies();
    return false;
  }
  return false;  // unreachable
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
      snprintf(statusOut, statusOutLen, "HTTP %d (session expired - recapture CHESSCOM_REMEMBERME in webadmin)", httpCode);
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
