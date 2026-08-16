#include "SoundFunctions.h"
#include "config.h"
#include "AdminPortal.h"

#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <math.h>

// NOTE: uses the pin-based LEDC API (ledcAttach()/ledcWriteTone() taking a
// GPIO pin directly) introduced in arduino-esp32 core 3.x - the current
// "install from Boards Manager" default (see website/build.html, no version
// pinned). Older 2.x cores need the channel-based ledcSetup()/
// ledcAttachPin()/ledcWriteTone(channel, ...) API instead.

#define SOUND_QUEUE_LEN 4

static QueueHandle_t soundQueue = nullptr;

// Plays 'freq' (0 = silence) at the current soundVolume (AdminPortal.cpp,
// 0-100, persisted) by scaling the PWM duty cycle instead of always using
// ledcWriteTone()'s fixed 50% - see config.h's SPEAKER_PWM_RESOLUTION_BITS
// comment for why this is how volume/mute work on this bare-piezo hardware.
// 1-100 is spread evenly across SOUND_VOLUME_MIN_DB..0dB (config.h)
// rather than mapped straight onto duty, so every adjacent 1% step is
// the same ratio louder than the last all the way up the slider - a
// linear duty ramp instead makes every low setting (e.g. 2% vs 1%)
// sound disproportionately louder than its neighbor, because forcing a
// smooth curve to touch exactly zero at 0% unavoidably makes it behave
// near-linearly right at the bottom, whatever curve shape is used. 0%
// is therefore its own hard-mute case, not part of the dB curve.
static void speakerTone(uint32_t freq) {
  if (freq == 0) {
    ledcWrite(SPEAKER_PIN, 0);
    return;
  }
  ledcChangeFrequency(SPEAKER_PIN, freq, SPEAKER_PWM_RESOLUTION_BITS);
  uint32_t maxDuty = (1u << SPEAKER_PWM_RESOLUTION_BITS) - 1;
  uint32_t volume = soundVolume > 100 ? 100 : soundVolume;
  uint32_t duty;
  if (volume == 0) {
    duty = 0;
  } else {
    float volumeFraction = volume / 100.0f;
    float dB = SOUND_VOLUME_MIN_DB * (1.0f - volumeFraction);  // 100% -> 0dB, 1% -> nearly SOUND_VOLUME_MIN_DB
    float gain = powf(10.0f, dB / 20.0f);
    duty = (uint32_t)((maxDuty / 2) * gain + 0.5f);  // 100% volume = the old fixed 50% duty (max loudness)
  }
  ledcWrite(SPEAKER_PIN, duty);
}

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

// Semitone offset from A4 (440Hz) for each natural note letter, used by
// noteFrequency() below. Matches standard equal-temperament tuning.
static int8_t noteSemitoneFromA(char letter) {
  switch (letter) {
    case 'c': return -9;
    case 'd': return -7;
    case 'e': return -5;
    case 'f': return -4;
    case 'g': return -2;
    case 'a': return 0;
    case 'b': return 2;
    default:  return 0;
  }
}

static uint32_t noteFrequency(char letter, bool sharp, int octave) {
  int semitone = noteSemitoneFromA(letter) + (sharp ? 1 : 0) + (octave - 4) * 12;
  return (uint32_t)(440.0 * pow(2.0, semitone / 12.0) + 0.5);
}

// A melody string is treated as RTTTL if it contains '=' (the "d=..,o=..,
// b=.." defaults section) - the legacy freqHz:durationMs format never does,
// so this cleanly distinguishes the two with no format flag needed.
static bool isRtttl(const char *melody) {
  return strchr(melody, '=') != nullptr;
}

// Plays an RTTTL ringtone string - "name:d=4,o=5,b=125:8e6,8d6,f#,...".
// Standard format used by countless free ringtone melodies online (search
// "RTTTL ringtones"): paste one straight into the admin portal's melody
// field. Runs entirely on the sound task, same as the legacy parser below.
static void playRtttlBlocking(char *rtttl) {
  char *defaults = strchr(rtttl, ':');
  if (!defaults) {
    speakerTone(0);
    return;
  }
  defaults++;  // past the name

  char *notes = strchr(defaults, ':');
  if (notes) {
    *notes = '\0';
    notes++;
  } else {
    // Malformed (no notes separator) - tolerate by treating everything
    // after the first colon as the note list, using spec defaults.
    notes = defaults;
    defaults = nullptr;
  }

  int defDuration = 4, defOctave = 5, defBpm = 63;
  if (defaults) {
    char *saveptr = nullptr;
    char *tok = strtok_r(defaults, ",", &saveptr);
    while (tok) {
      if (tok[0] == 'd' && tok[1] == '=') defDuration = atoi(tok + 2);
      else if (tok[0] == 'o' && tok[1] == '=') defOctave = atoi(tok + 2);
      else if (tok[0] == 'b' && tok[1] == '=') defBpm = atoi(tok + 2);
      tok = strtok_r(nullptr, ",", &saveptr);
    }
  }
  if (defDuration <= 0) defDuration = 4;
  if (defOctave <= 0) defOctave = 5;
  if (defBpm <= 0) defBpm = 63;

  double wholeNoteMs = 240000.0 / defBpm;  // quarter note = 60000/bpm; whole = *4

  char *saveptr2 = nullptr;
  char *note = strtok_r(notes, ",", &saveptr2);
  while (note != nullptr) {
    char *c = note;
    while (*c == ' ') c++;  // tolerate stray whitespace from pasted text

    int duration = 0;
    while (isdigit((unsigned char)*c)) { duration = duration * 10 + (*c - '0'); c++; }
    if (duration <= 0) duration = defDuration;

    bool isPause = (*c == 'p' || *c == 'P');
    char letter = (char)tolower((unsigned char)*c);
    if (*c != '\0') c++;

    bool sharp = false;
    if (*c == '#') { sharp = true; c++; }

    int octave = 0;
    while (isdigit((unsigned char)*c)) { octave = octave * 10 + (*c - '0'); c++; }
    if (octave <= 0) octave = defOctave;

    bool dotted = false;
    while (*c) { if (*c == '.') dotted = true; c++; }

    double ms = wholeNoteMs / duration;
    if (dotted) ms *= 1.5;

    uint32_t freq = isPause ? 0 : noteFrequency(letter, sharp, octave);
    speakerTone((freq > 0 && ms > 0) ? freq : 0);
    if (ms > 0) {
      vTaskDelay(pdMS_TO_TICKS((uint32_t)ms));
    }

    note = strtok_r(nullptr, ",", &saveptr2);
  }
  speakerTone(0);
}

// Plays one "freqHz:durationMs,freqHz:durationMs,..." melody string,
// freq=0 = rest (silence for that duration). Runs entirely on the sound
// task - vTaskDelay() here only blocks THIS task, never loop()/WiFi/BLE.
static void playLegacyMelodyBlocking(char *melody) {
  char *saveptr = nullptr;
  char *note = strtok_r(melody, ",", &saveptr);
  while (note != nullptr) {
    char *colon = strchr(note, ':');
    long freq = atol(note);
    long dur = colon ? atol(colon + 1) : 0;
    if (freq > 0 && dur > 0) {
      speakerTone((uint32_t)freq);
    } else {
      speakerTone(0);
    }
    if (dur > 0) {
      vTaskDelay(pdMS_TO_TICKS(dur));
    }
    note = strtok_r(nullptr, ",", &saveptr);
  }
  speakerTone(0);  // silence between/after melodies
}

// Dispatches to the RTTTL or legacy parser depending on the melody string's
// format (see isRtttl() above) - both are accepted transparently, so
// existing compiled-in defaults keep working unchanged.
static void playMelodyBlocking(char *melody) {
  if (isRtttl(melody)) {
    playRtttlBlocking(melody);
  } else {
    playLegacyMelodyBlocking(melody);
  }
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
  ledcAttach(SPEAKER_PIN, 2000, SPEAKER_PWM_RESOLUTION_BITS);  // 2kHz initial (overwritten per-note)
  speakerTone(0);  // silent until the first real event

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

void playMelodyNow(const char *melody) {
  if (!soundQueue || !melody) return;

  char buf[SOUND_MELODY_MAX_LEN];
  strncpy(buf, melody, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';

  xQueueSend(soundQueue, buf, 0);
}
