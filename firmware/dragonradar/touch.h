#pragma once
// Capacitive touch (I2C, TOUCH_I2C_ADDR) reduced to gestures: tap, long press and the four swipes.
// CST816 by default, CST92xx (SensorLib) with TOUCH_DRIVER_CST92XX in the board header. Gestures are derived from polled positions rather than the chip's gesture register,
// which only reports reliably in interrupt mode. Coordinates come out calibrated with the board's
// TOUCH_X/Y_GAIN/OFFSET and rotated to the canvas. On boards with HAS_TOUCH 0 every call is a stub.
#include <Arduino.h>

enum TouchKind : uint8_t { TOUCH_NONE, TOUCH_TAP, TOUCH_LONG_PRESS, TOUCH_SWIPE_LEFT, TOUCH_SWIPE_RIGHT, TOUCH_SWIPE_UP, TOUCH_SWIPE_DOWN };

struct TouchEvent {
  TouchKind kind = TOUCH_NONE;
  int16_t x = 0, y = 0;        // where the finger went down, canvas coordinates
  int16_t rawX = 0, rawY = 0;  // controller coordinates of the same point, for bring-up logs
};

// Resets the controller and checks it answers; `rotation` (0-3) is the canvas rotation.
bool touchBegin(uint8_t rotation);
// Polls the controller (at most every 15 ms) and returns at most one gesture. A tap or a swipe
// comes out when the finger lifts; a long press fires once while it is still down, after
// BUTTON_LONG_PRESS_MS without moving, and nothing follows it on release.
TouchEvent touchPoll();
bool touchPresent();  // a controller answered in touchBegin()
bool touchDown();     // a finger is on the glass right now
const char *touchKindName(TouchKind kind);  // "tap", "long", "left"... for logs
bool touchKindFromName(const char *name, TouchKind &out);  // the reverse, TOUCH_NONE excluded
// Simulated gesture at canvas (x, y), e.g. from the CLI: the next touchPoll() returns it (rawX/Y
// are -1). Works on boards without touch too.
void touchInject(TouchKind kind, int16_t x, int16_t y);
