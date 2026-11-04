#include "devsettings.h"
#include <Preferences.h>
#include <nvs_flash.h>
#include <time.h>
#include "led.h"
#include "sound.h"

#ifndef DEFAULT_BACKLIGHT
#define DEFAULT_BACKLIGHT 200
#endif
#ifndef DEFAULT_NIGHT_DIM
#define DEFAULT_NIGHT_DIM 0
#endif
#ifndef BACKLIGHT_NIGHT
#define BACKLIGHT_NIGHT 60
#endif

static String ssid, pass, tz = DEFAULT_TZ, host = DEFAULT_HOSTNAME;
static uint8_t backlight = DEFAULT_BACKLIGHT;
static uint8_t rotation = LCD_DEFAULT_ROTATION;
static bool nightDim = DEFAULT_NIGHT_DIM;
static bool wifiOn = true;

static void applyTz() {
  setenv("TZ", tz.c_str(), 1);
  tzset();
}

void devLoad() {
  Preferences p;
  p.begin("net", false);  // read-write: creates the namespace on the first boot
  if (p.getUChar("ver", 0) != DEV_SETTINGS_VERSION) p.putUChar("ver", DEV_SETTINGS_VERSION);
  ssid = p.getString("ssid", "");
  pass = p.getString("pass", "");
  tz = p.getString("tz", DEFAULT_TZ);
  host = p.getString("host", DEFAULT_HOSTNAME);
  backlight = constrain(p.getUChar("bl", DEFAULT_BACKLIGHT), 5, 255);
  rotation = p.getUChar("rot", LCD_DEFAULT_ROTATION) & 3;
  nightDim = p.getBool("ndim", DEFAULT_NIGHT_DIM);
  wifiOn = p.getBool("wifi", true);
  p.end();
  if (!devValidTz(tz)) tz = DEFAULT_TZ;
  if (!devValidHostname(host)) host = DEFAULT_HOSTNAME;
  applyTz();

  // Buzzer first, then the LED: each one refuses a pin the other holds.
  p.begin("hw", false);
  SoundConfig sc = soundDefaults();
  sc.volume = min<uint8_t>(100, p.getUChar("vol", sc.volume));
  sc.effects = p.getBool("sfx", sc.effects);
  sc.enabled = p.getBool("bz", sc.enabled);
  sc.pin = (int8_t)p.getUChar("bzp", (uint8_t)sc.pin);
  LedConfig lc = ledDefaults();
  lc.mode = (LedMode)p.getUChar("ledm", lc.mode);
  lc.pin = (int8_t)p.getUChar("ledp", (uint8_t)lc.pin);
  lc.color = p.getUInt("ledc", lc.color);
  lc.brightness = p.getUChar("ledb", lc.brightness);
  p.end();
  String err;
  if (!soundConfigure(sc, err)) soundConfigure(soundDefaults(), err);
  if (!ledConfigure(lc, err)) ledConfigure(ledDefaults(), err);
}

const String &devSsid() { return ssid; }
const String &devPass() { return pass; }

bool devSaveWifi(const String &s, const String &pw) {
  if (s.length() < 1 || s.length() > 32 || pw.length() > 64) return false;
  Preferences p;
  p.begin("net", false);
  p.putString("ssid", s);
  p.putString("pass", pw);
  p.end();
  ssid = s;
  pass = pw;
  return true;
}

void devForgetWifi() {
  Preferences p;
  p.begin("net", false);
  p.remove("ssid");
  p.remove("pass");
  p.end();
  ssid = "";
  pass = "";
}

bool devValidTz(const String &s) {
  if (s.length() < 3 || s.length() > 60) return false;
  char c0 = s[0];
  if (!((c0 >= 'A' && c0 <= 'Z') || (c0 >= 'a' && c0 <= 'z') || c0 == '<')) return false;
  for (size_t i = 0; i < s.length(); i++)
    if (s[i] <= ' ' || s[i] > '~') return false;
  return true;
}

const String &devTz() { return tz; }

bool devSetTz(const String &s) {
  if (!devValidTz(s)) return false;
  tz = s;
  Preferences p;
  p.begin("net", false);
  p.putString("tz", tz);
  p.end();
  applyTz();
  return true;
}

bool devValidHostname(const String &h) {
  if (h.length() < 1 || h.length() > 24) return false;
  for (size_t i = 0; i < h.length(); i++) {
    char c = h[i];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
  }
  return h[0] != '-' && h[h.length() - 1] != '-';
}

const String &devHostname() { return host; }

bool devSetHostname(const String &h) {
  String clean = h;
  clean.trim();
  clean.toLowerCase();
  if (!devValidHostname(clean)) return false;
  host = clean;
  Preferences p;
  p.begin("net", false);
  p.putString("host", host);
  p.end();
  return true;
}

uint8_t devBacklight() { return backlight; }

void devSetBacklight(int level) {
  backlight = constrain(level, 5, 255);
  Preferences p;
  p.begin("net", false);
  p.putUChar("bl", backlight);
  p.end();
}

bool devWifiOn() { return wifiOn; }

void devSetWifiOn(bool on) {
  wifiOn = on;
  Preferences p;
  p.begin("net", false);
  p.putBool("wifi", wifiOn);
  p.end();
}

bool devNightDim() { return nightDim; }

void devSetNightDim(bool on) {
  nightDim = on;
  Preferences p;
  p.begin("net", false);
  p.putBool("ndim", nightDim);
  p.end();
}

uint8_t devBacklightLevel(bool night) { return night && nightDim ? min<uint8_t>(backlight, BACKLIGHT_NIGHT) : backlight; }

uint8_t devRotation() { return rotation; }

bool devSetRotation(int r) {
  if (r < 0 || r > 3) return false;
  rotation = r;
  Preferences p;
  p.begin("net", false);
  p.putUChar("rot", rotation);
  p.end();
  return true;
}

void devSaveOutputs() {
  Preferences p;
  p.begin("hw", false);
  const SoundConfig &sc = soundConfig();
  p.putUChar("vol", sc.volume);
  p.putBool("sfx", sc.effects);
  p.putBool("bz", sc.enabled);
  p.putUChar("bzp", (uint8_t)sc.pin);
  const LedConfig &lc = ledConfig();
  p.putUChar("ledm", lc.mode);
  p.putUChar("ledp", (uint8_t)lc.pin);
  p.putUInt("ledc", lc.color);
  p.putUChar("ledb", lc.brightness);
  p.end();
}

void devEraseAll() {
  nvs_flash_erase();
  nvs_flash_init();
}
