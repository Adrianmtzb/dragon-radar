#include "cli.h"
#include "devsettings.h"
#include "hw.h"
#include "lcd.h"
#include "led.h"
#include "net.h"
#include "sound.h"
#include "touch.h"

#ifndef CLI_MAX_LINE
#define CLI_MAX_LINE 256
#endif

static CliHooks hooks = {};
static String line;
static bool overflow = false;

void cliBegin(const CliHooks &h) {
  hooks = h;
  line.reserve(CLI_MAX_LINE < 256 ? CLI_MAX_LINE : 256);
}

static void rebootSoon(const char *msg) {
  Serial.println(msg);
  Serial.flush();
  delay(200);
  ESP.restart();
}

static void printHelp() {
  Serial.println(
      "commands: help | status | wifi [on|off|<ssid> [<pass>]] | wifi \"<ssid with spaces>\" [<pass>] | forget | ap\n"
      "          tz [<posix>] | host [<name>] | bl [<5-255>] | rot [<0-3>] | ndim [on|off]");
  Serial.printf("          led [off|%s|status|#rrggbb|pin <gpio>|bri <0-255>] | buzzer [on|off|pin <gpio>]\n",
                ledModeName(LED_APP));
  Serial.println(
      "          sound <notes> | rtttl <rtttl> | sfx [<name>|on|off] | vol [<0-100>] | stop\n"
      "          bat | shot | gpio <n> | nvs erase | reboot");
  if (hooks.help) hooks.help(Serial);
}

static void printBattery() {
  if (hwHasBattery())
    Serial.printf("battery: %d mV %d%%%s%s\n", hwBatteryMv(), hwBatteryPct(), hwBatteryLow() ? " low" : "",
                  hwBatteryCharging() ? " charging" : "");
  else Serial.println("battery: no gauge on this board");
}

static void printStatus() {
  if (hooks.status) hooks.status(Serial);
  Serial.println(netStatusLine());
  Serial.printf("board: %s fw=%s %s heap=%u psram=%u touch=%d\n", BOARD_NAME, FW_NAME, FW_VERSION, ESP.getFreeHeap(),
                ESP.getFreePsram(), touchPresent());
  Serial.printf("time: %s source=%s tz=%s night=%d\n", netTimeValid() ? netLocalTime().c_str() : "not set",
                netTimeSource(), devTz().c_str(), netIsNight());
  Serial.printf("screen: backlight=%u duty=%u rot=%u nightDim=%d\n", devBacklight(), (unsigned)lcdBacklightDuty(),
                devRotation(), devNightDim());
  if (hwHasBattery()) printBattery();
}

static String pinList(bool led) {
  String s;
  int n = led ? ledPinCount() : soundPinCount();
  for (int i = 0; i < n; i++) s += (i ? ", " : "") + String(led ? ledPinAt(i) : soundPinAt(i));
  return n ? s : String("none on this board");
}

// "led ..." and "buzzer ...": returns an error message or "".
static String setOutput(bool led, String arg) {
  LedConfig lc = ledConfig();
  SoundConfig sc = soundConfig();
  arg.toLowerCase();
  if (arg.startsWith("pin")) {
    String p = arg.substring(3);
    p.trim();
    int pin = p.length() ? p.toInt() : -1;
    if (led ? !ledPinAllowed(pin) : !soundPinAllowed(pin)) return "allowed pins: " + pinList(led);
    if (led) lc.pin = pin;
    else sc.pin = pin;
  } else if (led && arg.startsWith("bri")) {
    lc.brightness = constrain(arg.substring(3).toInt(), 0, 255);
  } else if (led) {
    LedMode m;
    if (ledModeFromName(arg.c_str(), m)) lc.mode = m;
    else if (netParseColor(arg.c_str(), lc.color)) lc.mode = LED_COLOR;
    else return String("use off, ") + ledModeName(LED_APP) + ", status, #rrggbb, pin <gpio> or bri <0-255>";
  } else if (arg == "on" || arg == "off") {
    sc.enabled = arg == "on";
  } else {
    return "use on, off or pin <gpio>";
  }
  String err;
  if (led ? !ledConfigure(lc, err) : !soundConfigure(sc, err)) return err;
  devSaveOutputs();
  return "";
}

static void printLed() {
  char hex[8];
  netFormatColor(ledConfig().color, hex);
  Serial.printf("led mode=%s color=%s bri=%d pin=%d (allowed: %s)\n", ledModeName(ledConfig().mode), hex,
                ledConfig().brightness, ledConfig().pin, pinList(true).c_str());
}

static void printBuzzer() {
  Serial.printf("buzzer %s pin=%d vol=%d sfx=%s (allowed: %s)\n", soundHasBuzzer() ? "on" : "off", soundConfig().pin,
                soundConfig().volume, soundConfig().effects ? "on" : "off", pinList(false).c_str());
}

static bool setSound(const SoundConfig &sc) {
  String err;
  if (!soundConfigure(sc, err)) {
    Serial.println("error: " + err);
    return false;
  }
  devSaveOutputs();
  return true;
}

static void cmdWifi(const String &rest) {
  if (rest.isEmpty()) {
    Serial.println(netStatusLine());
    return;
  }
  if (rest == "on" || rest == "off") {  // a network really called "on" or "off" goes in quotes
    netSetWifiEnabled(rest == "on");
    Serial.println(netStatusLine());
    return;
  }
  // An SSID with spaces goes in double quotes; the password is the rest of the line.
  String ssid, pass;
  if (rest.startsWith("\"")) {
    int q = rest.indexOf('"', 1);
    if (q < 0) {
      Serial.println("error: missing closing quote after the SSID");
      return;
    }
    ssid = rest.substring(1, q);
    pass = rest.substring(q + 1);
  } else {
    int sp = rest.indexOf(' ');
    ssid = sp < 0 ? rest : rest.substring(0, sp);
    pass = sp < 0 ? "" : rest.substring(sp + 1);
  }
  pass.trim();
  if (!devSaveWifi(ssid, pass)) {
    Serial.println("error: ssid must be 1-32 chars, pass up to 64");
    return;
  }
  rebootSoon("saved, rebooting");
}

// Common commands; false when `cmd` is not one of them.
static bool runCommon(const String &cmd, const String &rest) {
  if (cmd == "help") {
    printHelp();
  } else if (cmd == "status") {
    printStatus();
  } else if (cmd == "wifi") {
    cmdWifi(rest);
  } else if (cmd == "forget") {
    devForgetWifi();
    rebootSoon("wifi credentials erased, rebooting into setup mode");
  } else if (cmd == "ap" || cmd == "openap") {
    netOpenAp();
    Serial.printf("setup network %s open at http://%s (credentials kept; reboot to rejoin WiFi)\n",
                  netApSsid().c_str(), netIp().c_str());
  } else if (cmd == "tz") {
    if (rest.isEmpty()) Serial.printf("tz=%s\n", devTz().c_str());
    else if (devSetTz(rest)) Serial.printf("tz=%s local=%s\n", devTz().c_str(), netLocalTime().c_str());
    else Serial.println("error: POSIX TZ such as CST6 or CET-1CEST,M3.5.0,M10.5.0/3");
  } else if (cmd == "host") {
    if (rest.isEmpty()) Serial.printf("host=%s.local\n", devHostname().c_str());
    else if (!devSetHostname(rest)) Serial.println("error: 1-24 chars of a-z, 0-9 and -");
    else rebootSoon(("hostname " + devHostname() + ".local saved, rebooting").c_str());
  } else if (cmd == "bl") {
    if (rest.length()) {
      devSetBacklight(rest.toInt());
      lcdSetBacklight(devBacklightLevel(netIsNight()));
    }
    Serial.printf("backlight=%u duty=%u nightDim=%d\n", devBacklight(), (unsigned)lcdBacklightDuty(), devNightDim());
  } else if (cmd == "rot") {
    if (rest.isEmpty()) Serial.printf("rot=%u\n", devRotation());
    else if (rest.length() != 1 || !devSetRotation(rest.toInt())) Serial.println("error: rot 0-3");
    else rebootSoon("rotation saved, rebooting");
  } else if (cmd == "ndim") {
    if (rest == "on" || rest == "off") {
      devSetNightDim(rest == "on");
      lcdSetBacklight(devBacklightLevel(netIsNight()));
    } else if (rest.length()) {
      Serial.println("error: ndim on|off");
      return true;
    }
    Serial.printf("nightDim=%s\n", devNightDim() ? "on" : "off");
  } else if (cmd == "led" || cmd == "buzzer") {
    bool led = cmd == "led";
    String err = rest.length() ? setOutput(led, rest) : String("");
    if (err.length()) Serial.println("error: " + err);
    else if (led) printLed();
    else printBuzzer();
  } else if (cmd == "sound" || cmd == "rtttl") {
    String err;
    bool ok = cmd == "sound" ? soundPlayNotes(rest.c_str(), 120, 1, err) : soundPlayRtttl(rest.c_str(), 1, err);
    if (ok) Serial.printf("ok %ums%s\n", (unsigned)soundRemainingMs(), soundHasBuzzer() ? "" : " (no buzzer)");
    else Serial.println("error: " + err);
  } else if (cmd == "sfx") {
    if (rest == "on" || rest == "off") {
      SoundConfig sc = soundConfig();
      sc.effects = rest == "on";
      if (setSound(sc)) Serial.printf("sfx=%s\n", rest.c_str());
    } else if (rest.isEmpty()) {
      for (int i = 0; i < SFX_COUNT; i++) Serial.printf("  %-8s %s\n", SFX[i].name, SFX[i].notes);
    } else {
      String err;
      int i = soundSfxFind(rest.c_str());
      if (i < 0) Serial.println("error: unknown sfx, run 'sfx' to list them");
      else if (soundPlayNotes(SFX[i].notes, SFX[i].tempo, 1, err)) Serial.println("ok");
      else Serial.println("error: " + err);
    }
  } else if (cmd == "vol") {
    SoundConfig sc = soundConfig();
    if (rest.length()) {
      sc.volume = constrain(rest.toInt(), 0, 100);
      if (!setSound(sc)) return true;
    }
    Serial.printf("volume=%d\n", sc.volume);
  } else if (cmd == "stop") {
    soundStop();
    Serial.println("ok");
  } else if (cmd == "bat") {
    printBattery();
  } else if (cmd == "shot") {
    lcdShot(Serial);  // tools/screenshot.py turns it into a PNG
  } else if (cmd == "gpio") {
    if (rest.isEmpty()) Serial.println("usage: gpio <n>");
    else Serial.printf("gpio %d = %d\n", (int)rest.toInt(), digitalRead(rest.toInt()));  // bring-up aid
  } else if (cmd == "nvs") {
    String r = rest;
    r.toLowerCase();
    if (r != "erase") Serial.println("usage: nvs erase (wipes every setting, WiFi included, and reboots)");
    else {
      devEraseAll();
      rebootSoon("settings erased, rebooting");
    }
  } else if (cmd == "reboot") {
    rebootSoon("rebooting");
  } else {
    return false;
  }
  return true;
}

void cliRun(const String &input) {
  String l = input;
  l.trim();
  if (l.isEmpty()) return;
  int sp = l.indexOf(' ');
  String cmd = sp < 0 ? l : l.substring(0, sp);
  String rest = sp < 0 ? String("") : l.substring(sp + 1);
  rest.trim();
  cmd.toLowerCase();
  if (runCommon(cmd, rest)) return;
  if (hooks.command && hooks.command(cmd, rest)) return;
  Serial.println("unknown command, try 'help'");
}

void cliLoop() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (overflow) Serial.printf("error: line longer than %d chars\n", CLI_MAX_LINE);
      else if (line.length()) cliRun(line);
      line = "";
      overflow = false;
    } else if (line.length() < CLI_MAX_LINE) {
      line += c;
    } else {
      overflow = true;
    }
  }
}
