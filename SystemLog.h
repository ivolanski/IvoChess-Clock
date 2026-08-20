#ifndef SYSTEM_LOG_H
#define SYSTEM_LOG_H

#include <Arduino.h>

// In-memory-only event log for the webadmin's Logs tab - deliberately NOT
// persisted to flash (see systemLog() in SystemLog.cpp for why: this is
// for "what has this device been doing", not "what happened right before
// a power cycle" - that narrower, reboot-surviving question is already
// answered by the targeted forensic keys in ChessApiFunctions.cpp
// (getLastChessComSessionFailure()/getLastGoodCookieWrite()), which stay
// the right tool for that job specifically because they write to flash
// rarely, not on every event.
//
// Deliberately excluded from this log: anything that fires on a routine
// timer/poll with nothing interesting to say - e-paper refreshes, LED
// color updates, the ~5s chess.com poll when nothing changed, etc. Only
// log a genuine state transition or user-initiated action.
#define SYSLOG_MAX_LINES 100

// Appends one line, wall-clock timestamped (falls back to "??:??:??" if
// NTP hasn't synced yet - still useful as an ordered sequence even
// without an absolute time). printf-style; also mirrors to Serial so
// there's still exactly one call site per event, not two.
void systemLog(const char *fmt, ...);

// Renders the current buffer, oldest first, one line per row, into the
// provided String (caller owns building the rest of the page around it).
void appendSystemLogHtml(String &html);

// Shared redaction helper for secrets that need to appear in a log line
// as "yes, something changed" without ever printing the actual value -
// CHESSCOM_REMEMBERME, webadmin/WiFi passwords. Short values (<=8 chars,
// covers most passwords) just report a length; longer ones (tokens) show
// a first4..last4 fingerprint, enough to notice "this is the same value
// as before" or "this rotated" across log lines without exposing anything
// usable to log in with.
void redactedPreview(const char *value, char *out, size_t outLen);

#endif  // SYSTEM_LOG_H
