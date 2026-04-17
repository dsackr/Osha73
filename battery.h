#ifndef BATTERY_H
#define BATTERY_H

#include "globals.h"

bool max17048Read16(uint8_t reg, uint16_t &out);
float batteryVoltage();
float batterySOC();
float batteryCRatePctPerHour();
String batteryStateString(float cratePctPerHr);
bool shouldOverwriteLowBattery();
uint8_t applyLowBatteryOverlayNibble(int x, int y, uint8_t nibble);

#endif
