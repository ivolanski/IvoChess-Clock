#include "OAuthPkce.h"

#include <string.h>
#include <mbedtls/sha256.h>
#include <mbedtls/base64.h>
#include <esp_random.h>

// RFC 7636's code_verifier charset is [A-Z a-z 0-9 - . _ ~]. Generating
// raw random bytes and base64url-encoding them (see base64UrlEncode
// below) lands safely inside that charset without needing a separate
// alphabet mapping - this is what most PKCE implementations do in
// practice. 32 random bytes -> 43-char base64url string, exactly at RFC
// 7636's 43-char minimum verifier length.
#define PKCE_RANDOM_BYTES 32

// mbedtls_base64_encode() produces STANDARD base64 (using '+', '/', and
// '=' padding) - PKCE (RFC 7636) requires base64URL ('-', '_', no
// padding). Translating in place after encoding is simpler than
// reimplementing the encoder, and this project's only caller of base64 is
// this file, so keeping the translation local here (rather than a
// general-purpose base64url utility elsewhere) is fine.
static void base64UrlEncode(const uint8_t *data, size_t len, char *out, size_t outSize) {
  size_t olen = 0;
  // Sized for the base64 of a 32-byte input (44 bytes with padding) -
  // both PKCE_RANDOM_BYTES and a SHA-256 digest are exactly 32 bytes, so
  // this comfortably covers every caller in this file.
  unsigned char raw[64];
  mbedtls_base64_encode(raw, sizeof(raw), &olen, data, len);

  size_t j = 0;
  for (size_t i = 0; i < olen && j < outSize - 1; i++) {
    char c = (char)raw[i];
    if (c == '+') {
      c = '-';
    } else if (c == '/') {
      c = '_';
    } else if (c == '=') {
      continue;  // strip padding entirely - base64url has none
    }
    out[j++] = c;
  }
  out[j] = '\0';
}

static void randomString(char *out, size_t outSize) {
  uint8_t raw[PKCE_RANDOM_BYTES];
  esp_fill_random(raw, sizeof(raw));
  base64UrlEncode(raw, sizeof(raw), out, outSize);
}

void pkceGenerate(PkceExchange &out) {
  randomString(out.verifier, sizeof(out.verifier));
  randomString(out.state, sizeof(out.state));

  // code_challenge = BASE64URL(SHA256(ASCII(code_verifier))) - RFC 7636 S256 method.
  unsigned char digest[32];
  mbedtls_sha256((const unsigned char *)out.verifier, strlen(out.verifier), digest, /*is224=*/0);
  base64UrlEncode(digest, sizeof(digest), out.challenge, sizeof(out.challenge));
}
