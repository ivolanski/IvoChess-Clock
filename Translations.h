#ifndef TRANSLATIONS_H
#define TRANSLATIONS_H

enum Language {
  LANG_EN = 0,
  LANG_PT = 1,
};

enum StringId {
  STR_WAITING_FOR_GAME = 0,
  STR_LAST_RESULT,
  STR_MOVE,
  STR_APP_NAME,
  STR_WIFI,
  STR_SIGNAL,
  STR_IP,
  STR_API,
  STR_SETUP_MODE,
  STR_NOT_CONNECTED,
  STR_DISCONNECTED,
  STR_POOR,
  STR_GOOD,
  STR_EXCELLENT,
  STR_YOU_WON,
  STR_YOU_LOST,
  STR_DRAW,
  STR_WIFI_CONNECTING,
  STR_SESSION_EXPIRED,
  STR_CONNECTION_ERROR,
  STR_RESTART_FOR_GAME,
  STR_COUNT  // always last - marks the size of the tables
};

extern Language currentLanguage;
const char *T(StringId id);

#endif  // TRANSLATIONS_H
