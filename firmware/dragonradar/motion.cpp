#include "motion.h"
#include <Wire.h>
#include "hw.h"
#include <math.h>

// QMI8658 registers
#define QMI_WHO_AM_I 0x00
#define QMI_CTRL1 0x02
#define QMI_CTRL2 0x03
#define QMI_CTRL3 0x04
#define QMI_CTRL7 0x08
#define QMI_AX_L 0x35
#define QMI_GX_L 0x3B
#define QMI_RESET 0x60

static uint8_t rot = 0;
static bool imuOk = false;
static float rawX = 0, rawY = 0, rawZ = 0;  // last IMU sample (g), sensor axes
static float gyrX = 0, gyrY = 0, gyrZ = 0;  // last gyro sample (deg/s), sensor axes, bias removed
static float biasX = 0, biasY = 0, biasZ = 0;
static bool biasValid = false;
// Gyro bias learning: a window of STILL_N samples whose raw rates stay within STILL_SPREAD_DPS on
// every axis (and the accelerometer within STILL_ACC_G) means the board is resting; its mean
// rate becomes the new bias. Works whenever the board rests, not only at boot.
static const int STILL_N = 100;  // 1 s at the 10 ms sample period
static const float STILL_SPREAD_DPS = 3.0f, STILL_ACC_G = 0.03f;
static float winSum[3], winMin[3], winMax[3], winAcc0[3];
static int winN = 0;

static void learnBias(const float r[3], const float acc[3]) {
  if (winN == 0)
    for (int i = 0; i < 3; i++) { winSum[i] = 0; winMin[i] = winMax[i] = r[i]; winAcc0[i] = acc[i]; }
  bool still = true;
  for (int i = 0; i < 3; i++) {
    winSum[i] += r[i];
    winMin[i] = min(winMin[i], r[i]);
    winMax[i] = max(winMax[i], r[i]);
    if (winMax[i] - winMin[i] > STILL_SPREAD_DPS || fabsf(acc[i] - winAcc0[i]) > STILL_ACC_G) still = false;
  }
  if (!still) {
    winN = 0;
    return;
  }
  if (++winN < STILL_N) return;
  biasX = winSum[0] / winN;
  biasY = winSum[1] / winN;
  biasZ = winSum[2] / winN;
  biasValid = true;
  winN = 0;
}
static float yaw = 0;                       // integrated rotation around the sensor Z axis (deg)
static float gravX = 0, gravY = 1;          // low-passed gravity, screen axes
static float lastAx = 0, lastAy = 0, lastAz = 1;
static float shake = 0;
static float injX = 0, injY = 1;
static uint32_t injUntil = 0;
static uint32_t lastSample = 0;
static uint32_t lastDecay = 0;

#if HAS_IMU
static bool qmiWrite(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(IMU_I2C_ADDR);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

static bool qmiRead(uint8_t reg, uint8_t *buf, uint8_t len) {
  Wire.beginTransmission(IMU_I2C_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((uint8_t)IMU_I2C_ADDR, len) != len) return false;
  for (uint8_t i = 0; i < len; i++) buf[i] = Wire.read();
  return true;
}
#endif

void motionBegin(uint8_t rotation) {
  rot = rotation & 3;
#if HAS_IMU
  uint8_t id = 0;
  if (hwI2cBegin() && qmiRead(QMI_WHO_AM_I, &id, 1) && id == 0x05) {
    qmiWrite(QMI_RESET, 0xB0);
    delay(20);
    qmiWrite(QMI_CTRL1, 0x40);  // register auto-increment
    qmiWrite(QMI_CTRL2, 0x15);  // accel +-4g, ~250 Hz
    qmiWrite(QMI_CTRL3, 0x55);  // gyro +-512 deg/s, ~224 Hz
    qmiWrite(QMI_CTRL7, 0x03);  // accel and gyro enable
    delay(60);                  // the gyro needs a moment before its first valid sample
    imuOk = true;
  }
  Serial.printf("[imu] %s (id=0x%02X)\n", imuOk ? "QMI8658 ok" : "not found", id);
#endif
}

// Sensor axes -> native portrait axes -> rotated screen axes. Measured on the S3 1.69 board: landscape
// (rotation 1) with the buttons on top reads +1 g on sensor Y, which must map to "down" (+y). Boards
// with the sensor mounted differently set IMU_NATIVE_AXES (not measured yet on the AMOLED 1.75C).
static void toScreen(float ax, float ay, float &mx, float &my) {
#ifdef IMU_NATIVE_AXES
  IMU_NATIVE_AXES;
#else
  float nx = -ay, ny = ax;  // gravity in native portrait coordinates
#endif
  switch (rot) {
    case 0: mx = nx; my = ny; break;
    case 1: mx = ny; my = -nx; break;
    case 2: mx = -nx; my = -ny; break;
    default: mx = -ny; my = nx; break;
  }
}

void motionLoop() {
  uint32_t now = millis();
  if ((int32_t)(now - lastSample) < 10) return;
  float dt = (now - lastSample) / 1000.0f;
  lastSample = now;
  if (dt > 0.1f) dt = 0.1f;

#if HAS_IMU
  if (imuOk) {
    uint8_t b[12];  // accel X/Y/Z then gyro X/Y/Z, little-endian
    if (qmiRead(QMI_AX_L, b, 12)) {
      const float scale = 1.0f / 8192.0f;  // +-4g
      rawX = (int16_t)(b[0] | (b[1] << 8)) * scale;
      rawY = (int16_t)(b[2] | (b[3] << 8)) * scale;
      rawZ = (int16_t)(b[4] | (b[5] << 8)) * scale;
      const float dps = 1.0f / 64.0f;  // +-512 deg/s
      const float r[3] = {(int16_t)(b[6] | (b[7] << 8)) * dps, (int16_t)(b[8] | (b[9] << 8)) * dps,
                          (int16_t)(b[10] | (b[11] << 8)) * dps};
      const float acc[3] = {rawX, rawY, rawZ};
      learnBias(r, acc);
      gyrX = r[0] - biasX;
      gyrY = r[1] - biasY;
      gyrZ = r[2] - biasZ;
      yaw += gyrZ * dt;
      if (yaw >= 360) yaw -= 360;
      else if (yaw < 0) yaw += 360;

      float mx, my;
      toScreen(rawX, rawY, mx, my);
      gravX += (mx - gravX) * 0.25f;
      gravY += (my - gravY) * 0.25f;

      // Shake = jerk between samples; ignore small hand tremor.
      float jx = rawX - lastAx, jy = rawY - lastAy, jz = rawZ - lastAz;
      float jerk = sqrtf(jx * jx + jy * jy + jz * jz);
      lastAx = rawX; lastAy = rawY; lastAz = rawZ;
      if (jerk > 0.25f) shake += (jerk - 0.25f) * 0.12f;
    }
  }
#endif

  // Decay of shake energy (about 1.5 s to fade from full).
  if (lastDecay == 0) lastDecay = now;
  float decay = (now - lastDecay) / 1000.0f;
  lastDecay = now;
  shake -= decay * 0.65f;
  if (shake < 0) shake = 0;
  if (shake > 1) shake = 1;
}

void motionGravity(float &gx, float &gy) {
  uint32_t now = millis();
  if ((int32_t)(injUntil - now) > 0) {
    gx = injX;
    gy = injY;
    return;
  }
  if (imuOk) {
    gx = gravX;
    gy = gravY;
    return;
  }
  gx = 0;  // no IMU: straight down
  gy = 1.0f;
}

float motionShake() { return shake; }
bool motionHasImu() { return imuOk; }

bool motionRaw(float &ax, float &ay, float &az) {
  ax = rawX; ay = rawY; az = rawZ;
  return imuOk;
}

bool motionGyro(float &gx, float &gy, float &gz) {
  gx = gyrX; gy = gyrY; gz = gyrZ;
  return imuOk;
}

bool motionGyroBias(float &bx, float &by, float &bz) {
  bx = biasX; by = biasY; bz = biasZ;
  return biasValid;
}

float motionYaw() { return yaw; }
void motionResetYaw() { yaw = 0; }

void motionInjectTilt(float gx, float gy, uint32_t holdMs) {
  injX = constrain(gx, -2.0f, 2.0f);
  injY = constrain(gy, -2.0f, 2.0f);
  injUntil = millis() + holdMs;
}

void motionInjectShake(float amount) {
  shake += constrain(amount, 0.0f, 1.0f);
  if (shake > 1) shake = 1;
}
