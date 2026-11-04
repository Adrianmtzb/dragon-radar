#include "hw.h"
#include <Wire.h>
#include "config.h"
#include "lcd.h"

#if HAS_BATTERY && defined(PMIC_AXP2101) && PMIC_AXP2101
#define HW_PMIC 1
#define XPOWERS_CHIP_AXP2101
#include <XPowersLib.h>
static XPowersPMU pmu;
static bool pmuOk = false;
static HwPower pwr = {false, -1, -1, false, false, 0, 0, 0, "idle", 0};
#else
#define HW_PMIC 0
#endif

#if HAS_BATTERY
// Battery monitor. One reading per second; the percentage, the low flag and the charging state all
// come from the same sample so they never disagree with each other.
static int batMv = -1;               // filtered millivolts
static int batPct = -1;
static uint32_t batNextSampleMs = 0;
static bool batLow = false, batCharging = false, batAlarmPending = false;
static uint32_t batAlarmMs = 0;      // when the low alarm last fired

#if !HW_PMIC
// Trend window: one slot per BATTERY_TREND_STEP_MS, the oldest slot is one window behind.
static const uint8_t BAT_TREND_N = BATTERY_TREND_WINDOW_MS / BATTERY_TREND_STEP_MS;
static int16_t batTrend[BAT_TREND_N];
static uint8_t batTrendHead = 0, batTrendFill = 0;
static uint32_t batTrendNextMs = 0;
static uint32_t batRiseSeenMs = 0;   // last time the window delta cleared the charging threshold

static int batteryRead() {
  // Average a few samples: the divider sits next to the WiFi radio and the ADC is noisy.
  uint32_t sum = 0;
  for (uint8_t i = 0; i < 8; i++) sum += analogReadMilliVolts(PIN_BAT_ADC);
  return (int)(sum / 8 * BATTERY_DIVIDER);
}

static int batteryPctFromMv(int mv) {
  // Piecewise LiPo discharge curve (open-circuit voltage, rough but monotonic).
  static const struct { uint16_t mv; uint8_t pct; } curve[] = {
    {4200, 100}, {4100, 90}, {4000, 78}, {3900, 62}, {3800, 45}, {3700, 25}, {3600, 10}, {3500, 4}, {3300, 0}};
  if (mv >= curve[0].mv) return 100;
  for (size_t i = 1; i < sizeof(curve) / sizeof(curve[0]); i++) {
    if (mv >= curve[i].mv) {
      int span = curve[i - 1].mv - curve[i].mv;
      return curve[i].pct + (mv - curve[i].mv) * (curve[i - 1].pct - curve[i].pct) / span;
    }
  }
  return 0;
}

// ADC divider: filtered voltage, percentage from the curve, charging guessed from the trend.
static void batteryMeasure(uint32_t now) {
  int raw = batteryRead();
  batMv = batMv < 0 ? raw : (batMv * 3 + raw) / 4;  // light IIR on top of the 8-sample average
  batPct = batteryPctFromMv(batMv);
  // Charging guess: the pack voltage climbing >= BATTERY_CHARGE_RISE_MV over the trend window.
  // A full pack on USB sits flat at ~4.2 V, so this only catches an actual charge in progress.
  if ((int32_t)(now - batTrendNextMs) >= 0) {
    batTrendNextMs = now + BATTERY_TREND_STEP_MS;
    batTrend[batTrendHead] = (int16_t)batMv;
    batTrendHead = (batTrendHead + 1) % BAT_TREND_N;
    if (batTrendFill < BAT_TREND_N) batTrendFill++;
    if (batTrendFill == BAT_TREND_N) {
      int oldest = batTrend[batTrendHead];  // the slot about to be overwritten is the oldest
      if (batMv - oldest >= BATTERY_CHARGE_RISE_MV) batRiseSeenMs = now;
    }
  }
  // Stay "charging" for one window after the last rise, so the flag does not flicker between steps.
  batCharging = batRiseSeenMs && (uint32_t)(now - batRiseSeenMs) < BATTERY_TREND_WINDOW_MS;
  if (!batCharging) batRiseSeenMs = 0;
}
#else
static const char *chargerName(uint8_t st) {
  switch (st) {
    case XPOWERS_AXP2101_CHG_TRI_STATE: return "trickle";
    case XPOWERS_AXP2101_CHG_PRE_STATE: return "pre";
    case XPOWERS_AXP2101_CHG_CC_STATE: return "cc";
    case XPOWERS_AXP2101_CHG_CV_STATE: return "cv";
    case XPOWERS_AXP2101_CHG_DONE_STATE: return "done";
    case XPOWERS_AXP2101_CHG_STOP_STATE: return "stopped";
    default: return "idle";
  }
}

// AXP2101: voltage and percentage from its fuel gauge, charging and VBUS from its status bits.
static void batteryMeasure(uint32_t) {
  if (!pmuOk) return;
  pwr.battery = pmu.isBatteryConnect();
  pwr.batteryMv = pwr.battery ? pmu.getBattVoltage() : -1;
  pwr.batteryPct = pwr.battery ? pmu.getBatteryPercent() : -1;
  pwr.charging = pmu.isCharging();
  pwr.vbus = pmu.isVbusIn();
  pwr.vbusMv = pmu.getVbusVoltage();
  pwr.systemMv = pmu.getSystemVoltage();
  pwr.tempC = pmu.getTemperature();
  pwr.charger = chargerName(pmu.getChargerStatus());
  batMv = pwr.batteryMv;
  batPct = pwr.batteryPct;
  batCharging = pwr.charging;
}

static void pmicBegin() {
  // Same setup as Waveshare's AXP2101 example: no rail is touched, only the charger target
  // (4.2 V, the LiPo default), the IRQ mask and the measurement ADCs.
  if (!hwI2cBegin()) return;
  pmuOk = pmu.begin(Wire, PMIC_I2C_ADDR, PIN_I2C_SDA, PIN_I2C_SCL);
  if (!pmuOk) {
    Serial.println("[hw] AXP2101 not found");
    return;
  }
  pmu.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
  pmu.clearIrqStatus();
  pmu.setChargeTargetVoltage(XPOWERS_AXP2101_CHG_VOL_4V2);
  pmu.enableBattDetection();
  pmu.enableVbusVoltageMeasure();
  pmu.enableBattVoltageMeasure();
  pmu.enableSystemVoltageMeasure();
  pmu.enableTemperatureMeasure();
  static const uint8_t offSec[] = {4, 6, 8, 10};
  pwr.powerOffSec = offSec[pmu.getPowerKeyPressOffTime() & 3];
  Serial.printf("[hw] AXP2101 chip id 0x%02X, power key off after %u s\n", pmu.getChipID(), pwr.powerOffSec);
}
#endif

static void batterySample() {
  uint32_t now = millis();
  batNextSampleMs = now + 1000;
  batteryMeasure(now);
  if (batPct < 0) return;  // no pack connected
  // Low flag with hysteresis so a noisy reading around the threshold does not flap.
  if (!batLow && batPct <= BATTERY_LOW_PCT) { batLow = true; batAlarmPending = true; batAlarmMs = now; }
  else if (batLow && batPct >= BATTERY_LOW_CLEAR_PCT) batLow = false;
  else if (batLow && (uint32_t)(now - batAlarmMs) >= BATTERY_LOW_REMIND_MS) { batAlarmPending = true; batAlarmMs = now; }
}
#endif

void hwBegin() {
#if HAS_POWER_LATCH
  // Hold the battery rail on. On USB the regulator is powered anyway, so this is harmless there.
  pinMode(PIN_SYS_EN, OUTPUT);
  digitalWrite(PIN_SYS_EN, HIGH);
  pinMode(PIN_SYS_OUT, INPUT);
#endif
#if HAS_BATTERY
#if HW_PMIC
  pmicBegin();
#else
  analogReadResolution(12);
  pinMode(PIN_BAT_ADC, INPUT);
#endif
  batterySample();
#endif
  pinMode(PIN_BOOT_BTN, INPUT_PULLUP);
#if defined(HAS_KEY2) && HAS_KEY2
  pinMode(PIN_KEY2, INPUT);  // the board has its own pull resistor on this line
#endif
}

void hwLoop() {
#if HAS_BATTERY
  if ((int32_t)(millis() - batNextSampleMs) >= 0) batterySample();
#endif
}

// ---------- I2C ----------

bool hwI2cBegin() {
#ifdef PIN_I2C_SDA
  static bool started = false;
  if (!started) started = Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 400000);
  if (!started) Serial.println("[hw] I2C bus init failed");
  return started;
#else
  return false;
#endif
}

// ---------- keys ----------

static const uint32_t BTN_DEBOUNCE_MS = 30;

struct Key {
  bool down = false, longFired = false, resetFired = false;
  uint32_t downMs = 0, changeMs = 0;
  uint32_t pendingMs = 0;                  // first click waiting for a second one, 0 = none
  HwButtonEvent queued = HW_BTN_NONE;      // an event fed in by hwKeyInject()/hwButtonClick()
};
static Key keys[HW_KEY_COUNT];

bool hwKeyPresent(HwKey k) {
#if defined(HAS_KEY2) && HAS_KEY2
  return k < HW_KEY_COUNT;
#else
  return k == HW_KEY_BOOT;
#endif
}

static bool keyPressed(HwKey k) {
  if (k == HW_KEY_BOOT) return digitalRead(PIN_BOOT_BTN) == LOW;
#if defined(HAS_KEY2) && HAS_KEY2
  return digitalRead(PIN_KEY2) == KEY2_ACTIVE_LEVEL;
#else
  return false;
#endif
}

// A finished click: completes a pending double click, starts one, or comes out as it is.
static HwButtonEvent keyClick(Key &b, bool doubleClick) {
  if (b.pendingMs) { b.pendingMs = 0; return HW_BTN_DOUBLE; }
  if (doubleClick) { b.pendingMs = millis() | 1; return HW_BTN_NONE; }
  return HW_BTN_CLICK;
}

HwButtonEvent hwKeyPoll(HwKey k, bool doubleClick) {
  if (!hwKeyPresent(k)) return HW_BTN_NONE;
  Key &b = keys[k];
  if (b.queued != HW_BTN_NONE) {
    HwButtonEvent e = b.queued;
    b.queued = HW_BTN_NONE;
    return e;
  }
  uint32_t now = millis();
  if (b.pendingMs && (int32_t)(now - b.pendingMs) > (int32_t)BUTTON_DOUBLE_CLICK_MS) {
    b.pendingMs = 0;  // no second click came: it was a single one
    return HW_BTN_CLICK;
  }
  bool pressed = keyPressed(k);
  if (pressed != b.down && (int32_t)(now - b.changeMs) > (int32_t)BTN_DEBOUNCE_MS) {
    b.changeMs = now;
    b.down = pressed;
    if (pressed) {
      b.downMs = now;
      b.longFired = b.resetFired = false;
      return HW_BTN_DOWN;
    }
    return b.longFired ? HW_BTN_NONE : keyClick(b, doubleClick);
  }
  if (b.down) {
    int32_t held = (int32_t)(now - b.downMs);
    if (!b.longFired && held >= (int32_t)BUTTON_LONG_PRESS_MS) {
      b.longFired = true;
      b.pendingMs = 0;  // a hold cancels a click still waiting for its pair
      return HW_BTN_LONG;
    }
    if (!b.resetFired && held >= (int32_t)BUTTON_RESET_PRESS_MS) {
      b.resetFired = true;
      return HW_BTN_RESET;
    }
  }
  return HW_BTN_NONE;
}

void hwKeyInject(HwKey k, HwButtonEvent e) {
  if (hwKeyPresent(k)) keys[k].queued = e;
}

bool hwKeyDown(HwKey k) { return hwKeyPresent(k) && keys[k].down; }

const char *hwKeyName(HwKey k) { return k == HW_KEY_BOOT ? "boot" : k == HW_KEY_2 ? "key2" : "?"; }

const char *hwButtonEventName(HwButtonEvent e) {
  static const char *const names[] = {"none", "down", "click", "double", "long", "reset"};
  return e <= HW_BTN_RESET ? names[e] : "?";
}

HwButtonEvent hwButtonPoll(bool doubleClick) { return hwKeyPoll(HW_KEY_BOOT, doubleClick); }

void hwButtonClick(bool doubleClick) {
  HwButtonEvent e = keyClick(keys[HW_KEY_BOOT], doubleClick);
  if (e != HW_BTN_NONE) keys[HW_KEY_BOOT].queued = e;
}

bool hwButtonDown() { return keys[HW_KEY_BOOT].down; }

// ---------- battery ----------

bool hwHasBattery() { return HAS_BATTERY != 0; }

int hwBatteryMv() {
#if HAS_BATTERY
  return batMv;
#else
  return -1;
#endif
}

int hwBatteryPct() {
#if HAS_BATTERY
  return batPct;
#else
  return -1;
#endif
}

bool hwBatteryLow() {
#if HAS_BATTERY
  return batLow;
#else
  return false;
#endif
}

bool hwBatteryCharging() {
#if HAS_BATTERY
  return batCharging;
#else
  return false;
#endif
}

bool hwBatteryLowAlarmDue() {
#if HAS_BATTERY
  bool due = batAlarmPending;
  batAlarmPending = false;
  return due;
#else
  return false;
#endif
}

// ---------- PMIC ----------

bool hwPower(HwPower &out) {
#if HW_PMIC
  if (!pmuOk) return false;
  out = pwr;
  return true;
#else
  (void)out;
  return false;
#endif
}

bool hwVbusPresent() {
#if HW_PMIC
  return pmuOk && pwr.vbus;
#else
  return false;
#endif
}

// ---------- power latch ----------

bool hwPowerKeyHeld() {
#if HAS_POWER_LATCH
  return digitalRead(PIN_SYS_OUT) == LOW;  // measured: idles HIGH on the real board, so pressed is LOW
#else
  return false;
#endif
}

bool hwPowerOffDue() {
#if HAS_POWER_LATCH
  static uint32_t heldSince = 0;
  static bool fired = false;
  if (!hwPowerKeyHeld()) {
    heldSince = 0;
    fired = false;
    return false;
  }
  if (fired) return false;  // re-arms only once the key is released
  uint32_t now = millis();
  if (!heldSince) heldSince = now | 1;
  else if ((int32_t)(now - heldSince) >= (int32_t)POWER_OFF_HOLD_MS) {
    fired = true;
    return true;
  }
#endif
  return false;
}

void hwPowerOff() {
#if HAS_POWER_LATCH
  Serial.println("[hw] power off");
  Serial.flush();
  uint8_t bl = lcdBacklight();
  lcdSetBacklight(0);
  digitalWrite(PIN_SYS_EN, LOW);
  delay(500);  // still running: powered from USB
  lcdSetBacklight(bl);
#endif
}
