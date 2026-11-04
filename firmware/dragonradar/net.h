#pragma once
// Network layer shared by every app on this base: WiFi STA with a captive-portal setup AP as
// fallback, the HTTP server with the common API, mDNS, the clock (NTP, or the browser while there
// is no internet) and discovery of other boards running the same app. App-specific parts come in
// through NetHooks; names and defaults come from config.h (FW_NAME, FW_VERSION, AP_SSID_PREFIX,
// MDNS_SERVICE, NTP_SERVER, SETUP_ACCENT, DONATE_URL, and for the credits PROJECT_URL (the repo)
// and AUTHOR_URL). Settings live in devsettings.h.
//
// Common routes: GET / (web/index.html), GET /setup (small setup page), GET /api/info,
// POST /api/settings, GET|POST /api/wifi, POST /api/time, POST /api/reboot, GET|POST /api/led,
// GET|POST /api/sound. Errors are {ok: false, error} with 400, 404, 413 or 429.
#include "config.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <IPAddress.h>
#include <WebServer.h>

enum NetEvent : uint8_t {
  NET_AP_STARTED,    // arg: setup network SSID
  NET_AP_CLOSED,     // arg: setup network SSID
  NET_CONNECTED,     // arg: IP address
  NET_DISCONNECTED,  // arg: saved SSID
  NET_WIFI_OFF,      // the radio was switched off (netSetWifiEnabled(false)); arg: ""
  NET_WIFI_ON,       // and back on; arg: ""
};

// Every member may be null.
struct NetHooks {
  void (*routes)(WebServer &server);            // app routes, registered before the common ones
  void (*info)(JsonObject info);                // app fields for GET /api/info (may override common ones)
  void (*event)(NetEvent event, const char *arg);
  void (*timeValid)(uint32_t epoch);            // once, when the clock first becomes valid
  // App fields of POST /api/settings. Runs after the common fields have been validated and before
  // they are applied: validate everything first, then apply, and write the app's current values
  // into `out` (also when `body` has none of them). Returning false answers 400 with `err` and
  // leaves the common fields untouched.
  bool (*settings)(JsonObjectConst body, JsonObject out, String &err);
};

void netBegin(const NetHooks &hooks);  // after devLoad()
void netLoop();

// HTTP helpers for app routes.
WebServer &netServer();
void netSendJson(JsonDocument &doc, int code = 200);
void netSendError(int code, const String &msg);
void netSendOk();
bool netReadBody(JsonDocument &doc);  // false after answering 400 (no or bad JSON) or 413 (too big)
bool netParseColor(const char *s, uint32_t &out);  // "#rrggbb", "rrggbb" or "#rgb" -> 0xRRGGBB
void netFormatColor(uint32_t color, char *out);     // 0xRRGGBB -> "#rrggbb" (out holds 8 chars)

// WiFi state.
bool netConnected();   // joined the saved network
bool netApMode();      // the setup network is up
bool netSetupMode();   // on its own network only: AP up, not joined
bool netStaFailed();   // the saved network could not be joined since boot
String netIp();        // STA IP, AP IP in setup mode, "" otherwise
String netSsid();      // joined network, "" otherwise
int netRssi();         // 0 when not joined
String netApSsid();    // AP_SSID_PREFIX + 4 hex digits of the eFuse MAC
// Radio switch, persisted in devsettings ("wifi") and honoured at boot. Off means WIFI_OFF: no
// STA, no setup AP, no scans, no retries, and the web server and mDNS stopped. On resumes the
// normal flow (saved network, or the setup AP without credentials or when joining fails).
void netSetWifiEnabled(bool on);
bool netWifiEnabled();
// "off", "connected", "setup-ap", "connecting" or "no-credentials".
const char *netWifiState();
// Opens the setup network now and keeps it until reboot (STA is not retried); saved credentials
// stay, e.g. to take the board somewhere without WiFi. Switches the radio on if it was off.
void netOpenAp();
bool netRestartRequested();  // the sketch reboots at the end of loop(), after the reply went out
void netRequestRestart();
String netStatusLine();

// Clock. Valid once NTP synced or a browser sent its time (POST /api/time); an NTP-synced clock
// only takes a browser time with force.
uint32_t netEpoch();  // 0 until valid
bool netTimeValid();
const char *netTimeSource();  // "ntp", "browser" or "none"
bool netSetClock(double epoch, bool force);
bool netIsNight();     // 22:00-07:00 local time (false until the clock is valid)
String netLocalTime();  // "YYYY-MM-DD HH:MM:SS", "" until the clock is valid

// Discovery of other boards that advertise MDNS_SERVICE over mDNS (never this one).
void netAdvertise(const char *key, const String &value);  // TXT record on MDNS_SERVICE
bool netDiscoverStart();  // async query; false without WiFi or with one in flight
bool netDiscovering();
// -1 while the query runs; then, once, the number of peers written (up to `max`).
int netDiscoverResults(IPAddress *ips, uint16_t *ports, int max);
// IP literal, "name.local" or a bare mDNS name; 0.0.0.0 when it cannot be resolved.
IPAddress netResolve(const char *host);
