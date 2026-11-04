// IMU input: gravity, shake and rotation. Uses the QMI8658 when the board has one; otherwise (or
// when a client injects values over the API) a virtual accelerometer. Sampled every 10 ms.
#pragma once
#include "config.h"

void motionBegin(uint8_t rotation);
void motionLoop();

// Gravity in screen coordinates (+x right, +y down), in g. Board upright -> roughly (0, 1).
void motionGravity(float &gx, float &gy);
// Shake energy 0..1, decays over time.
float motionShake();
bool motionHasImu();
bool motionRaw(float &ax, float &ay, float &az);  // last raw IMU sample, false without IMU
// Angular rate in deg/s on the sensor axes minus the gyro bias; false without IMU. The bias is
// learned whenever the board rests still for a second (motionGyroBias() is false until then).
bool motionGyro(float &gx, float &gy, float &gz);
bool motionGyroBias(float &bx, float &by, float &bz);
// Rotation around the sensor Z axis (the axis through the screen), integrated from the gyro,
// 0-360 degrees. It drifts slowly: reset it when the app needs a fresh reference.
float motionYaw();
void motionResetYaw();

// Virtual input from the API/CLI/button. Tilt overrides the IMU for `holdMs`.
void motionInjectTilt(float gx, float gy, uint32_t holdMs);
void motionInjectShake(float amount);
