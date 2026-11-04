#include "touch.h"
#include "config.h"
#include "hw.h"

const char *touchKindName(TouchKind kind) {
  static const char *const names[] = {"none", "tap", "long", "left", "right", "up", "down"};
  return kind <= TOUCH_SWIPE_DOWN ? names[kind] : "?";
}

bool touchKindFromName(const char *name, TouchKind &out) {
  for (uint8_t k = TOUCH_TAP; k <= TOUCH_SWIPE_DOWN; k++)
    if (name && !strcmp(name, touchKindName((TouchKind)k))) {
      out = (TouchKind)k;
      return true;
    }
  return false;
}

// A simulated gesture (CLI), returned by the next touchPoll() on any board.
static TouchEvent injected;

void touchInject(TouchKind kind, int16_t x, int16_t y) {
  injected.kind = kind;
  injected.x = x;
  injected.y = y;
  injected.rawX = injected.rawY = -1;  // marks it as simulated in the logs
}

static bool takeInjected(TouchEvent &e) {
  if (injected.kind == TOUCH_NONE) return false;
  e = injected;
  injected = TouchEvent();
  return true;
}

#if HAS_TOUCH
#include <Wire.h>

static const uint16_t TAP_MAX_MS = 400, SWIPE_MIN_PX = 40, TAP_MAX_MOVE = 18, POLL_MS = 15;

static bool ok = false, isDown = false, longFired = false, armed = false;
static uint8_t rot = 0;
static uint32_t lastPollMs = 0, downMs = 0;
static int16_t downX = 0, downY = 0, lastX = 0, lastY = 0, rawDownX = 0, rawDownY = 0;  // gesture start and latest point

#if defined(TOUCH_DRIVER_CST92XX) && TOUCH_DRIVER_CST92XX
// CST9217/CST92xx through SensorLib, which handles its reset and report format. Mirrored on both
// axes and capped at the panel size, as in Waveshare's examples, it reports panel coordinates.
#include <TouchDrvCSTXXX.hpp>
static TouchDrvCST92xx cst;

static bool driverBegin() {
  cst.setPins(PIN_TP_RST, PIN_TP_INT);
  if (!cst.begin(Wire, TOUCH_I2C_ADDR, PIN_I2C_SDA, PIN_I2C_SCL)) {
    Serial.println("[touch] CST92xx not found");
    return false;
  }
  cst.setMaxCoordinates(LCD_NATIVE_W, LCD_NATIVE_H);
  cst.setMirrorXY(true, true);
  Serial.printf("[touch] %s\n", cst.getModelName());
  return true;
}

// Raw controller coordinates of the first finger; false when nothing (plausible) is touching.
static bool driverRead(int16_t &nx, int16_t &ny) {
  int16_t xs[1], ys[1];
  if (cst.getPoint(xs, ys, 1) == 0) return false;
  nx = xs[0];
  ny = ys[0];
  return nx >= 0 && ny >= 0 && nx < 0x0FFF && ny < 0x0FFF;
}
#else
static const uint8_t REG_FINGER = 0x02, REG_CHIP_ID = 0xA7, REG_DIS_AUTOSLEEP = 0xFE;

static bool writeReg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(TOUCH_I2C_ADDR);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

static bool readRegs(uint8_t reg, uint8_t *buf, uint8_t n) {
  Wire.beginTransmission(TOUCH_I2C_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)TOUCH_I2C_ADDR, (int)n) != n) return false;
  for (uint8_t i = 0; i < n; i++) buf[i] = Wire.read();
  return true;
}

static bool driverBegin() {
  pinMode(PIN_TP_INT, INPUT_PULLUP);
  pinMode(PIN_TP_RST, OUTPUT);
  digitalWrite(PIN_TP_RST, LOW);
  delay(10);
  digitalWrite(PIN_TP_RST, HIGH);
  delay(60);
  uint8_t id = 0;
  if (!readRegs(REG_CHIP_ID, &id, 1)) {
    Serial.println("[touch] CST816 not found");
    return false;
  }
  Serial.printf("[touch] CST816 chip id 0x%02X\n", id);  // B4 = S, B5 = T, B6 = D
  writeReg(REG_DIS_AUTOSLEEP, 0xFF);  // stay awake so polling keeps working
  return true;
}

static bool driverRead(int16_t &nx, int16_t &ny) {
  uint8_t b[5];
  if (!readRegs(REG_FINGER, b, sizeof(b))) return false;
  // A failed or empty I2C read comes back as 0xFF bytes: finger count 15 at (4095, 4095).
  // Those produced phantom taps on the real board, so anything implausible is dropped here.
  uint8_t fingers = b[0] & 0x0F;
  if (fingers == 0 || fingers > 2) return false;
  nx = ((b[1] & 0x0F) << 8) | b[2];
  ny = ((b[3] & 0x0F) << 8) | b[4];
  return nx < 0x0FFF && ny < 0x0FFF;
}
#endif

bool touchBegin(uint8_t rotation) {
  rot = rotation & 3;
  if (!hwI2cBegin()) return false;
  ok = driverBegin();
  return ok;
}

// Native panel coordinates (portrait, origin top-left) to the rotated canvas.
static void toCanvas(int16_t nx, int16_t ny, int16_t &x, int16_t &y) {
  switch (rot) {
    case 0: x = nx; y = ny; break;
    case 1: x = ny; y = LCD_NATIVE_W - 1 - nx; break;
    case 2: x = LCD_NATIVE_W - 1 - nx; y = LCD_NATIVE_H - 1 - ny; break;
    default: x = LCD_NATIVE_H - 1 - ny; y = nx; break;
  }
}

static bool readPoint(int16_t &x, int16_t &y, int16_t &nx, int16_t &ny) {
  if (!driverRead(nx, ny)) return false;
  // Board calibration (see boards/*.h), then clamp to the panel.
  int16_t px = constrain((int)(nx * TOUCH_X_GAIN + TOUCH_X_OFFSET), 0, LCD_NATIVE_W - 1);
  int16_t py = constrain((int)(ny * TOUCH_Y_GAIN + TOUCH_Y_OFFSET), 0, LCD_NATIVE_H - 1);
  toCanvas(px, py, x, y);
  return true;
}

TouchEvent touchPoll() {
  TouchEvent e;
  if (takeInjected(e)) return e;
  if (!ok) return e;
  uint32_t now = millis();
  if ((int32_t)(now - lastPollMs) < (int32_t)POLL_MS) return e;
  lastPollMs = now;
  int16_t x, y, nx, ny;
  bool touching = readPoint(x, y, nx, ny);
  // Debounce: a finger has to be seen on two consecutive polls before it counts as down.
  if (touching && !isDown && !armed) {
    armed = true;
    return e;
  }
  if (!touching) armed = false;
  if (touching && !isDown) {
    isDown = true;
    longFired = false;
    downMs = now;
    downX = lastX = x;
    downY = lastY = y;
    rawDownX = nx;
    rawDownY = ny;
  } else if (touching) {
    lastX = x;
    lastY = y;
    if (!longFired && (int32_t)(now - downMs) >= (int32_t)BUTTON_LONG_PRESS_MS && abs(lastX - downX) < TAP_MAX_MOVE &&
        abs(lastY - downY) < TAP_MAX_MOVE) {
      longFired = true;
      e.kind = TOUCH_LONG_PRESS;
      e.x = downX; e.y = downY; e.rawX = rawDownX; e.rawY = rawDownY;
    }
  } else if (isDown) {
    isDown = false;
    if (longFired) return e;
    int16_t dx = lastX - downX, dy = lastY - downY;
    e.x = downX; e.y = downY; e.rawX = rawDownX; e.rawY = rawDownY;
    if (abs(dx) >= SWIPE_MIN_PX && abs(dx) > abs(dy)) e.kind = dx > 0 ? TOUCH_SWIPE_RIGHT : TOUCH_SWIPE_LEFT;
    else if (abs(dy) >= SWIPE_MIN_PX) e.kind = dy > 0 ? TOUCH_SWIPE_DOWN : TOUCH_SWIPE_UP;
    else if ((int32_t)(now - downMs) <= (int32_t)TAP_MAX_MS && abs(dx) < TAP_MAX_MOVE && abs(dy) < TAP_MAX_MOVE)
      e.kind = TOUCH_TAP;
  }
  return e;
}

bool touchPresent() { return ok; }
bool touchDown() { return isDown; }
#else
bool touchBegin(uint8_t) { return false; }
TouchEvent touchPoll() {
  TouchEvent e;
  takeInjected(e);
  return e;
}
bool touchPresent() { return false; }
bool touchDown() { return false; }
#endif
