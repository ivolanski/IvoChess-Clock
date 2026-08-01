#ifndef BATTERY_FUNCTIONS_H
#define BATTERY_FUNCTIONS_H

#include "ClockState.h"

void initBatteryADC();
void updateBatteryInfo(ClockState &state);

#endif  // BATTERY_FUNCTIONS_H
