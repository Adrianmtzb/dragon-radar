// Dragon Radar: a Dragon Ball style radar on the Waveshare ESP32-S3-Touch-AMOLED-1.75C.
// Bring-up skeleton: splash, keys, touch, IMU, PMIC, speaker, WiFi with the setup portal, the
// common HTTP API plus /api/state, and the serial console. The radar itself comes later.
#include <ArduinoJson.h>
#include <Preferences.h>
#include <Wire.h>
#include "cli.h"
#include "config.h"
#include "devsettings.h"
#include "hw.h"
#include "lcd.h"
#include "led.h"
#include "motion.h"
#include "net.h"
#include "sound.h"
#include "touch.h"

static Arduino_Canvas *gfx = nullptr;
static uint32_t bootCount = 0;

// Last input seen, for /api/state and the logs.
static struct {
  const char *source = "";  // "boot", "key2" or "touch"
  const char *event = "";   // "click", "long", "tap", "left"...
  int16_t x = -1, y = -1;   // touch only
  uint32_t ms = 0;
} lastInput;

static void noteInput(const char *source, const char *event, int16_t x = -1, int16_t y = -1) {
  lastInput.source = source;
  lastInput.event = event;
  lastInput.x = x;
  lastInput.y = y;
  lastInput.ms = millis();
}

// ---------- screen ----------

static uint16_t rgb565(uint32_t rgb) { return ((rgb >> 8) & 0xF800) | ((rgb >> 5) & 0x07E0) | ((rgb >> 3) & 0x001F); }

static void drawCentered(const char *text, int16_t cy, uint8_t size, uint16_t color) {
  int16_t x1, y1;
  uint16_t w, h;
  gfx->setTextSize(size);
  gfx->getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
  gfx->setTextColor(color);
  gfx->setCursor((lcdWidth() - (int16_t)w) / 2 - x1, cy - (int16_t)h / 2 - y1);
  gfx->print(text);
}

// Boot screen: black, a placeholder green disc for the radar, name, version and the author's site.
// Every position derives from the canvas size.
static void drawSplash() {
  if (!gfx) return;
  const int16_t W = lcdWidth(), H = lcdHeight(), cx = W / 2, cy = H / 2;
  const int16_t r = min(W, H) * 39 / 100;
  gfx->fillScreen(0x0000);
  gfx->fillCircle(cx, cy, r, rgb565(0x2BD66B));
  gfx->drawCircle(cx, cy, r * 2 / 3, rgb565(0x1E9E4E));
  gfx->drawCircle(cx, cy, r / 3, rgb565(0x1E9E4E));
  drawCentered("Dragon Radar", cy - H / 30, 4, 0x0000);
  drawCentered("v" FW_VERSION, cy + H / 12, 2, 0x0000);
  drawCentered(AUTHOR_SHORT, cy + r + (H / 2 - r) / 2, 2, rgb565(0x9AA59D));
  gfx->flush();
}

// Full-screen flush rate: `frames` frames of a changing fill, flush timed on its own and with the
// fill. Redraws the splash afterwards.
static void measureFps(int frames) {
  if (!gfx) {
    Serial.println("error: no display");
    return;
  }
  frames = constrain(frames, 5, 600);
  uint32_t flushUs = 0, totalUs = 0;
  for (int i = 0; i < frames; i++) {
    uint32_t t0 = micros();
    gfx->fillScreen(i & 1 ? rgb565(0x103018) : rgb565(0x08200F));
    gfx->fillCircle(lcdWidth() / 2, lcdHeight() / 2, 20 + (i * 7) % 180, rgb565(0x2BD66B));
    uint32_t t1 = micros();
    gfx->flush();
    uint32_t t2 = micros();
    flushUs += t2 - t1;
    totalUs += t2 - t0;
  }
  Serial.printf("fps: %d frames %dx%d, flush %.1f ms (%.1f fps), draw+flush %.1f ms (%.1f fps)\n", frames, lcdWidth(),
                lcdHeight(), flushUs / 1000.0f / frames, frames * 1e6f / flushUs, totalUs / 1000.0f / frames,
                frames * 1e6f / totalUs);
  drawSplash();
}

// Night dimming (devsettings.h): the brightness follows the clock, checked once a second.
static void applyBacklight() {
  static uint32_t last = 0;
  if (millis() - last < 1000) return;
  last = millis();
  uint8_t bl = devBacklightLevel(netIsNight());
  if (lcdBacklight() != bl) lcdSetBacklight(bl);
}

// ---------- input ----------

static void pollKeys() {
  for (uint8_t k = 0; k < HW_KEY_COUNT; k++) {
    if (!hwKeyPresent((HwKey)k)) continue;
    HwButtonEvent e = hwKeyPoll((HwKey)k);
    if (e == HW_BTN_NONE || e == HW_BTN_DOWN) continue;
    noteInput(hwKeyName((HwKey)k), hwButtonEventName(e));
    Serial.printf("[key] %s %s\n", hwKeyName((HwKey)k), hwButtonEventName(e));
  }
}

static void pollTouch() {
  TouchEvent e = touchPoll();
  if (e.kind == TOUCH_NONE) return;
  noteInput("touch", touchKindName(e.kind), e.x, e.y);
  if (e.rawX < 0) Serial.printf("[touch] %s x=%d y=%d (simulated)\n", touchKindName(e.kind), e.x, e.y);
  else Serial.printf("[touch] %s x=%d y=%d raw=(%d, %d)\n", touchKindName(e.kind), e.x, e.y, e.rawX, e.rawY);
}

// ---------- status (CLI and API) ----------

static void powerJson(JsonObject o) {
  HwPower p;
  o["pmic"] = hwPower(p);
  o["batteryMv"] = hwBatteryMv();
  o["batteryPct"] = hwBatteryPct();
  o["charging"] = hwBatteryCharging();
  o["lowBattery"] = hwBatteryLow();
  if (hwPower(p)) {
    o["battery"] = p.battery;
    o["vbus"] = p.vbus;
    o["vbusMv"] = p.vbusMv;
    o["systemMv"] = p.systemMv;
    o["charger"] = p.charger;
    o["pmicTempC"] = roundf(p.tempC * 10) / 10;
  }
}

static void imuJson(JsonObject o) {
  float ax, ay, az, gx, gy, gz;
  o["present"] = motionRaw(ax, ay, az);
  motionGyro(gx, gy, gz);
  JsonArray a = o["accelG"].to<JsonArray>();
  a.add(roundf(ax * 1000) / 1000);
  a.add(roundf(ay * 1000) / 1000);
  a.add(roundf(az * 1000) / 1000);
  JsonArray g = o["gyroDps"].to<JsonArray>();
  g.add(roundf(gx * 10) / 10);
  g.add(roundf(gy * 10) / 10);
  g.add(roundf(gz * 10) / 10);
  o["yawDeg"] = roundf(motionYaw() * 10) / 10;
}

static void stateJson(JsonObject o) {
  o["ok"] = true;
  o["name"] = FW_NAME;
  o["firmware"] = FW_VERSION;
  o["board"] = BOARD_ID;
  o["uptimeSec"] = millis() / 1000;
  o["bootCount"] = bootCount;
  o["wifi"] = netWifiState();
  powerJson(o["power"].to<JsonObject>());
  imuJson(o["imu"].to<JsonObject>());
  JsonObject keys = o["keys"].to<JsonObject>();
  for (uint8_t k = 0; k < HW_KEY_COUNT; k++)
    if (hwKeyPresent((HwKey)k)) keys[hwKeyName((HwKey)k)] = hwKeyDown((HwKey)k);
  JsonObject t = o["touch"].to<JsonObject>();
  t["present"] = touchPresent();
  t["down"] = touchDown();
  JsonObject in = o["lastInput"].to<JsonObject>();
  in["source"] = lastInput.source;
  in["event"] = lastInput.event;
  if (lastInput.x >= 0) {
    in["x"] = lastInput.x;
    in["y"] = lastInput.y;
  }
  in["agoMs"] = lastInput.ms ? millis() - lastInput.ms : 0;
  JsonObject s = o["sound"].to<JsonObject>();
  s["output"] = soundOutputName();
  s["playing"] = soundPlaying();
  s["volume"] = soundConfig().volume;
}

static void cliStatus(Print &out) {
  HwPower p;
  if (hwPower(p))
    out.printf("pmic: AXP2101 battery=%s %d mV %d%% charging=%d charger=%s vbus=%d (%d mV) sys=%d mV temp=%.1f C "
               "powerOff=%us\n",
               p.battery ? "yes" : "no", p.batteryMv, p.batteryPct, p.charging, p.charger, p.vbus, p.vbusMv, p.systemMv,
               p.tempC, p.powerOffSec);
  else out.println("pmic: not found");
  float ax, ay, az, gx, gy, gz;
  bool imu = motionRaw(ax, ay, az);
  motionGyro(gx, gy, gz);
  out.printf("imu: %s accel=(%.3f, %.3f, %.3f) g gyro=(%.1f, %.1f, %.1f) dps yaw=%.1f\n", imu ? "QMI8658" : "none", ax,
             ay, az, gx, gy, gz, motionYaw());
  out.printf("input: boot=%d key2=%d touch=%s down=%d last=%s %s", hwKeyDown(HW_KEY_BOOT), hwKeyDown(HW_KEY_2),
             touchPresent() ? "CST92xx" : "none", touchDown(), lastInput.source, lastInput.event);
  if (lastInput.x >= 0) out.printf(" (%d, %d)", lastInput.x, lastInput.y);
  out.println();
  out.printf("sound: %s playing=%d volume=%d | app: boots=%u screen=%dx%d\n", soundOutputName(), soundPlaying(),
             soundConfig().volume, (unsigned)bootCount, lcdWidth(), lcdHeight());
  soundDiag(out);
}

// ---------- CLI ----------

static void cliHelp(Print &out) {
  out.println(
      "radar:    tp <x> <y> [tap|long|left|right|up|down] | press [boot|key2] | hold [boot|key2]\n"
      "          imu | yaw0 | beep <hz> <ms> [square|sine] [<vol 0-100>] | fps [<frames>] | splash | i2c");
}

static bool keyArg(const String &arg, HwKey &k) {
  if (arg.isEmpty() || arg == "boot") k = HW_KEY_BOOT;
  else if (arg == "key2" || arg == "2") k = HW_KEY_2;
  else return false;
  return hwKeyPresent(k);
}

static bool cliCommand(const String &cmd, const String &rest) {
  if (cmd == "tp") {
    int x = -1, y = -1;
    char kind[8] = "tap";
    int n = sscanf(rest.c_str(), "%d %d %7s", &x, &y, kind);
    TouchKind k;
    if (n < 2 || x < 0 || y < 0 || x >= lcdWidth() || y >= lcdHeight() || !touchKindFromName(kind, k)) {
      Serial.printf("usage: tp <x 0-%d> <y 0-%d> [tap|long|left|right|up|down]\n", lcdWidth() - 1, lcdHeight() - 1);
      return true;
    }
    touchInject(k, x, y);
    Serial.println("ok");
  } else if (cmd == "press" || cmd == "hold") {
    HwKey k;
    String arg = rest;
    arg.toLowerCase();
    if (!keyArg(arg, k)) {
      Serial.println("usage: press|hold [boot|key2]");
      return true;
    }
    hwKeyInject(k, cmd == "press" ? HW_BTN_CLICK : HW_BTN_LONG);
    Serial.println("ok");
  } else if (cmd == "imu") {
    float ax, ay, az, gx, gy, gz;
    bool has = motionRaw(ax, ay, az);
    motionGyro(gx, gy, gz);
    float bx, by, bz;
    bool bias = motionGyroBias(bx, by, bz);
    Serial.printf("imu=%d accel=(%.3f, %.3f, %.3f) g gyro=(%.2f, %.2f, %.2f) dps yaw=%.1f deg bias=(%.2f, %.2f, %.2f)%s\n",
                  has, ax, ay, az, gx, gy, gz, motionYaw(), bx, by, bz, bias ? "" : " (not learned yet)");
  } else if (cmd == "yaw0") {
    motionResetYaw();
    Serial.println("ok");
  } else if (cmd == "beep") {
    int hz = 0, ms = 0, vol = -1;
    char wave[8] = "";
    int n = sscanf(rest.c_str(), "%d %d %7s %d", &hz, &ms, wave, &vol);
    SoundWave w = WAVE_DEFAULT;
    if (n < 2 || hz < 20 || hz > 20000 || ms < 1 || ms > 10000 || (n >= 3 && !soundWaveFromName(wave, w)) ||
        (n >= 4 && (vol < 0 || vol > 100))) {
      Serial.println("usage: beep <20-20000 Hz> <1-10000 ms> [square|sine] [<volume 0-100>]");
      return true;
    }
    soundToneEx(hz, ms, w, vol);
    Serial.printf("ok %s %d Hz %d ms volume %d via %s\n", soundWaveName(w), hz, ms, vol >= 0 ? vol : soundConfig().volume,
                  soundOutputName());
  } else if (cmd == "fps") {
    measureFps(rest.length() ? rest.toInt() : 60);
  } else if (cmd == "splash" || cmd == "boot") {
    drawSplash();
    Serial.println("ok");
  } else if (cmd == "state") {  // the body of GET /api/state
    JsonDocument doc;
    stateJson(doc.to<JsonObject>());
    serializeJson(doc, Serial);
    Serial.println();
  } else if (cmd == "i2c") {
    Serial.print("i2c:");
    for (uint8_t a = 1; a < 127; a++) {
      Wire.beginTransmission(a);
      if (Wire.endTransmission() == 0) Serial.printf(" 0x%02X", a);
    }
    Serial.println();
  } else {
    return false;
  }
  return true;
}

// ---------- HTTP ----------

static void routes(WebServer &server) {
  server.on("/api/state", HTTP_GET, []() {
    JsonDocument doc;
    stateJson(doc.to<JsonObject>());
    netSendJson(doc);
  });
}

static void info(JsonObject o) {
  o["imu"] = motionHasImu();
  HwPower p;
  if (hwPower(p)) {
    o["vbus"] = p.vbus;
    o["charging"] = p.charging;  // from the PMIC on this board, not a guess
  }
  o["sound"] = soundOutputName();
}

static void netEvent(NetEvent e, const char *arg) {
  static const char *const names[] = {"ap-started", "ap-closed", "connected", "disconnected", "wifi-off", "wifi-on"};
  Serial.printf("[net] %s %s\n", e <= NET_WIFI_ON ? names[e] : "?", arg ? arg : "");
}

static LedNetState ledNetState() {
  if (!netWifiEnabled()) return LED_NET_OFF;
  return netConnected() ? LED_NET_CONNECTED : netApMode() ? LED_NET_AP : LED_NET_CONNECTING;
}

static void loadAppState() {
  Preferences p;
  p.begin(APP_PREFS_NS, false);
  bootCount = p.getUInt("boots", 0) + 1;
  p.putUInt("boots", bootCount);
  p.end();
}

void setup() {
  Serial.begin(115200);
  hwBegin();  // I2C, keys, PMIC
  devLoad();  // WiFi, clock, screen and sound settings
  loadAppState();
  gfx = lcdBegin(devRotation());
  drawSplash();
  lcdSetBacklight(devBacklightLevel(false));
  motionBegin(devRotation());
  touchBegin(devRotation());
  soundBegin();
  ledBegin({nullptr, ledNetState});
  NetHooks hooks = {};
  hooks.routes = routes;
  hooks.info = info;
  hooks.event = netEvent;
  netBegin(hooks);
  cliBegin({cliCommand, cliHelp, cliStatus});
  Serial.printf("%s %s on %s, boot %u\n", FW_NAME, FW_VERSION, BOARD_NAME, (unsigned)bootCount);
}

void loop() {
  netLoop();
  motionLoop();
  cliLoop();
  pollKeys();
  pollTouch();
  hwLoop();
  if (hwBatteryLowAlarmDue()) Serial.println("[hw] battery low");
  soundLoop();
  ledLoop();
  applyBacklight();
  if (netRestartRequested()) {
    delay(300);  // let the HTTP response go out
    ESP.restart();
  }
  delay(1);
}
