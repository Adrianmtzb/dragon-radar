#pragma once
// The Dragon Radar: renders the radar of docs/design.md on the round canvas (the same picture as
// tools/preview/index.html, from the constants of radar_assets.h), runs the beep loop, the boot
// and power-off timelines and the zoom levels, and keeps the user's settings in Preferences
// ("dradar": zoom, theme, vol, muted, man + balls).
//
// The first block is the contract of docs/api.md (radar_api.cpp calls only these); the rest is
// for the sketch's inputs and serial console.
#include <Arduino.h>
#include <ArduinoJson.h>

void radarBegin();                                // after lcdBegin(), soundBegin() and motionBegin()
void radarLoop(uint32_t nowMs);                   // draws, beeps, animates
void radarStateJson(JsonObject out);              // the "radar" object of GET /api/state
const char *radarApply(JsonObjectConst in);       // POST /api/radar; nullptr or an error message
const char *radarSetBalls(JsonObjectConst in);    // POST /api/balls; nullptr or an error message
// A physical press went down (src "boot", "key2", "touch"): plays the press sound at once, even
// with the radar off (muted: a dry run that logs when it would have started).
void radarPressDown(const char *src);
bool radarPlaySfx(const char *name);              // false when unknown, off or muted

// Inputs. Zoom out wraps from the widest level to the closest; zoom in stops at the closest.
void radarZoomOut();
void radarZoomIn();
void radarStepTheme(int dir);  // +1 next, -1 previous
void radarPowerToggle();
bool radarIsOn();              // "on" or "boot"
bool radarReady();             // "on": the boot animation is over
void radarResetHeading();      // the current orientation becomes north (yaw0)
void radarInvalidate();        // redraw the whole frame next time (after something else drew)
// Holds the radar's own beeps (and its blink and ring) while `on`, e.g. during a recording or while
// another screen shows; nothing else changes. Nested calls are not counted: the last one wins.
void radarPauseBeeps(bool on);
// Another screen (the info card) takes the panel: no rendering and no beeps until resumed, then the
// radar redraws its whole frame. Its state (zoom, theme, sweep, balls, timers) carries on.
void radarSuspend(bool on);
bool radarSuspended();
// For such a screen: the current theme's glass, dimmed by `keep` (0-1), round-clipped, into a
// W x H RGB565 framebuffer; and the theme's colours (0xRRGGBB) for text, accent and secondary text.
void radarCardBackground(uint16_t *fb, float keep);
void radarCardColors(uint32_t &text, uint32_t &accent, uint32_t &dim);

// Serial console: radar | zoom | theme | mute | power | yaw0 | ball | balls | sfx | blink.
// radarCli runs one of them (false when `cmd` is not a radar command).
bool radarCli(const String &cmd, const String &args);
void radarCliHelp(Print &out);
