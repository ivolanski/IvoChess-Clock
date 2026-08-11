#include "SoundFunctions.h"
#include "config.h"
#include "AdminPortal.h"

#include <string.h>
#include <stdlib.h>

// NOTE: uses the pin-based LEDC API (ledcAttach()/ledcWriteTone() taking a
// GPIO pin directly) introduced in arduino-esp32 core 3.x - the current
// "install from Boards Manager" default (see website/build.html, no version
// pinned). Older 2.x cores need the channel-based ledcSetup()/
// ledcAttachPin()/ledcWriteTone(channel, ...) API instead.

#define SOUND_QUEUE_LEN 4

static QueueHandle_t soundQueue = nullptr;

const char *soundDefaultMelody(SoundEvent evt) {
  switch (evt) {
    case SOUND_GAME_START:    return DEFAULT_SOUND_GAME_START;
    case SOUND_MOVE_OPPONENT: return DEFAULT_SOUND_MOVE_OPPONENT;
    case SOUND_MOVE_OWN:      return DEFAULT_SOUND_MOVE_OWN;
    case SOUND_CHECK:         return DEFAULT_SOUND_CHECK;
    case SOUND_GAME_WIN:      return DEFAULT_SOUND_GAME_WIN;
    case SOUND_GAME_LOSS:     return DEFAULT_SOUND_GAME_LOSS;
    case SOUND_GAME_DRAW:     return DEFAULT_SOUND_GAME_DRAW;
    case SOUND_GAME_END:
    default:                  return DEFAULT_SOUND_GAME_END;
  }
}

// Plays one "freqHz:durationMs,freqHz:durationMs,..." melody string,
// freq=0 = rest (silence for that duration). Runs entirely on the sound
// task - vTaskDelay() here only blocks THIS task, never loop()/WiFi/BLE.
static void playMelodyBlocking(char *melody) {
  char *saveptr = nullptr;
  char *note = strtok_r(melody, ",", &saveptr);
  while (note != nullptr) {
    char *colon = strchr(note, ':');
    long freq = atol(note);
    long dur = colon ? atol(colon + 1) : 0;
    if (freq > 0 && dur > 0) {
      ledcWriteTone(SPEAKER_PIN, (uint32_t)freq);
    } else {
      ledcWriteTone(SPEAKER_PIN, 0);
    }
    if (dur > 0) {
      vTaskDelay(pdMS_TO_TICKS(dur));
    }
    note = strtok_r(nullptr, ",", &saveptr);
  }
  ledcWriteTone(SPEAKER_PIN, 0);  // silence between/after melodies
}

static void soundTask(void *param) {
  char melody[SOUND_MELODY_MAX_LEN];
  for (;;) {
    if (xQueueReceive(soundQueue, melody, portMAX_DELAY) == pdTRUE) {
      playMelodyBlocking(melody);
    }
  }
}

void initSoundTask() {
  ledcAttach(SPEAKER_PIN, 2000, 10);  // 2kHz initial (overwritten per-note), 10-bit resolution
  ledcWriteTone(SPEAKER_PIN, 0);      // silent until the first real event

  soundQueue = xQueueCreate(SOUND_QUEUE_LEN, SOUND_MELODY_MAX_LEN);
  xTaskCreate(soundTask, "sound", 3072, nullptr, 1, nullptr);
}

void playSoundEvent(SoundEvent evt) {
  if (evt < 0 || evt >= SOUND_EVENT_COUNT || !soundQueue) return;
  if (!soundEnabled[evt]) return;

  const char *melody = (soundMelodyOverride[evt][0] != '\0')
                            ? soundMelodyOverride[evt]
                            : soundDefaultMelody(evt);

  char buf[SOUND_MELODY_MAX_LEN];
  strncpy(buf, melody, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';

  // Non-blocking send, 0 timeout - if the queue is full (several events
  // fired within the same second) the oldest-still-queued sounds simply
  // finish playing and this one is dropped rather than piling up delay;
  // same "shouldn't happen at this traffic rate" tolerance as
  // ChessConnectBLE.cpp's event queue.
  xQueueSend(soundQueue, buf, 0);
}
