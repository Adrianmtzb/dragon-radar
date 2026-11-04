#include "led.h"
#include "sound.h"

#ifndef LED_APP_MODE_NAME
#define LED_APP_MODE_NAME "app"
#endif
#ifndef LED_DEFAULT_BRIGHTNESS
#define LED_DEFAULT_BRIGHTNESS 40
#endif
#ifndef LED_DEFAULT_COLOR
#define LED_DEFAULT_COLOR 0x0078FF
#endif
#ifndef LED_REFRESH_MS
#define LED_REFRESH_MS 100
#endif

// Pins from the board header. A board with no LED at all may leave RGB_LED_PINS undefined.
#ifdef RGB_LED_PINS
static constexpr uint8_t PINS[] = RGB_LED_PINS;
static constexpr int PIN_COUNT = sizeof(PINS);
static constexpr bool listHas(const uint8_t *p, int n, int pin) { return n > 0 && (p[0] == pin || listHas(p + 1, n - 1, pin)); }
#if HAS_RGB_LED
static_assert(listHas(PINS, PIN_COUNT, PIN_RGB_LED), "RGB_LED_PINS must include PIN_RGB_LED");
#endif
#ifdef RGB_LED_DEFAULT_PIN
static_assert(listHas(PINS, PIN_COUNT, RGB_LED_DEFAULT_PIN), "RGB_LED_PINS must include RGB_LED_DEFAULT_PIN");
#endif
#else
#if HAS_RGB_LED
#error "HAS_RGB_LED needs RGB_LED_PINS in the board header"
#endif
static const uint8_t *const PINS = nullptr;
static constexpr int PIN_COUNT = 0;
#endif

#if defined(RGB_LED_DEFAULT_PIN)
#define LED_PIN_DEFAULT RGB_LED_DEFAULT_PIN
#elif HAS_RGB_LED
#define LED_PIN_DEFAULT PIN_RGB_LED
#else
#define LED_PIN_DEFAULT (PIN_COUNT ? PINS[0] : -1)
#endif

static LedHooks hooks = {nullptr, nullptr};
static LedConfig cfg = ledDefaults();
static bool begun = false;
static int activePin = -1;
static uint32_t current = 0;
static bool dirty = true;
static bool driven = false;  // the RMT channel is only claimed once the LED first lights up

LedConfig ledDefaults() {
  // The on-board LED starts in the app mode; an external one stays off until someone enables it.
  return {HAS_RGB_LED ? LED_APP : LED_OFF, (int8_t)LED_PIN_DEFAULT, LED_DEFAULT_COLOR, LED_DEFAULT_BRIGHTNESS};
}

const LedConfig &ledConfig() { return cfg; }
bool ledAvailable() { return PIN_COUNT > 0; }
bool ledBuiltin() { return HAS_RGB_LED; }
int ledPinCount() { return PIN_COUNT; }
int ledPinAt(int i) { return i >= 0 && i < PIN_COUNT ? PINS[i] : -1; }

bool ledPinAllowed(int pin) {
  for (int i = 0; i < PIN_COUNT; i++)
    if (PINS[i] == pin) return true;
  return false;
}

bool ledUsesPin(int pin) { return pin >= 0 && cfg.mode != LED_OFF && cfg.pin == pin; }

static void write(uint32_t c) {
  if (!dirty && c == current) return;
  dirty = false;
  current = c;
  if (activePin < 0) return;
  if (!c && !driven) return;  // never touched: leave the pin alone (nothing may be wired)
  driven = true;
  rgbLedWrite(activePin, c >> 16, (c >> 8) & 0xFF, c & 0xFF);
}

static uint32_t scaled(uint32_t c) {
  uint16_t k = cfg.brightness;
  uint32_t r = ((c >> 16) & 0xFF) * k / 255, g = ((c >> 8) & 0xFF) * k / 255, b = (c & 0xFF) * k / 255;
  return (r << 16) | (g << 8) | b;
}

static uint32_t statusColor() {
  switch (hooks.status ? hooks.status() : LED_NET_CONNECTING) {
    case LED_NET_CONNECTED: return 0x00FF3C;
    case LED_NET_AP: return 0xFF9600;
    case LED_NET_OFF: return 0;  // radio off on purpose: dark, not a blink that says "trying"
    default: return (millis() / 400) & 1 ? 0x0050FF : 0;  // connecting: blink blue
  }
}

static void refresh() {
  uint32_t c = 0;
  switch (cfg.mode) {
    case LED_APP: c = hooks.app ? hooks.app() : 0; break;
    case LED_COLOR: c = cfg.color; break;
    case LED_STATUS: c = statusColor(); break;
    default: break;
  }
  write(scaled(c & 0xFFFFFF));
}

static void apply() {
  if (activePin != cfg.pin) {
    if (activePin >= 0 && driven) {
      rgbLedWrite(activePin, 0, 0, 0);  // leave the old LED dark, then free its RMT channel
      rmtDeinit(activePin);
    }
    driven = false;
    activePin = cfg.pin;
    if (activePin >= 0) Serial.printf("[led] GPIO %d, mode %s\n", activePin, ledModeName(cfg.mode));
  }
  dirty = true;
  refresh();
}

bool ledConfigure(const LedConfig &c, String &err) {
  if (c.mode > LED_STATUS) {
    err = "unknown LED mode";
    return false;
  }
  if (!ledPinAllowed(c.pin)) {
    err = PIN_COUNT ? "pin not allowed for the LED on this board" : "this board has no LED pins";
    return false;
  }
  if (c.mode != LED_OFF && soundUsesPin(c.pin)) {
    err = "that pin is used by the buzzer, move or turn off the buzzer first";
    return false;
  }
  cfg = c;
  if (begun) apply();
  return true;
}

void ledBegin(const LedHooks &h) {
  hooks = h;
  begun = true;
  if (!ledPinAllowed(cfg.pin)) cfg = ledDefaults();
  if (!ledAvailable()) Serial.println("[led] no LED on this board");
  apply();
}

void ledLoop() {
  if (!begun) return;
  static uint32_t last = 0;
  if (millis() - last < LED_REFRESH_MS) return;
  last = millis();
  refresh();
}

uint32_t ledCurrent() { return current; }

static const char *const MODE_NAMES[] = {"off", LED_APP_MODE_NAME, "color", "status"};
const char *ledModeName(LedMode m) { return m <= LED_STATUS ? MODE_NAMES[m] : "off"; }
bool ledModeFromName(const char *s, LedMode &out) {
  for (int i = 0; i <= LED_STATUS; i++)
    if (s && !strcmp(s, MODE_NAMES[i])) {
      out = (LedMode)i;
      return true;
    }
  return false;
}
