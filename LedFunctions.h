#ifndef LED_FUNCTIONS_H
#define LED_FUNCTIONS_H

#include "ClockState.h"

void initLEDs();
void updateLEDs(const ClockState &state);
void setAllLEDs(uint8_t r, uint8_t g, uint8_t b);
void clearLEDs();

// Changes how many pixels of the strip are actually driven, applied
// immediately (no reboot needed) via Adafruit_NeoPixel's own
// updateLength() - lets the webadmin's "Number of LEDs" field (AdminPortal.
// cpp) match whatever physical strip length a given build actually used,
// instead of that being a compile-time-only config.h constant. Clamped
// internally to a sane range regardless of what the caller passes in.
void setLedCount(uint16_t count);

// Admin portal "Test LED colors" button - cycles the strip through up to 7
// colors (whatever's currently typed into the LED colors form, even if not
// saved yet), one at a time, then hands control back to updateLEDs()'s
// normal state-driven logic. Non-blocking: just records the sequence,
// updateLEDs() (already called every second from loop()) advances it -
// same reasoning as SoundFunctions.cpp's dedicated task, applied to the
// existing 1Hz cadence instead of a new one, since that's all this needs.
void startLedTest(const uint8_t colors[][3], int count);

#endif  // LED_FUNCTIONS_H
