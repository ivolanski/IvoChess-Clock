#ifndef OAUTH_PKCE_H
#define OAUTH_PKCE_H

#include <Arduino.h>

// Fits a base64url encoding of 32 random bytes (43 chars, right at RFC
// 7636's minimum code_verifier length) or of a 32-byte SHA-256 digest -
// both land in the low 40s, this leaves headroom without wasting much RAM.
#define PKCE_STRING_MAX_LEN 64

// One PKCE (RFC 7636) + CSRF exchange, generated fresh per authorization
// attempt. Provider-agnostic on purpose - this file knows nothing about
// chess.com or Lichess, just the PKCE math and a random CSRF token, so it
// can be reused if/when a chess.com OAuth application (separately
// pending) is approved.
//
// RAM-only by design: the whole flow (device renders an authorize link ->
// user's browser goes to the provider -> consent -> browser redirected
// back to the device) is expected to complete within the same boot, so
// nothing here needs to survive a reboot or even outlive the webadmin
// process that generated it.
struct PkceExchange {
  char verifier[PKCE_STRING_MAX_LEN];   // RFC 7636 code_verifier - kept on the device, sent only in the final token exchange
  char challenge[PKCE_STRING_MAX_LEN];  // base64url(SHA256(verifier)) - sent in the authorize URL, safe to expose
  char state[PKCE_STRING_MAX_LEN];      // random CSRF token - sent in the authorize URL, must match what the callback receives
};

// Fills 'out' with a fresh, cryptographically random verifier/challenge/
// state triple, using the ESP32's hardware RNG (esp_fill_random) - not
// Arduino's random()/rand(), which are not suitable for anything
// security-sensitive like this.
void pkceGenerate(PkceExchange &out);

#endif  // OAUTH_PKCE_H
