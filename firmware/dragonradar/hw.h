#pragma once
// Board plumbing shared by every app on this base: the I2C bus, the BOOT button and a second key,
// the battery gauge (ADC divider, or an AXP2101 PMIC) and the power latch. Every call is a no-op (or reports "absent") on a board without that
// part, so the sketch never needs to know which board it runs on. Timings come from config.h:
// BUTTON_DOUBLE_CLICK_MS, BUTTON_LONG_PRESS_MS, BUTTON_RESET_PRESS_MS, POWER_OFF_HOLD_MS and the
// BATTERY_* thresholds.
#include <Arduino.h>

void hwBegin();  // call first in setup(): latches the battery rail on boards that have one
void hwLoop();   // battery monitor; call every loop

// Shared I2C bus (touch, IMU, RTC). Idempotent: every driver calls it, the first one opens the
// bus at 400 kHz. False on boards without an I2C bus.
bool hwI2cBegin();

// BOOT button. Events come out of hwButtonPoll(), one per call:
// - HW_BTN_DOWN as soon as the key goes down (for instant feedback),
// - HW_BTN_CLICK on release. With `doubleClick` true the click waits BUTTON_DOUBLE_CLICK_MS for a
//   second one, which turns both into a single HW_BTN_DOUBLE; with false it comes out at once,
// - HW_BTN_LONG once the key has been held BUTTON_LONG_PRESS_MS (no click follows),
// - HW_BTN_RESET once it has been held BUTTON_RESET_PRESS_MS (after HW_BTN_LONG).
enum HwButtonEvent : uint8_t { HW_BTN_NONE, HW_BTN_DOWN, HW_BTN_CLICK, HW_BTN_DOUBLE, HW_BTN_LONG, HW_BTN_RESET };
HwButtonEvent hwButtonPoll(bool doubleClick = false);
// A click from another input (a tap, the CLI) fed through the same double-click logic.
void hwButtonClick(bool doubleClick = false);
bool hwButtonDown();

// Every key by index: BOOT on all boards, and KEY2 on boards with HAS_KEY2 (PIN_KEY2, pressed at
// KEY2_ACTIVE_LEVEL). Same events and timings as the BOOT button above, which is HW_KEY_BOOT.
enum HwKey : uint8_t { HW_KEY_BOOT, HW_KEY_2, HW_KEY_COUNT };
bool hwKeyPresent(HwKey key);
HwButtonEvent hwKeyPoll(HwKey key, bool doubleClick = false);
// A simulated event (CLI press/hold): CLICK, DOUBLE, LONG or RESET comes out of the next poll.
void hwKeyInject(HwKey key, HwButtonEvent event);
bool hwKeyDown(HwKey key);
const char *hwKeyName(HwKey key);            // "boot", "key2"
const char *hwButtonEventName(HwButtonEvent e);  // "down", "click", "double", "long", "reset"

// Battery gauge, refreshed once a second from hwLoop() on boards with one (cached: reading it
// never touches the ADC or the bus). `low` latches at <= BATTERY_LOW_PCT and clears at
// >= BATTERY_LOW_CLEAR_PCT; `charging` comes from the PMIC where there is one, otherwise it is a
// guess from the voltage trend (those boards do not route VBUS).
bool hwHasBattery();
int hwBatteryMv();   // filtered millivolts, -1 without a gauge
int hwBatteryPct();  // 0-100, -1 without a gauge
bool hwBatteryLow();
bool hwBatteryCharging();
// True once when the low-battery alarm is due: the moment the flag turns on, then every
// BATTERY_LOW_REMIND_MS while it stays on. The sketch turns it into a sound.
bool hwBatteryLowAlarmDue();

// Power management IC (boards with PMIC_AXP2101), sampled with the battery. hwPower() is false
// without one; then only the battery calls above apply.
struct HwPower {
  bool battery;         // a pack is connected
  int batteryMv;        // -1 without a pack
  int batteryPct;       // fuel gauge 0-100, -1 without a pack
  bool charging;
  bool vbus;            // USB power present
  int vbusMv;
  int systemMv;
  float tempC;          // PMIC die temperature
  const char *charger;  // "idle", "trickle", "pre", "cc", "cv", "done" or "stopped"
  uint8_t powerOffSec;  // how long the power key must be held to switch off
};
bool hwPower(HwPower &out);
bool hwVbusPresent();  // false on boards that cannot tell

// Power latch (boards with HAS_POWER_LATCH). The PWR key idles HIGH and reads LOW while pressed.
bool hwPowerKeyHeld();
// True once per press when PWR has been held POWER_OFF_HOLD_MS: the app saves its state, plays
// its sound and calls hwPowerOff().
bool hwPowerOffDue();
// Turns the backlight off and cuts the battery rail. On USB the board stays powered: then it
// returns after a moment with the backlight restored.
void hwPowerOff();
