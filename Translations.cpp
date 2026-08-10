#include "Translations.h"

Language currentLanguage = LANG_EN;

static const char *STRINGS_EN[STR_COUNT] = {
  "Waiting for game",
  "Last result:",
  "Move",
  "IvoChess Clock",
  "WiFi:",
  "Signal:",
  "IP:",
  "API:",
  "Setup mode",
  "Not connected",
  "Disconnected",
  "Poor",
  "Good",
  "Excellent",
  "You WON",
  "You LOST",
  "Draw",
  "Connecting to WiFi...",
  "Session expired",
  "Connection error",
  "Restart for new game",
  "Opp",
};

static const char *STRINGS_PT[STR_COUNT] = {
  "Aguardando partida",
  "Ultimo resultado:",
  "Lance",
  "IvoChess Clock",
  "WiFi:",
  "Sinal:",
  "IP:",
  "API:",
  "Modo config.",
  "Nao conectado",
  "Desconectado",
  "Fraco",
  "Bom",
  "Excelente",
  "Voce VENCEU",
  "Voce PERDEU",
  "Empate",
  "Conectando ao WiFi...",
  "Sessao expirada",
  "Erro de conexao",
  "Reinicie p/ nova partida",
  "Adv",
};

const char *T(StringId id) {
  if (id < 0 || id >= STR_COUNT) return "?";
  switch (currentLanguage) {
    case LANG_PT: return STRINGS_PT[id];
    case LANG_EN:
    default: return STRINGS_EN[id];
  }
}
