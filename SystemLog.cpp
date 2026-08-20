#include "SystemLog.h"
#include <time.h>
#include <stdarg.h>

// Generous enough that the longest actual call sites (the webadmin-save
// summary line, which can list several changed fields plus a redacted
// token preview) don't get silently truncated mid-word.
#define SYSLOG_LINE_MAX_LEN 160

static char logLines[SYSLOG_MAX_LINES][SYSLOG_LINE_MAX_LEN];
static int logCount = 0;      // how many lines actually written yet (caps at SYSLOG_MAX_LINES)
static int logNextSlot = 0;   // circular write cursor

void systemLog(const char *fmt, ...) {
  char msg[SYSLOG_LINE_MAX_LEN];

  char timePrefix[16] = "??:??:?? ";
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 0)) {
    strftime(timePrefix, sizeof(timePrefix), "%H:%M:%S ", &timeinfo);
  }
  size_t prefixLen = strlen(timePrefix);
  strncpy(msg, timePrefix, sizeof(msg) - 1);

  va_list args;
  va_start(args, fmt);
  vsnprintf(msg + prefixLen, sizeof(msg) - prefixLen, fmt, args);
  va_end(args);
  msg[sizeof(msg) - 1] = '\0';

  strncpy(logLines[logNextSlot], msg, SYSLOG_LINE_MAX_LEN - 1);
  logLines[logNextSlot][SYSLOG_LINE_MAX_LEN - 1] = '\0';
  logNextSlot = (logNextSlot + 1) % SYSLOG_MAX_LINES;
  if (logCount < SYSLOG_MAX_LINES) logCount++;

  Serial.println(msg);
}

// Minimal HTML-escaping - values that end up in log lines (WiFi SSID,
// usernames, opponent names) are arbitrary user/remote-supplied text, not
// firmware-controlled constants, so they could contain '<'/'>'/'&' and
// this is rendered straight into the page. Webadmin is behind Basic Auth
// (not a cross-user surface), but escaping costs nothing and avoids a
// self-inflicted broken/stored-script page from an oddly-named WiFi
// network or chess.com display name.
static void appendEscaped(String &html, const char *raw) {
  for (const char *p = raw; *p; p++) {
    switch (*p) {
      case '<': html += "&lt;"; break;
      case '>': html += "&gt;"; break;
      case '&': html += "&amp;"; break;
      default: html += *p; break;
    }
  }
}

void redactedPreview(const char *value, char *out, size_t outLen) {
  size_t len = strlen(value);
  if (len == 0) {
    snprintf(out, outLen, "(empty)");
  } else if (len <= 8) {
    snprintf(out, outLen, "(%u chars, hidden)", (unsigned)len);
  } else {
    snprintf(out, outLen, "%.4s..%.4s (%u chars)", value, value + len - 4, (unsigned)len);
  }
}

void appendSystemLogHtml(String &html) {
  if (logCount == 0) {
    html += "<div class='row'><small>Nothing logged yet this boot.</small></div>";
    return;
  }
  html += "<pre style='white-space:pre-wrap;word-break:break-word;font-size:0.8rem;line-height:1.5;margin:0'>";
  // Newest first - oldest buffer slot is at 'start', so walking i from
  // logCount-1 down to 0 visits newest-to-oldest.
  int start = (logCount < SYSLOG_MAX_LINES) ? 0 : logNextSlot;
  for (int i = logCount - 1; i >= 0; i--) {
    int idx = (start + i) % SYSLOG_MAX_LINES;
    appendEscaped(html, logLines[idx]);
    html += "\n";
  }
  html += "</pre>";
}
