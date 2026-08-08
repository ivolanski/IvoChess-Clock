#ifndef LICHESS_API_FUNCTIONS_H
#define LICHESS_API_FUNCTIONS_H

#include "ClockState.h"
#include "OAuthPkce.h"

// Lichess's equivalent of chess.com's GameInfo (ChessApiFunctions.h) -
// deliberately a SEPARATE struct, not a reuse: chess.com's carries
// rsocketUrl/watchRoute/legacyId fields that mean nothing here. Lichess
// also tells us our own color directly (GET /api/account/playing's
// "color" field, verified against the real API schema), so unlike
// chess.com there's no need to match a configured username against the
// two players to figure out which side is "me".
struct LichessGameInfo {
  char id[16];          // Lichess game IDs are 8 chars - headroom for safety
  bool myColorIsWhite;   // from "color" - which side I'm playing
  PlayerInfo opponent;    // username/rating from the "opponent" object; clockMs unused (this endpoint doesn't carry live clocks, same as chess.com's discovery poll)
  int myRating;           // top-level "rating" field - my own rating in this game
};

// ---- OAuth (PKCE) ----

// Builds the full Lichess authorize URL for a fresh PkceExchange, using
// 'redirectUri' as given. 'redirectUri' must be built by the caller from
// the actual request host (see AdminPortal.cpp) - never hardcoded, since
// PKCE requires the exact same redirect_uri string here and in the token
// exchange below.
String lichessBuildAuthorizeUrl(const PkceExchange &pkce, const String &redirectUri);

// Exchanges an authorization 'code' for an access token (POST to
// LICHESS_TOKEN_URL), using the same 'redirectUri' and the 'verifier'
// from the PkceExchange that produced the authorize URL. On success,
// fills 'tokenOut' and returns true. Never logs the code or the token -
// only lengths/success, matching this project's existing
// credential-redaction discipline (see ChessApiFunctions.cpp).
bool lichessExchangeCodeForToken(const char *code, const char *verifier, const String &redirectUri,
                                  char *tokenOut, size_t tokenOutLen);

// Fetches the token owner's username via GET /api/account (the token
// exchange response itself doesn't include it). Fills usernameOut.
bool lichessFetchUsername(const char *token, char *usernameOut, size_t usernameOutLen);

// ---- game discovery ----

// Queries GET /api/account/playing using the saved Lichess token. If
// there's an active game, fills 'game' with the FIRST one in
// "nowPlaying" and returns true. 'statusOut' is ALWAYS filled (e.g.
// "OK (1 game)", "OK (no game)", "HTTP 401 (token revoked...)",
// "No WiFi") - same contract as chess.com's fetchActiveGame()
// (ChessApiFunctions.h), so GameDataSource.cpp can treat both
// symmetrically.
bool lichessFetchActiveGame(const char *token, LichessGameInfo &game,
                             char *statusOut, size_t statusOutLen);

#endif  // LICHESS_API_FUNCTIONS_H
