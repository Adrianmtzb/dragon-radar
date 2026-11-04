#pragma once
// Device settings shared by every app on this base, persisted in two Preferences namespaces:
//   "net": ssid pass wifi tz host bl rot ndim + ver (schema)     WiFi, clock, mDNS name, screen
//   "hw":  vol sfx bz bzp ledm ledp ledc ledb                    buzzer and status LED
// App settings stay in the app's own namespace. Defaults come from config.h: DEFAULT_HOSTNAME,
// DEFAULT_TZ, DEFAULT_BACKLIGHT, DEFAULT_NIGHT_DIM and BACKLIGHT_NIGHT; the rotation defaults to
// the board's LCD_DEFAULT_ROTATION.
#include "config.h"
#include <Arduino.h>

#define DEV_SETTINGS_VERSION 1

// Loads both namespaces, applies the time zone and hands the buzzer and LED settings to
// soundConfigure()/ledConfigure() (an invalid stored value falls back to the board default).
// Call early in setup(), before lcdBegin(), soundBegin() and ledBegin().
void devLoad();

// WiFi credentials (802.11 limits: SSID 1-32 bytes, password up to 64). The API never returns
// the password.
const String &devSsid();
const String &devPass();
bool devSaveWifi(const String &ssid, const String &pass);
void devForgetWifi();
// Radio on or off (default on). Only stores it: netSetWifiEnabled() switches the radio.
bool devWifiOn();
void devSetWifiOn(bool on);

// POSIX TZ string such as CST6 or CET-1CEST,M3.5.0,M10.5.0/3: 3-60 printable chars without
// spaces, starting with the zone name (letters, or <+03> style). devSetTz applies it at once.
bool devValidTz(const String &tz);
const String &devTz();
bool devSetTz(const String &tz);

// mDNS hostname without ".local": 1-24 chars of a-z, 0-9 and '-', not starting or ending with
// '-'. devSetHostname lowercases it first; it takes effect on the next boot.
bool devValidHostname(const String &host);
const String &devHostname();
bool devSetHostname(const String &host);

// Backlight setting 5-255 (clamped). The app drives the panel: devBacklightLevel() is the duty
// for now, capped at BACKLIGHT_NIGHT at night while night dimming is on.
uint8_t devBacklight();
void devSetBacklight(int level);
bool devNightDim();
void devSetNightDim(bool on);
uint8_t devBacklightLevel(bool night);

// Screen rotation 0-3; the canvas is allocated for one rotation, so a change applies on reboot.
uint8_t devRotation();
bool devSetRotation(int rotation);

// Persists the current ledConfig() and soundConfig() in "hw".
void devSaveOutputs();

// Erases the whole NVS partition (every namespace, the app's too). Reboot right after.
void devEraseAll();
