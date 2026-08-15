#ifndef SOUND_FUNCTIONS_H
#define SOUND_FUNCTIONS_H

#include "ClockState.h"

// One entry per distinct event the admin portal lets the user toggle/
// customize independently (AdminPortal.cpp's soundEnabled[]/
// soundMelodyOverride[] are indexed by this same enum - see its comment for
// per-event availability caveats, e.g. SOUND_CHECK only ever fires for
// ChessConnect today).
enum SoundEvent {
  SOUND_GAME_START = 0,
  SOUND_MOVE_OPPONENT,
  SOUND_MOVE_OWN,
  SOUND_CHECK,
  SOUND_GAME_WIN,
  SOUND_GAME_LOSS,
  SOUND_GAME_DRAW,
  SOUND_GAME_END,
  SOUND_EVENT_COUNT
};

// Sets up the LEDC PWM output on SPEAKER_PIN and starts the dedicated
// FreeRTOS task that actually plays melodies. Call once from setup().
// Melody playback is task-based (not inline in loop()) for the same reason
// the display got its own task (DisplayFunctions.cpp) and BLE events get a
// queue (ChessConnectBLE.cpp): a melody plays over vTaskDelay()'d
// milliseconds per note, which would otherwise stall WiFi/BLE/the admin
// portal for the duration of every sound.
void initSoundTask();

// The compiled-in DEFAULT_SOUND_* (config.h) for 'evt' - exposed so
// AdminPortal.cpp can show it as the custom-melody field's placeholder.
const char *soundDefaultMelody(SoundEvent evt);

// Plays 'evt' if AdminPortal's soundEnabled[evt] is true, using
// soundMelodyOverride[evt] if non-empty or the compiled-in DEFAULT_SOUND_*
// (config.h) otherwise. Non-blocking - queues the melody for the sound task
// and returns immediately. Safe to call from any task (main loop or the BLE
// task - see ChessConnectBLE.h's threading note) since it only ever
// enqueues, never touches shared state directly.
void playSoundEvent(SoundEvent evt);

// Admin portal "Test" button - plays 'melody' immediately regardless of
// soundEnabled[] and without going through soundMelodyOverride[]/the
// compiled-in default, so a not-yet-saved (or currently muted) melody can
// be previewed exactly as typed. Same non-blocking queue as
// playSoundEvent() - safe to call from the admin portal's request handler.
void playMelodyNow(const char *melody);

#endif  // SOUND_FUNCTIONS_H
