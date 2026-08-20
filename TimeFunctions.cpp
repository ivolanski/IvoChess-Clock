#include "TimeFunctions.h"
#include "config.h"
#include "AdminPortal.h"  // gmtOffsetSec - webadmin-configurable, config.h's GMT_OFFSET_SEC is only the factory default

#include <Arduino.h>
#include <time.h>

void initTime() {
  configTime(gmtOffsetSec, DAYLIGHT_OFFSET_SEC, NTP_SERVER);

  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 5000)) {
    Serial.printf("[Time] Synced via NTP: %02d:%02d\n", timeinfo.tm_hour, timeinfo.tm_min);
  } else {
    Serial.println("[Time] Could not sync now - night mode stays off until it can.");
  }
}

bool isNightTime() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 0)) {
    return false;
  }

  int hour = timeinfo.tm_hour;

  if (NIGHT_MODE_START_HOUR <= NIGHT_MODE_END_HOUR) {
    return hour >= NIGHT_MODE_START_HOUR && hour < NIGHT_MODE_END_HOUR;
  } else {
    return hour >= NIGHT_MODE_START_HOUR || hour < NIGHT_MODE_END_HOUR;
  }
}
