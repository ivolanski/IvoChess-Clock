#ifndef LED_FUNCTIONS_H
#define LED_FUNCTIONS_H

#include "ClockState.h"

void initLEDs();
void updateLEDs(const ClockState &state);
void setAllLEDs(uint8_t r, uint8_t g, uint8_t b);
void clearLEDs();

#endif  // LED_FUNCTIONS_H
