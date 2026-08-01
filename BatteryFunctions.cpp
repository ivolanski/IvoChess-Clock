#include "BatteryFunctions.h"
#include "config.h"

#include <Arduino.h>

void initBatteryADC() {
  analogReadResolution(12);
}

void updateBatteryInfo(ClockState &state) {
  int raw = analogRead(BATTERY_PIN);
  float adcVoltage = (raw / ADC_RESOLUTION) * ADC_REF_VOLTAGE;
  float batteryVoltage = adcVoltage / VOLTAGE_DIVIDER_RATIO;

  float fraction = (batteryVoltage - BATTERY_EMPTY) / (BATTERY_FULL - BATTERY_EMPTY);
  int percentage = (int)(fraction * 100.0f);
  percentage = constrain(percentage, 0, 100);

  state.batteryVoltage = batteryVoltage;
  state.batteryPercentage = percentage;
}
