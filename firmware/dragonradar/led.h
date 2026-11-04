// Status LED (WS2812): the board's own LED (HAS_RGB_LED) or an external one wired to one of the
// GPIOs the board header allows (RGB_LED_PINS). Nothing here knows which board it runs on: pins,
// presence and the default come from board.h, and an LED that is not on the board starts off.
//
// Modes: off, app (a color the sketch computes, see LedHooks), a fixed color, or WiFi status
// (green connected, amber setup AP, blinking blue connecting). Brightness 0-255 scales all of them.
// Optional config.h macros: LED_APP_MODE_NAME (API name of the app mode, default "app"),
// LED_DEFAULT_BRIGHTNESS (default 40), LED_DEFAULT_COLOR (0xRRGGBB, default 0x0078FF).
#pragma once
#include "config.h"
#include <Arduino.h>

enum LedMode : uint8_t { LED_OFF = 0, LED_APP = 1, LED_COLOR = 2, LED_STATUS = 3 };
enum LedNetState : uint8_t { LED_NET_CONNECTING = 0, LED_NET_CONNECTED = 1, LED_NET_AP = 2, LED_NET_OFF = 3 };

struct LedConfig {
  LedMode mode;
  int8_t pin;          // -1 when the board allows no LED pin
  uint32_t color;      // 0xRRGGBB for LED_COLOR
  uint8_t brightness;  // 0-255
};

// What the sketch provides; either pointer may be null (that mode then stays dark).
struct LedHooks {
  uint32_t (*app)();         // LED_APP color, 0xRRGGBB before brightness
  LedNetState (*status)();   // LED_STATUS
};

void ledBegin(const LedHooks &hooks);
void ledLoop();

LedConfig ledDefaults();
const LedConfig &ledConfig();
// Validates (mode, allowed pin, no clash with an enabled buzzer) and applies. On failure nothing
// changes and `err` says why. Callable before ledBegin(), e.g. when loading saved settings.
bool ledConfigure(const LedConfig &cfg, String &err);

bool ledAvailable();          // the board has an LED or at least one allowed LED pin
bool ledBuiltin();            // HAS_RGB_LED
bool ledPinAllowed(int pin);
int ledPinCount();
int ledPinAt(int i);
bool ledUsesPin(int pin);     // the LED is on (mode != off) and drives this pin

const char *ledModeName(LedMode m);
bool ledModeFromName(const char *s, LedMode &out);
uint32_t ledCurrent();  // color sent to the LED right now (after brightness), 0xRRGGBB
