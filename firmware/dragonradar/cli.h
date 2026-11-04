#pragma once
// Serial console shared by every app on this base. One command per line (CR or LF), the command
// word is case-insensitive and lines longer than CLI_MAX_LINE (config.h, default 256) are
// rejected. The common commands cover WiFi, clock, screen, LED, buzzer, battery and NVS; the app
// adds its own through CliHooks and lists them in `help`.
//
// Common: help | status | wifi [<ssid> [<pass>]] | wifi "<ssid with spaces>" [<pass>] | forget |
//         ap | openap | tz [<posix>] | host [<name>] | bl [<5-255>] | rot [<0-3>] | ndim [on|off] |
//         led [off|<app mode>|status|#rrggbb|pin <gpio>|bri <0-255>] | buzzer [on|off|pin <gpio>] |
//         sound <notes> | rtttl <rtttl> | sfx [<name>|on|off] | vol [<0-100>] | stop | bat | shot |
//         gpio <n> | nvs erase | reboot
#include "config.h"
#include <Arduino.h>

// Every member may be null.
struct CliHooks {
  // App commands, tried after the common ones. `cmd` is lowercased, `args` trimmed. Returns false
  // when the command is unknown.
  bool (*command)(const String &cmd, const String &args);
  void (*help)(Print &out);    // app lines for `help`
  void (*status)(Print &out);  // app part of `status`, printed before the device lines
};

void cliBegin(const CliHooks &hooks);
void cliLoop();                  // reads Serial
void cliRun(const String &line);  // runs one line, e.g. from another transport
