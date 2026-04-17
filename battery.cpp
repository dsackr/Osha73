#include "battery.h"

bool max17048Read16(uint8_t reg, uint16_t &out) {
  Wire.beginTransmission(MAX17048_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)MAX17048_ADDR, 2) != 2) return false;
  uint8_t msb = Wire.read();
  uint8_t lsb = Wire.read();
  out = ((uint16_t)msb << 8) | lsb;
  return true;
}

float batteryVoltage() {
  if (!fuelGaugeOk) return 0.0f;
  uint16_t raw = 0;
  if (!max17048Read16(REG_VCELL, raw)) return 0.0f;
  return (float)raw * 78.125f / 1000000.0f;
}

float batterySOC() {
  if (!fuelGaugeOk) return 0.0f;
  uint16_t raw = 0;
  if (!max17048Read16(REG_SOC, raw)) return 0.0f;
  return (float)raw / 256.0f;
}

float batteryCRatePctPerHour() {
  if (!fuelGaugeOk) return 0.0f;
  uint16_t rawU = 0;
  if (!max17048Read16(REG_CRATE, rawU)) return 0.0f;
  int16_t raw = (int16_t)rawU;
  return (float)raw * 0.208f;
}

String batteryStateString(float cratePctPerHr) {
  if (!fuelGaugeOk) return "unknown";
  if (cratePctPerHr > 0.8f) return "charging";
  if (cratePctPerHr < -0.8f) return "discharging";
  return "idle";
}

bool shouldOverwriteLowBattery() {
  if (!fuelGaugeOk) return false;
  int pct = (int)(batterySOC() + 0.5f);
  return pct <= LOW_BAT_OVERWRITE_PCT;
}

static inline bool iconPixelOn(int lx, int ly) {
  if (lx >= 0 && lx <= 21 && (ly == 0 || ly == ICON_H - 1)) return true;
  if ((lx == 0 || lx == 21) && ly >= 0 && ly < ICON_H) return true;
  if (lx >= 22 && lx <= 25 && (ly == 4 || ly == 7)) return true;
  if (lx == 25 && ly >= 4 && ly <= 7) return true;
  if (lx == 10 && ly >= 3 && ly <= 8) return true;
  if (lx == 10 && ly == 10) return true;
  return false;
}

uint8_t applyLowBatteryOverlayNibble(int x, int y, uint8_t nibble) {
  if (!shouldOverwriteLowBattery()) return nibble;

  int x0 = EPD_WIDTH - ICON_MARGIN - ICON_W;
  int y0 = ICON_MARGIN;

  int lx = x - x0;
  int ly = y - y0;

  if (lx < 0 || ly < 0 || lx >= ICON_W || ly >= ICON_H) return nibble;
  if (iconPixelOn(lx, ly)) return EPD_7IN3F_RED;
  return nibble;
}
