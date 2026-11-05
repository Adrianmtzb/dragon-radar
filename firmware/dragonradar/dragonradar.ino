// Dragon Radar: a Dragon Ball style radar on the Waveshare ESP32-S3-Touch-AMOLED-1.75C.
// The radar (radar.h) owns the screen and the sounds; this sketch wires the board to it: keys,
// touch, IMU, PMIC, WiFi with the setup portal, the HTTP API (radar_api.h) and the serial console.
//
// Controls: BOOT click = zoom out, Key2 click = zoom in (both wrap around), Key2 hold (600 ms) = power off/on
// (holding Key2 6 s switches the PMIC off: that is the board's own power key), BOOT hold = info
// card. On the screen: tap = zoom out, double tap = info card, swipe left/right = next/previous
// theme, long press = reset the heading (north = now). Every press, key or finger, plays the press
// sound as it goes down (radarPressDown), only while the radar is on.
#include <ArduinoJson.h>
#include <Preferences.h>
#include <Wire.h>
#include "cli.h"
#include "config.h"
#include "devsettings.h"
#include "hw.h"
#include "lcd.h"
#include "led.h"
#include "info.h"
#include "mic.h"
#include "motion.h"
#include "net.h"
#include "radar.h"
#include "radar_api.h"
#include "sound.h"
#include "touch.h"

static Arduino_Canvas *gfx = nullptr;
static bool recActive = false;  // a 'rec' is running: the radar's beeps hold off until it is dumped
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

// Raw full-screen flush rate: `frames` frames of a changing fill, flush timed on its own and with
// the fill. The radar redraws its whole frame afterwards.
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
  radarInvalidate();
}

// ---------- input ----------

// The info card: BOOT long press, a double tap or the CLI 'info' open it for INFO_MS; any press or
// gesture closes it (and does nothing else).
static const uint32_t INFO_MS = 10000, INFO_SHORT_MS = 2200;

// BOOT click: zoom out; BOOT hold: info card. Key2 click: zoom in; Key2 hold: power off/on.
static const char *onKey(HwKey k, HwButtonEvent e) {
  if (e == HW_BTN_LONG && k == HW_KEY_2) {
    infoHide();
    radarPowerToggle();
    return radarIsOn() ? "power on" : "power off";
  }
  if (infoShowing()) {
    if (e != HW_BTN_CLICK && e != HW_BTN_LONG) return nullptr;
    infoHide();
    return "close info";
  }
  if (e == HW_BTN_LONG && k == HW_KEY_BOOT && radarIsOn()) {
    infoShow(true, INFO_MS);
    return "info";
  }
  if (e != HW_BTN_CLICK || !radarIsOn()) return nullptr;
  if (k == HW_KEY_BOOT) radarZoomOut();
  else radarZoomIn();
  return k == HW_KEY_BOOT ? "zoom out" : "zoom in";
}

// Every press plays the press sound on its down edge (radarPressDown), before anything else.
// `pollGapMs`: time since the previous poll, so the edge happened at most that long before.
static uint32_t lastKeyPollMs = 0;

static void pollKeys() {
  const uint32_t now = millis(), gap = lastKeyPollMs ? now - lastKeyPollMs : 0;
  lastKeyPollMs = now;
  for (uint8_t k = 0; k < HW_KEY_COUNT; k++) {
    if (!hwKeyPresent((HwKey)k)) continue;
    HwButtonEvent e = hwKeyPoll((HwKey)k);
    if (e == HW_BTN_NONE) continue;
    if (e == HW_BTN_DOWN) {
      Serial.printf("[key] %s down at %u ms (edge within the last %u ms poll gap) -> press sound\n", hwKeyName((HwKey)k),
                    (unsigned)now, (unsigned)gap);
      radarPressDown(hwKeyName((HwKey)k));
      continue;
    }
    noteInput(hwKeyName((HwKey)k), hwButtonEventName(e));
    const char *did = onKey((HwKey)k, e);
    Serial.printf("[key] %s %s%s%s\n", hwKeyName((HwKey)k), hwButtonEventName(e), did ? " -> " : "", did ? did : "");
  }
}

// Touch: a tap waits BUTTON_DOUBLE_CLICK_MS for a second one; a single tap zooms out, a double tap
// opens the info card. Swipe left/right: next/previous theme. Long press: north = where it points.
static uint32_t tapPendingMs = 0;  // a first tap waiting for its pair, 0 = none
static bool touchWasDown = false;  // for the finger's down edge

// Simulated presses (CLI press/hold/tp): the down edge now and the event when a finger or key would
// give it (a click on release ~80 ms later, a hold after BUTTON_LONG_PRESS_MS), so the press sound
// and the action keep their real timing.
struct SimStep {
  uint32_t at;
  uint8_t what;  // 0 = key edge, 1 = key event, 2 = touch edge, 3 = touch gesture
  uint8_t key, ev;
  int16_t x, y;
};
static SimStep sims[8];
static int simCount = 0;

static void simAdd(uint32_t delayMs, uint8_t what, uint8_t key, uint8_t ev, int16_t x = 0, int16_t y = 0) {
  if (simCount < (int)(sizeof(sims) / sizeof(sims[0]))) sims[simCount++] = {millis() + delayMs, what, key, ev, x, y};
}

static void simLoop() {
  const uint32_t now = millis();
  for (int i = 0; i < simCount;) {
    SimStep st = sims[i];
    if ((int32_t)(now - st.at) < 0) {
      i++;
      continue;
    }
    memmove(sims + i, sims + i + 1, sizeof(SimStep) * (simCount - i - 1));
    simCount--;
    if (st.what == 0 || st.what == 2) {
      const char *src = st.what == 0 ? hwKeyName((HwKey)st.key) : "touch";
      Serial.printf("[%s] %s down at %u ms (simulated) -> press sound\n", st.what == 0 ? "key" : "touch", src,
                    (unsigned)now);
      radarPressDown(src);
    } else if (st.what == 1) {
      hwKeyInject((HwKey)st.key, (HwButtonEvent)st.ev);
    } else {
      touchInject((TouchKind)st.ev, st.x, st.y);
    }
  }
}

static const char *onTouch(const TouchEvent &e) {
  if (infoShowing()) {
    tapPendingMs = 0;
    infoHide();
    return "close info";
  }
  if (!radarIsOn()) return nullptr;
  switch (e.kind) {
    case TOUCH_TAP:
      if (tapPendingMs) {
        tapPendingMs = 0;
        infoShow(true, INFO_MS);
        return "double tap: info";
      }
      tapPendingMs = millis() | 1;
      return "tap (waiting for a second one)";
    case TOUCH_SWIPE_LEFT: radarStepTheme(1); return "next theme";
    case TOUCH_SWIPE_RIGHT: radarStepTheme(-1); return "previous theme";
    case TOUCH_LONG_PRESS: radarResetHeading(); return "heading reset";
    default: return nullptr;
  }
}

static void pollTouch() {
  if (tapPendingMs && (int32_t)(millis() - tapPendingMs) > (int32_t)BUTTON_DOUBLE_CLICK_MS) {
    tapPendingMs = 0;  // no second tap: it was a single one
    if (radarIsOn() && !infoShowing()) {
      radarZoomOut();
      Serial.println("[touch] single tap -> zoom out");
    }
  }
  TouchEvent e = touchPoll();
  const bool down = touchDown();
  if (down && !touchWasDown) {  // seen on two polls in a row (touch.cpp): 15-30 ms after contact
    Serial.printf("[touch] down at %u ms -> press sound\n", (unsigned)millis());
    radarPressDown("touch");
  }
  touchWasDown = down;
  if (e.kind == TOUCH_NONE) return;
  noteInput("touch", touchKindName(e.kind), e.x, e.y);
  const char *did = onTouch(e);
  if (e.rawX < 0) Serial.printf("[touch] %s x=%d y=%d (simulated)", touchKindName(e.kind), e.x, e.y);
  else Serial.printf("[touch] %s x=%d y=%d raw=(%d, %d)", touchKindName(e.kind), e.x, e.y, e.rawX, e.rawY);
  Serial.printf("%s%s\n", did ? " -> " : "", did ? did : "");
}

// The short card (name, version, board) once the boot animation is over, as on the sibling apps.
static void bootCard() {
  static bool done = false;
  if (done || !radarReady()) return;
  done = true;
  infoShow(false, INFO_SHORT_MS);
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
      "board:    tp <x> <y> [tap|double|long|left|right|up|down] | press [boot|key2] | hold [boot|key2]\n"
      "          info [short|close]\n"
      "          imu | beep <hz> <ms> [square|sine|tri] [<vol 0-100>] | fps [<frames>] | state | i2c\n"
      "          rec <ms> [<sfx> | tone <hz> <ms> [<wave>] [<vol>]] | micgain [0-14]");
  radarCliHelp(out);
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
    TouchKind k = TOUCH_TAP;
    const bool dbl = !strcmp(kind, "double");
    if (n < 2 || x < 0 || y < 0 || x >= lcdWidth() || y >= lcdHeight() || (!dbl && !touchKindFromName(kind, k))) {
      Serial.printf("usage: tp <x 0-%d> <y 0-%d> [tap|double|long|left|right|up|down]\n", lcdWidth() - 1, lcdHeight() - 1);
      return true;
    }
    // Like a finger: down now, the gesture on lift (tap 80 ms, swipe 150 ms) or while still down
    // (long press); a double tap is two taps 150 ms apart.
    const uint32_t lift = k == TOUCH_TAP ? 80 : k == TOUCH_LONG_PRESS ? BUTTON_LONG_PRESS_MS : 150;
    simAdd(0, 2, 0, 0);
    simAdd(lift, 3, 0, dbl ? TOUCH_TAP : k, x, y);
    if (dbl) {
      simAdd(150, 2, 0, 0);
      simAdd(230, 3, 0, TOUCH_TAP, x, y);
    }
    Serial.println("ok");
  } else if (cmd == "info") {
    if (rest == "close" || rest == "off") infoHide();
    else if (!radarIsOn()) Serial.println("error: the radar is off");
    else infoShow(rest != "short", rest == "short" ? INFO_SHORT_MS : INFO_MS);
    Serial.printf("info=%s\n", infoShowing() ? "shown" : "hidden");
  } else if (cmd == "press" || cmd == "hold") {
    HwKey k;
    String arg = rest;
    arg.toLowerCase();
    if (!keyArg(arg, k)) {
      Serial.println("usage: press|hold [boot|key2]");
      return true;
    }
    // Down now; a click on release 80 ms later, or the long press after BUTTON_LONG_PRESS_MS.
    simAdd(0, 0, k, 0);
    simAdd(cmd == "press" ? 80 : BUTTON_LONG_PRESS_MS, 1, k, cmd == "press" ? HW_BTN_CLICK : HW_BTN_LONG);
    Serial.println("ok");
  } else if (cmd == "imu") {
    float ax, ay, az, gx, gy, gz;
    bool has = motionRaw(ax, ay, az);
    motionGyro(gx, gy, gz);
    float bx, by, bz;
    bool bias = motionGyroBias(bx, by, bz);
    Serial.printf("imu=%d accel=(%.3f, %.3f, %.3f) g gyro=(%.2f, %.2f, %.2f) dps yaw=%.1f deg bias=(%.2f, %.2f, %.2f)%s\n",
                  has, ax, ay, az, gx, gy, gz, motionYaw(), bx, by, bz, bias ? "" : " (not learned yet)");
  } else if (cmd == "beep") {
    int hz = 0, ms = 0, vol = -1;
    char wave[8] = "";
    int n = sscanf(rest.c_str(), "%d %d %7s %d", &hz, &ms, wave, &vol);
    SoundWave w = WAVE_DEFAULT;
    if (n < 2 || hz < 20 || hz > 20000 || ms < 1 || ms > 10000 || (n >= 3 && !soundWaveFromName(wave, w)) ||
        (n >= 4 && (vol < 0 || vol > 100))) {
      Serial.println("usage: beep <20-20000 Hz> <1-10000 ms> [square|sine|tri] [<volume 0-100>]");
      return true;
    }
    soundToneEx(hz, ms, w, vol);
    Serial.printf("ok %s %d Hz %d ms volume %d via %s\n", soundWaveName(w), hz, ms, vol >= 0 ? vol : soundConfig().volume,
                  soundOutputName());
  } else if (cmd == "fps") {
    measureFps(rest.length() ? rest.toInt() : 60);
  } else if (cmd == "state") {  // the body of GET /api/state
    JsonDocument doc;
    stateJson(doc.to<JsonObject>());
    radarStateJson(doc["radar"].to<JsonObject>());
    serializeJson(doc, Serial);
    Serial.println();
  } else if (cmd == "rec") {
    // rec <ms> [<sfx> | tone <hz> <ms> [square|sine|tri] [<vol>]]: the board's microphones while it
    // plays; the radar's own beeps hold off meanwhile. The dump (REC ... ENDREC) follows when done.
    int ms = 0, hz = 0, tms = 0, vol = -1;
    char what[24] = "", wave[8] = "square";
    int n = sscanf(rest.c_str(), "%d %23s %d %d %7s %d", &ms, what, &hz, &tms, wave, &vol);
    if (n < 1 || ms < 50 || ms > 4000) {
      Serial.println("usage: rec <50-4000 ms> [<sfx name> | tone <hz> <ms> [square|sine|tri] [<vol>]]");
      return true;
    }
    // Quiet first: the radar's beep loop holds off, and what the codec still has queued plays out.
    radarPauseBeeps(true);
    soundStop();
    delay(150);
    if (!micPresent() || !micRecordStart(ms)) {
      radarPauseBeeps(false);
      Serial.println("error: no microphone, or a recording is running");
      return true;
    }
    recActive = true;
    const uint32_t t0 = millis();
    while (!micRecordStarted() && millis() - t0 < 300) delay(1);  // the recorder drops its stale queue
    delay(30);  // a little silence before the sound
    if (!strcmp(what, "tone")) {
      SoundWave w = WAVE_DEFAULT;
      soundWaveFromName(wave, w);
      soundToneEx(hz, tms, w, vol);
    } else if (what[0] && !radarPlaySfx(what)) {
      Serial.printf("note: sfx '%s' not played (unknown, muted or radar off)\n", what);
    }
    Serial.printf("rec %d ms at %u Hz, mic gain step %d\n", ms, (unsigned)soundSampleRate(), micGain());
  } else if (cmd == "micgain") {
    if (rest.length() && !micSetGain(rest.toInt())) Serial.println("error: micgain 0-14 (0-37.5 dB)");
    else Serial.printf("micgain=%d\n", micGain());
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

// GET /api/state (these fields plus "radar"), POST /api/radar, /api/balls and /api/sfx.
static void routes(WebServer &server) { radarApiRegister(server, stateJson); }

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
  motionBegin(devRotation());
  touchBegin(devRotation());
  soundBegin();
  micBegin();
  radarBegin();  // plays the power-on timeline; the panel brightness follows it
  ledBegin({nullptr, ledNetState});
  NetHooks hooks = {};
  hooks.routes = routes;
  hooks.info = info;
  hooks.event = netEvent;
  netBegin(hooks);
  // radarCli goes first: it gives 'sfx <name>' the radar's gain and mute.
  cliBegin({cliCommand, cliHelp, cliStatus, radarCli});
  Serial.printf("%s %s on %s, boot %u\n", FW_NAME, FW_VERSION, BOARD_NAME, (unsigned)bootCount);
}

void loop() {
  netLoop();
  motionLoop();
  cliLoop();
  simLoop();
  pollKeys();
  pollTouch();
  hwLoop();
  if (hwBatteryLowAlarmDue()) Serial.println("[hw] battery low");
  soundLoop();
  if (recActive && !micRecording()) {
    micLoop(Serial);  // the dump of a finished 'rec'
    recActive = false;
    radarPauseBeeps(false);
  }
  bootCard();
  infoLoop();
  radarLoop(millis());
  ledLoop();
  if (netRestartRequested()) {
    delay(300);  // let the HTTP response go out
    ESP.restart();
  }
  delay(1);
}
