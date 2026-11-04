#include "net.h"
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include <mdns.h>
#include <sys/time.h>
#include <time.h>
#include <memory>
#include <new>
#include "devsettings.h"
#include "hw.h"
#include "lcd.h"
#include "led.h"
#include "sound.h"
#include "touch.h"
#include "web_assets.h"

#ifndef NTP_SERVER
#define NTP_SERVER "pool.ntp.org"
#endif
#ifndef NTP_SERVER2
#define NTP_SERVER2 "time.google.com"
#endif
#ifndef SETUP_ACCENT
#define SETUP_ACCENT "#4d96ff"
#endif
#ifndef NIGHT_START_HOUR
#define NIGHT_START_HOUR 22
#endif
#ifndef NIGHT_END_HOUR
#define NIGHT_END_HOUR 7
#endif
#ifndef NET_STA_TIMEOUT_MS
#define NET_STA_TIMEOUT_MS 30000  // without a connection by then the setup AP opens (STA keeps trying)
#endif
#ifndef NET_STA_RETRY_MS
#define NET_STA_RETRY_MS 15000  // STA retry period while the setup AP is up
#endif

static WebServer server(80);
static DNSServer dnsServer;
static NetHooks hooks = {};
static bool apMode = false;
static bool connected = false;
static bool staFailed = false;  // the saved network could not be joined since boot
static bool mdnsStarted = false;
static bool restartRequested = false;
static bool staPaused = false;  // STA retries hop channels and drop a phone on the portal: paused while one is there
static bool apHeld = false;     // netOpenAp(): AP kept, STA not retried until the next boot
static bool wifiOn = true;      // netSetWifiEnabled(); false = WIFI_OFF, nothing below runs
static bool serverStarted = false;
static bool timeNotified = false;
static uint32_t staStartedAt = 0;
static uint32_t lastRetry = 0;
enum TimeSource : uint8_t { TIME_NONE, TIME_BROWSER, TIME_NTP };
static volatile uint8_t timeSource = TIME_NONE;  // set from the SNTP callback too
static mdns_search_once_t *discoverSearch = nullptr;

static const char *const ORIENTATIONS[] = {"portrait", "landscape", "portrait-flipped", "landscape-flipped"};

// ------------------------------------------------------------------ helpers

WebServer &netServer() { return server; }

void netSendJson(JsonDocument &doc, int code) {
  String out;
  serializeJson(doc, out);
  server.sendHeader("Cache-Control", "no-store");
  server.send(code, "application/json", out);
}

void netSendError(int code, const String &msg) {
  JsonDocument doc;
  doc["ok"] = false;
  doc["error"] = msg;
  netSendJson(doc, code);
}

void netSendOk() {
  JsonDocument doc;
  doc["ok"] = true;
  netSendJson(doc);
}

bool netReadBody(JsonDocument &doc) {
  if (!server.hasArg("plain")) {
    netSendError(400, "expected a JSON body (Content-Type: application/json)");
    return false;
  }
  DeserializationError e = deserializeJson(doc, server.arg("plain"));
  if (e) {
    netSendError(e == DeserializationError::NoMemory ? 413 : 400, String("invalid JSON: ") + e.c_str());
    return false;
  }
  return true;
}

static String htmlEscape(const String &s) {
  String o;
  o.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '<') o += "&lt;";
    else if (c == '>') o += "&gt;";
    else if (c == '&') o += "&amp;";
    else if (c == '"') o += "&quot;";
    else if (c == '\'') o += "&#39;";
    else o += c;
  }
  return o;
}

bool netParseColor(const char *s, uint32_t &out) {
  if (!s) return false;
  if (*s == '#') s++;
  size_t len = strlen(s);
  if (len != 6 && len != 3) return false;
  uint32_t v = 0;
  for (size_t i = 0; i < len; i++) {
    char c = s[i];
    int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
    if (d < 0) return false;
    v = len == 6 ? (v << 4) | d : (v << 8) | (d * 17);
  }
  out = v;
  return true;
}

void netFormatColor(uint32_t c, char *out) { snprintf(out, 8, "#%06lx", (unsigned long)(c & 0xFFFFFF)); }

static void emit(NetEvent e, const String &arg) {
  if (hooks.event) hooks.event(e, arg.c_str());
}

// ------------------------------------------------------------------ state

bool netConnected() { return connected; }
bool netApMode() { return apMode; }
bool netSetupMode() { return apMode && !connected; }
bool netStaFailed() { return staFailed; }
bool netRestartRequested() { return restartRequested; }
void netRequestRestart() { restartRequested = true; }
String netIp() { return connected ? WiFi.localIP().toString() : (apMode ? WiFi.softAPIP().toString() : String("")); }
String netSsid() { return connected ? WiFi.SSID() : String(""); }
int netRssi() { return connected ? WiFi.RSSI() : 0; }

String netApSsid() {
  uint64_t mac = ESP.getEfuseMac();  // eFuse MAC: the interface MACs differ between AP and STA
  char buf[32];
  snprintf(buf, sizeof(buf), "%s%02X%02X", AP_SSID_PREFIX, (uint8_t)(mac >> 32), (uint8_t)(mac >> 40));
  return String(buf);
}

const char *netWifiState() {
  if (!wifiOn) return "off";
  return connected ? "connected" : (apMode ? "setup-ap" : (devSsid().length() ? "connecting" : "no-credentials"));
}

bool netWifiEnabled() { return wifiOn; }

String netStatusLine() {
  String s = "wifi: ";
  s += netWifiState();
  s += " ssid=\"" + devSsid() + "\" ip=" + netIp() + " rssi=" + String(netRssi());
  s += " host=" + devHostname() + ".local";
  if (apMode) s += " ap=" + netApSsid() + " clients=" + String(WiFi.softAPgetStationNum());
  if (staFailed) s += " join-failed";
  return s;
}

// ------------------------------------------------------------------ clock

uint32_t netEpoch() {
  time_t now = time(nullptr);
  return now > 1700000000 ? (uint32_t)now : 0;
}

bool netTimeValid() { return netEpoch() != 0; }

const char *netTimeSource() {
  if (timeSource == TIME_NTP) return "ntp";
  return netEpoch() ? "browser" : "none";
}

bool netSetClock(double epoch, bool force) {
  if (epoch < 1700000000.0 || epoch > 4200000000.0) return false;
  if (timeSource == TIME_NTP && !force) return false;  // NTP owns the clock once it has synced
  struct timeval tv;
  tv.tv_sec = (time_t)epoch;
  tv.tv_usec = (suseconds_t)((epoch - floor(epoch)) * 1e6);
  settimeofday(&tv, nullptr);
  if (timeSource != TIME_NTP) timeSource = TIME_BROWSER;
  Serial.printf("[net] clock set from the network: %lu\n", (unsigned long)tv.tv_sec);
  return true;
}

bool netIsNight() {
  if (!netEpoch()) return false;
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  return t.tm_hour >= NIGHT_START_HOUR || t.tm_hour < NIGHT_END_HOUR;
}

String netLocalTime() {
  if (!netEpoch()) return "";
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  char buf[24];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &t);
  return buf;
}

// ------------------------------------------------------------------ WiFi

static void startMdns() {
  if (mdnsStarted || !MDNS.begin(devHostname().c_str())) return;
  mdnsStarted = true;
  MDNS.addService("http", "tcp", 80);
  MDNS.addService(MDNS_SERVICE, "tcp", 80);  // other boards running this app find us through it
  MDNS.addServiceTxt(MDNS_SERVICE, "tcp", "board", BOARD_ID);
  MDNS.addServiceTxt(MDNS_SERVICE, "tcp", "fw", FW_VERSION);
}

static void startAp() {
  if (apMode) return;
  apMode = true;
  String ssid = netApSsid();
  WiFi.mode(WIFI_AP_STA);  // STA stays on so the setup page can scan networks
  WiFi.softAP(ssid.c_str());  // open network, no password
  // Core 3.x only offers a DNS server over DHCP when one is configured. Without it phones never
  // resolve their connectivity-check host, so they send no request and the portal never opens.
  IPAddress apIp(192, 168, 4, 1);
  WiFi.softAPConfig(apIp, apIp, IPAddress(255, 255, 255, 0), IPAddress(0, 0, 0, 0), apIp);
  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer.start(53, "*", WiFi.softAPIP());  // every name resolves to the board
  startMdns();  // <host>.local answers on the setup network as well
  WiFi.scanNetworks(true);  // async: the setup page has a list when it opens
  Serial.printf("[net] setup AP %s (open) at http://%s\n", ssid.c_str(), WiFi.softAPIP().toString().c_str());
  emit(NET_AP_STARTED, ssid);
}

static void stopAp() {
  if (!apMode) return;
  dnsServer.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  apMode = false;
  if (staPaused) {
    staPaused = false;
    WiFi.setAutoReconnect(true);
  }
  Serial.println("[net] setup AP closed");
  emit(NET_AP_CLOSED, netApSsid());
}

static void beginServer() {
  if (serverStarted) return;
  server.begin();
  serverStarted = true;
}

// STA with the saved credentials, or the setup AP when there are none.
static void startRadio() {
  if (devSsid().isEmpty()) {
    startAp();
  } else {
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(devHostname().c_str());
    WiFi.begin(devSsid().c_str(), devPass().c_str());
    staStartedAt = millis();
    Serial.printf("[net] joining \"%s\"\n", devSsid().c_str());
  }
  beginServer();
}

static void stopRadio() {
  if (discoverSearch) {
    mdns_query_async_delete(discoverSearch);
    discoverSearch = nullptr;
  }
  if (mdnsStarted) {
    MDNS.end();
    mdnsStarted = false;
  }
  if (serverStarted) {
    server.stop();
    serverStarted = false;
  }
  if (apMode) {
    dnsServer.stop();
    apMode = false;
    emit(NET_AP_CLOSED, netApSsid());
  }
  bool was = connected;
  connected = false;
  staFailed = staPaused = apHeld = false;
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  if (was) emit(NET_DISCONNECTED, devSsid());
}

void netSetWifiEnabled(bool on) {
  if (on == wifiOn) return;
  wifiOn = on;
  devSetWifiOn(on);
  if (on) {
    Serial.println("[net] WiFi on");
    emit(NET_WIFI_ON, "");
    startRadio();
  } else {
    stopRadio();
    Serial.println("[net] WiFi off (radio off until switched back on)");
    emit(NET_WIFI_OFF, "");
  }
}

void netOpenAp() {
  if (!wifiOn) {  // asked for the setup network on purpose: that needs the radio
    wifiOn = true;
    devSetWifiOn(true);
    emit(NET_WIFI_ON, "");
    beginServer();
  }
  apHeld = true;
  if (apMode) return;
  connected = false;
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(false, false);
  startAp();
}

static void onConnected() {
  connected = true;
  staFailed = false;
  stopAp();  // the saved network is back: the setup network has done its job
  String ip = WiFi.localIP().toString();
  Serial.printf("[net] connected to \"%s\", IP %s, %d dBm\n", devSsid().c_str(), ip.c_str(), WiFi.RSSI());
  startMdns();
  static bool ntpStarted = false;
  if (!ntpStarted) {
    sntp_set_time_sync_notification_cb([](struct timeval *) { timeSource = TIME_NTP; });
    configTzTime(devTz().c_str(), NTP_SERVER, NTP_SERVER2);
    ntpStarted = true;
  }
  emit(NET_CONNECTED, ip);
}

// ------------------------------------------------------------------ discovery

void netAdvertise(const char *key, const String &value) {
  if (mdnsStarted) MDNS.addServiceTxt(MDNS_SERVICE, "tcp", key, value.c_str());
}

bool netDiscoverStart() {
  if (!connected || apMode || !mdnsStarted || discoverSearch) return false;
  discoverSearch = mdns_query_async_new(nullptr, "_" MDNS_SERVICE, "_tcp", MDNS_TYPE_PTR, 1500, 8, nullptr);
  return discoverSearch != nullptr;
}

bool netDiscovering() { return discoverSearch != nullptr; }

int netDiscoverResults(IPAddress *ips, uint16_t *ports, int max) {
  if (!discoverSearch) return 0;
  mdns_result_t *results = nullptr;
  uint8_t n = 0;
  if (!mdns_query_async_get_results(discoverSearch, 0, &results, &n)) return -1;  // still listening
  mdns_query_async_delete(discoverSearch);
  discoverSearch = nullptr;
  int count = 0;
  IPAddress me = WiFi.localIP();
  for (mdns_result_t *r = results; r && count < max; r = r->next) {
    for (mdns_ip_addr_t *a = r->addr; a; a = a->next) {
      if (a->addr.type != ESP_IPADDR_TYPE_V4) continue;
      IPAddress ip(a->addr.u_addr.ip4.addr);
      if (ip == me) break;  // never ourselves
      ips[count] = ip;
      ports[count] = r->port ? r->port : 80;
      count++;
      break;
    }
  }
  mdns_query_results_free(results);
  return count;
}

IPAddress netResolve(const char *host) {
  IPAddress ip;
  if (!host || !*host || strlen(host) > 64) return IPAddress((uint32_t)0);
  if (ip.fromString(host)) return ip;
  // "name.local" or a bare mDNS name: the DNS resolver does not handle .local, mDNS does
  char name[65];
  strlcpy(name, host, sizeof(name));
  size_t n = strlen(name);
  if (n > 6 && !strcasecmp(name + n - 6, ".local")) name[n - 6] = 0;
  if (!mdnsStarted) return IPAddress((uint32_t)0);
  return MDNS.queryHost(name, 500);
}

// ------------------------------------------------------------------ setup portal

// One line per portal page request, to diagnose phones that do not open the sign-in sheet.
static void portalLog(const char *what) {
  if (!netSetupMode()) return;
  Serial.printf("[portal] %s %s%s -> %s\n", server.client().remoteIP().toString().c_str(), server.hostHeader().c_str(),
                server.uri().c_str(), what);
}

static void captiveRedirect() {
  server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/", true);
  server.send(302, "text/plain", "");
}

// OS connectivity checks: in setup mode every one of them lands on the web panel, so the phone
// opens its sign-in sheet; otherwise 204 so the OS shows nothing.
static void handleProbe() {
  if (!netSetupMode()) {
    server.send(204);
    return;
  }
  portalLog("redirect");
  captiveRedirect();
}

// ------------------------------------------------------------------ setup pages
// The small setup page (GET /setup) and the result of its form (POST /api/wifi). They have to
// work in the most limited captive sheet (iOS): no alert/confirm/prompt, nothing external, a
// little inline JS that the page does not depend on, light and dark schemes, 44 px targets.
// Names and links come from config.h: FW_NAME, FW_VERSION, PROJECT_URL (repo) and
// AUTHOR_URL, SETUP_ACCENT (a light accent: buttons use dark text on it) and DONATE_URL.

static String setupHead(const __FlashStringHelper *title) {
  String h;
  h.reserve(4800);
  h += F("<!doctype html><html lang=en><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
         "<meta name=color-scheme content='dark light'><title>");
  h += title;
  h += F("</title><style>"
         ":root{--bg:#0b0d12;--card:#161a22;--line:#2c3240;--fg:#ececf2;--dim:#a5aaba;--a:" SETUP_ACCENT ";--link:#7cc4ff;--warn:#3a2410}"
         "@media(prefers-color-scheme:light){:root{--bg:#f6f6fa;--card:#fff;--line:#cfd2dc;--fg:#15161c;--dim:#545a69;--link:#0b5cb8;--warn:#ffefd6}}"
         "html{-webkit-text-size-adjust:100%}*{box-sizing:border-box}[hidden]{display:none!important}"
         "body{margin:auto;max-width:440px;padding:20px 16px 24px;font:16px/1.45 system-ui,-apple-system,sans-serif;"
         "background:var(--bg);color:var(--fg);overflow-wrap:anywhere}"
         "h1{font-size:22px;margin:0 0 6px}ol{margin:8px 0 4px;padding-left:22px}li{margin:4px 0}"
         "label{display:block;font-weight:600;margin:16px 0 6px}"
         "select,input,button{display:block;width:100%;max-width:100%;min-height:44px;padding:10px 12px;font:inherit;color:inherit;"
         "background:var(--card);border:1px solid var(--line);border-radius:10px}"
         "button{margin-top:20px;background:var(--a);border:0;color:#111;font-weight:700}button:disabled{opacity:.65}"
         ":focus-visible{outline:3px solid var(--a);outline-offset:2px}"
         "a{color:var(--link)}.dim{color:var(--dim);font-size:14px}.warn{background:var(--warn);padding:10px 12px;border-radius:10px}"
         ".head{display:flex;justify-content:space-between;align-items:baseline;gap:8px}"
         ".head a,.links a{display:inline-block;padding:10px 4px;min-height:44px}"
         ".chk{display:flex;align-items:center;gap:10px;font-weight:400;margin:6px 0 0;min-height:44px}"
         ".chk input{width:22px;min-height:22px;margin:0}.links{text-align:center;margin:14px 0 0}"
         "footer{margin-top:22px;text-align:center;font-size:13px;color:var(--dim)}footer a{color:inherit}"
         "</style></head><body>");
  return h;
}

// "<App> v<version> · <site> · GitHub", on every setup page.
static void setupFooter(String &h) {  // the site keeps its https:// so it reads as a web page
  h += F("<footer>" FW_NAME " v" FW_VERSION " &middot; <a href='" AUTHOR_URL "' target=_blank rel=noopener>" AUTHOR_URL);
  h += F("</a> &middot; <a href='" PROJECT_URL "' target=_blank rel=noopener>GitHub</a></footer></body></html>");
}

// Signal as bars plus a lock for secured networks, e.g. "Home ▂▄▆ 🔒".
static String signalLabel(int rssi, bool secure) {
  int bars = rssi >= -55 ? 4 : rssi >= -67 ? 3 : rssi >= -77 ? 2 : 1;
  static const char *const BARS[] = {"\u2582", "\u2582\u2584", "\u2582\u2584\u2586", "\u2582\u2584\u2586\u2588"};
  return String(" \u00a0") + BARS[bars - 1] + (secure ? " \U0001F512" : "");
}

// Small page without external JS for captive sheets that choke on the full panel.
static void handleSetupPage() {
  portalLog("setup");
  String h = setupHead(F(FW_NAME " setup"));
  h += F("<h1>" FW_NAME "</h1><p class=dim>Connect it to your WiFi:</p><ol><li>Pick your network.</li><li>Type its password.</li>"
         "<li>Tap <b>Connect</b>. It reboots, joins that network and answers at <b>");
  h += htmlEscape(devHostname());
  h += F(".local</b> from any device on it.</li></ol>");
  if (staFailed && devSsid().length())
    h += "<p class=warn role=alert>Could not join <b>" + htmlEscape(devSsid()) + "</b>. Check the password or choose another network.</p>";
  h += F("<form method=post action=/api/wifi id=f><div class=head><label for=s>Network</label><a href=/setup>Rescan</a></div>"
         "<select name=ssid id=s>");
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) h += F("<option disabled>Scanning...</option>");
  for (int i = 0, kept = 0; i < n && kept < 20; i++) {  // results come strongest first
    String ssid = WiFi.SSID(i);
    if (!ssid.length()) continue;
    bool dup = false;  // one entry per SSID
    for (int j = 0; j < i && !dup; j++) dup = WiFi.SSID(j) == ssid;
    if (dup) continue;
    kept++;
    String e = htmlEscape(ssid);
    h += "<option value=\"" + e + "\"" + (ssid == devSsid() ? " selected" : "") + ">" + e +
         signalLabel(WiFi.RSSI(i), WiFi.encryptionType(i) != WIFI_AUTH_OPEN) + "</option>";
  }
  h += F("<option value=''>Other network...</option></select>"
         "<label for=o id=ol>Network name (SSID)</label><input name=network id=o maxlength=32 autocomplete=off autocapitalize=off "
         "autocorrect=off spellcheck=false enterkeyhint=next>"
         "<label for=p>Password</label><input name=pass id=p type=password maxlength=64 autocomplete=current-password "
         "autocapitalize=off autocorrect=off spellcheck=false enterkeyhint=go aria-describedby=ph>"
         "<label class=chk><input type=checkbox id=v> Show password</label><p class=dim id=ph>Leave it empty for an open network.</p>"
         "<button id=b>Connect</button></form>"
         "<script>var s=document.getElementById('s'),o=document.getElementById('o'),l=document.getElementById('ol'),"
         "p=document.getElementById('p'),b=document.getElementById('b');"
         "function t(){o.hidden=l.hidden=s.value!=''}s.onchange=t;t();"
         "document.getElementById('v').onchange=function(){p.type=this.checked?'text':'password'};"
         "document.getElementById('f').onsubmit=function(){if(b.disabled)return false;b.disabled=true;b.textContent='Saving...'}</script>"
         "<p class=links><a href=/>All controls</a></p>");
#ifdef DONATE_URL
  h += F("<p class=dim style='text-align:center;font-size:12px;margin-top:20px'>Found " FW_NAME " useful? <a href='");
  h += htmlEscape(F(DONATE_URL));
  h += F("' target='_blank' rel='noopener' style='color:inherit'>Buy me a transistor</a> (optional, via Orbi Pay)</p>");
#endif
  setupFooter(h);
  if (n != WIFI_SCAN_RUNNING) {  // fresh list for the next visit
    WiFi.scanDelete();
    WiFi.scanNetworks(true);
  }
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html", h);
}

// What the setup form gets back: what happens next, or what was wrong.
static void sendSetupResult(bool ok, const String &ssid, const String &url) {
  String h = setupHead(ok ? F(FW_NAME ": saved") : F(FW_NAME ": not saved"));
  if (ok) {
    h += F("<h1>Saved, rebooting</h1><p>" FW_NAME " is joining <b>");
    h += htmlEscape(ssid);
    h += F("</b>.</p><ol><li>Wait about 15 seconds.</li><li>Connect this phone to <b>");
    h += htmlEscape(ssid);
    h += F("</b> again.</li><li>Open <a href='");
    h += url;
    h += F("'>");
    h += url.substring(7);
    h += F("</a>, or the IP shown on the board's info screen.</li></ol><p class=dim>If it cannot join, it opens its own network <b>");
    h += htmlEscape(netApSsid());
    h += F("</b> again so you can try once more.</p>");
  } else {
    h += F("<h1>Not saved</h1><p class=warn role=alert>The network name must be 1 to 32 characters and the password up to 64.</p>"
           "<p class=links><a href=/setup>Back to the setup</a></p>");
  }
  setupFooter(h);
  server.sendHeader("Cache-Control", "no-store");
  server.send(ok ? 200 : 400, "text/html", h);
}

static void handleRoot() {
  portalLog("panel");
  server.sendHeader("Content-Encoding", "gzip");
  server.sendHeader("Cache-Control", "no-cache");
  server.send_P(200, "text/html", (const char *)WEB_INDEX_GZ, WEB_INDEX_GZ_LEN);
}

// ------------------------------------------------------------------ common API

// GET /api/wifi: status plus the networks of the last scan; ?scan=1 starts a new one.
static void handleWifiGet() {
  JsonDocument doc;
  doc["ok"] = true;
  doc["connected"] = connected;
  doc["ssid"] = devSsid();  // the saved network
  doc["apMode"] = apMode;
  doc["setup"] = netSetupMode();
  doc["staFailed"] = staFailed;
  doc["apSsid"] = apMode ? netApSsid() : String("");
  doc["apIp"] = apMode ? WiFi.softAPIP().toString() : String("");
  doc["apClients"] = apMode ? WiFi.softAPgetStationNum() : 0;
  int n = WiFi.scanComplete();
  doc["scanning"] = n == WIFI_SCAN_RUNNING;
  JsonArray nets = doc["networks"].to<JsonArray>();
  for (int i = 0; i < n && nets.size() < 25; i++) {
    String s = WiFi.SSID(i);
    if (s.isEmpty()) continue;
    bool dup = false;
    for (JsonObject o : nets) dup |= o["ssid"] == s;
    if (dup) continue;  // results are sorted by signal: keep the strongest
    JsonObject o = nets.add<JsonObject>();
    o["ssid"] = s;
    o["rssi"] = WiFi.RSSI(i);
    o["secure"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
  }
  if (n != WIFI_SCAN_RUNNING && (server.hasArg("scan") || n <= 0)) {
    WiFi.scanDelete();
    WiFi.scanNetworks(true);
    doc["scanning"] = true;
  }
  netSendJson(doc);
}

// POST /api/wifi: JSON {ssid, pass} (web panel, scripts) or the setup page form (ssid|network, pass).
static void handleWifiPost() {
  String s, p;
  bool form = server.hasArg("ssid") || server.hasArg("network");
  if (form) {
    s = server.arg("ssid");
    if (s.isEmpty()) s = server.arg("network");
    p = server.arg("pass");
  } else {
    JsonDocument doc;
    if (!netReadBody(doc)) return;
    s = doc["ssid"] | "";
    p = doc["pass"] | "";
  }
  if (!devSaveWifi(s, p)) {
    if (form) sendSetupResult(false, s, "");
    else netSendError(400, "ssid must be 1-32 chars, pass up to 64");
    return;
  }
  restartRequested = true;
  String url = "http://" + devHostname() + ".local";
  if (!form) {
    JsonDocument doc;
    doc["ok"] = true;
    doc["rebooting"] = true;
    doc["url"] = url;
    netSendJson(doc);
    return;
  }
  sendSetupResult(true, s, url);
}

static void handleInfo() {
  JsonDocument doc;
  doc["ok"] = true;
  doc["name"] = FW_NAME;
  doc["firmware"] = FW_VERSION;
  doc["projectUrl"] = PROJECT_URL;  // the repo
  doc["authorUrl"] = AUTHOR_URL;
  doc["wifi"] = netWifiState();  // "off" is never seen over HTTP, but the same field feeds the CLI
  doc["board"] = BOARD_NAME;
  doc["boardId"] = BOARD_ID;
  doc["chip"] = ESP.getChipModel();
  doc["hostname"] = devHostname();
  doc["mdns"] = devHostname() + ".local";
  doc["ip"] = netIp();
  doc["ssid"] = netSsid();
  doc["rssi"] = netRssi();
  doc["apMode"] = apMode;
  doc["setup"] = netSetupMode();
  doc["apSsid"] = apMode ? netApSsid() : String("");
  doc["apIp"] = apMode ? WiFi.softAPIP().toString() : String("");
  doc["apClients"] = apMode ? WiFi.softAPgetStationNum() : 0;
  doc["tz"] = devTz();
  doc["localTime"] = netLocalTime();
  doc["epoch"] = netEpoch();
  doc["timeValid"] = netTimeValid();
  doc["timeSource"] = netTimeSource();
  doc["night"] = netIsNight();
  doc["nightDim"] = devNightDim();
  doc["backlight"] = devBacklight();
  doc["backlightDuty"] = lcdBacklightDuty();
  doc["orientation"] = ORIENTATIONS[devRotation() & 3];
  doc["rotation"] = devRotation();
  doc["uptimeSec"] = millis() / 1000;
  doc["freeHeap"] = ESP.getFreeHeap();
  doc["freePsram"] = ESP.getFreePsram();
  doc["touch"] = touchPresent();
  doc["imu"] = (bool)HAS_IMU;
  if (hwHasBattery()) {
    doc["batteryMv"] = hwBatteryMv();
    doc["batteryPct"] = hwBatteryPct();
    doc["lowBattery"] = hwBatteryLow();          // <= BATTERY_LOW_PCT, clears at BATTERY_LOW_CLEAR_PCT
    doc["charging"] = hwBatteryCharging();       // a guess from the voltage trend (no VBUS sense)
  }
  if (hooks.info) hooks.info(doc.as<JsonObject>());
  netSendJson(doc);
}

// POST /api/settings {hostname, tz, backlight, orientation | rotation, nightDim} plus the app's
// fields. Everything is validated before anything changes; hostname and rotation reboot.
static void handleSettings() {
  JsonDocument doc;
  if (!netReadBody(doc)) return;
  if (!doc.is<JsonObject>()) {
    netSendError(400, "expected a JSON object");
    return;
  }
  String host = devHostname();
  if (!doc["hostname"].isNull()) {
    host = doc["hostname"] | "";
    host.trim();
    host.toLowerCase();
    if (!devValidHostname(host)) {
      netSendError(400, "hostname must be 1-24 chars of a-z, 0-9 and -");
      return;
    }
  }
  if (!doc["tz"].isNull() && !devValidTz(doc["tz"] | "")) {
    netSendError(400, "tz must be a POSIX TZ string such as CST6 or CET-1CEST,M3.5.0,M10.5.0/3");
    return;
  }
  if (!doc["backlight"].isNull() && !doc["backlight"].is<int>()) {
    netSendError(400, "backlight must be a number, 5-255");
    return;
  }
  if (!doc["nightDim"].isNull() && !doc["nightDim"].is<bool>()) {
    netSendError(400, "nightDim must be true or false");
    return;
  }
  int rot = -1;
  if (!doc["orientation"].isNull()) {
    const char *o = doc["orientation"] | "";
    for (int i = 0; i < 4; i++)
      if (!strcmp(o, ORIENTATIONS[i])) rot = i;
    if (rot < 0) {
      netSendError(400, "orientation must be portrait, landscape, portrait-flipped or landscape-flipped");
      return;
    }
  }
  if (!doc["rotation"].isNull()) {
    rot = doc["rotation"].is<int>() ? doc["rotation"].as<int>() : -1;
    if (rot < 0 || rot > 3) {
      netSendError(400, "rotation must be 0-3");
      return;
    }
  }
  JsonDocument out;
  out["ok"] = true;
  String err;
  if (hooks.settings && !hooks.settings(doc.as<JsonObjectConst>(), out.as<JsonObject>(), err)) {
    netSendError(400, err);
    return;
  }

  bool reboot = false;
  if (host != devHostname()) {
    devSetHostname(host);
    reboot = true;  // the WiFi hostname and mDNS are set at connect time
  }
  if (!doc["tz"].isNull()) devSetTz(doc["tz"] | "");
  if (!doc["backlight"].isNull()) devSetBacklight(doc["backlight"].as<int>());
  if (!doc["nightDim"].isNull()) devSetNightDim(doc["nightDim"].as<bool>());
  if (!doc["backlight"].isNull() || !doc["nightDim"].isNull()) lcdSetBacklight(devBacklightLevel(netIsNight()));
  if (rot >= 0 && rot != devRotation()) {
    devSetRotation(rot);
    reboot = true;  // the canvas is allocated for one rotation at boot
  }
  if (reboot) restartRequested = true;
  out["rebooting"] = reboot;
  out["hostname"] = devHostname();
  out["tz"] = devTz();
  out["backlight"] = devBacklight();
  out["orientation"] = ORIENTATIONS[devRotation() & 3];
  out["rotation"] = devRotation();
  out["nightDim"] = devNightDim();
  netSendJson(out);
}

// POST /api/time {epoch, force}: sets the clock from the browser while there is no NTP (setup
// network, a network without internet). An NTP-synced clock is kept unless force is true.
static void handleTime() {
  JsonDocument doc;
  if (!netReadBody(doc)) return;
  if (!doc["epoch"].is<double>()) {
    netSendError(400, "epoch (unix seconds) required");
    return;
  }
  double epoch = doc["epoch"].as<double>();
  if (epoch < 1700000000.0 || epoch > 4200000000.0) {
    netSendError(400, "epoch out of range");
    return;
  }
  JsonDocument out;
  out["ok"] = true;
  out["applied"] = netSetClock(epoch, doc["force"] | false);
  out["epoch"] = netEpoch();
  out["timeSource"] = netTimeSource();
  netSendJson(out);
}

static void addPins(JsonDocument &doc, bool led) {
  JsonArray arr = doc["pins"].to<JsonArray>();
  int n = led ? ledPinCount() : soundPinCount();
  for (int i = 0; i < n; i++) arr.add(led ? ledPinAt(i) : soundPinAt(i));
}

static void ledState(JsonDocument &doc) {
  const LedConfig &lc = ledConfig();
  char hex[8];
  doc["ok"] = true;
  doc["builtin"] = ledBuiltin();
  doc["pin"] = lc.pin;
  addPins(doc, true);
  doc["mode"] = ledModeName(lc.mode);
  netFormatColor(lc.color, hex);
  doc["color"] = hex;
  doc["brightness"] = lc.brightness;
  netFormatColor(ledCurrent(), hex);
  doc["current"] = hex;
}

static void handleLedGet() {
  JsonDocument out;
  ledState(out);
  netSendJson(out);
}

// POST /api/led {mode: off | <app mode> | color | status, color: "#rrggbb", brightness: 0-255,
// pin: gpio}; all optional and persisted. A color without a mode switches to "color".
static void handleLedPost() {
  JsonDocument doc;
  if (!netReadBody(doc)) return;
  LedConfig lc = ledConfig();
  if (!doc["mode"].isNull() && !ledModeFromName(doc["mode"] | "", lc.mode)) {
    netSendError(400, String("mode must be off, ") + ledModeName(LED_APP) + ", color or status");
    return;
  }
  if (!doc["color"].isNull()) {
    if (!netParseColor(doc["color"] | "", lc.color)) {
      netSendError(400, "bad color, use #rrggbb");
      return;
    }
    if (doc["mode"].isNull()) lc.mode = LED_COLOR;
  }
  if (!doc["pin"].isNull()) {
    int pin = doc["pin"] | -1;
    if (!ledPinAllowed(pin)) {
      netSendError(400, "pin not allowed for the LED on this board, see GET /api/led \"pins\"");
      return;
    }
    lc.pin = pin;
  }
  if (!doc["brightness"].isNull()) lc.brightness = constrain(doc["brightness"].as<int>(), 0, 255);
  String err;
  if (!ledConfigure(lc, err)) {
    netSendError(400, err);
    return;
  }
  devSaveOutputs();
  JsonDocument out;
  ledState(out);
  netSendJson(out);
}

static void soundState(JsonDocument &doc) {
  doc["ok"] = true;
  doc["buzzer"] = soundHasBuzzer();
  doc["builtin"] = soundBuiltin();
  doc["pin"] = soundConfig().pin;
  addPins(doc, false);
  doc["playing"] = soundPlaying();
  doc["remainingMs"] = soundRemainingMs();
  doc["volume"] = soundConfig().volume;
  doc["effects"] = soundConfig().effects;
}

static void handleSoundGet() {
  JsonDocument doc;
  soundState(doc);
  JsonArray arr = doc["sounds"].to<JsonArray>();
  for (int i = 0; i < SFX_COUNT; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["name"] = SFX[i].name;
    o["tempo"] = SFX[i].tempo;
    o["notes"] = SFX[i].notes;
  }
  netSendJson(doc);
}

// POST /api/sound, one of
//   {"notes": "C5/8 E5/8 G5/4", "tempo": 120, "repeat": 1}
//   {"rtttl": "name:d=4,o=5,b=100:8e6,8d6"}
//   {"sfx": "coin"}    {"tone": 880, "ms": 200}    {"stop": true}
// plus optional {"volume": 0-100, "effects": bool, "buzzer": bool, "pin": gpio} (persisted; "buzzer"
// turns the output on or off, "pin" moves it to another allowed GPIO, e.g. an external buzzer).
// "dryRun": true parses and returns the [hz, ms, legato] sequence without playing (the web panel
// uses it to preview in the browser).
static void handleSoundPost() {
  JsonDocument doc;
  if (!netReadBody(doc)) return;
  bool changed = false;
  SoundConfig sc = soundConfig();
  if (!doc["volume"].isNull()) {
    sc.volume = constrain(doc["volume"].as<int>(), 0, 100);
    changed = true;
  }
  if (!doc["effects"].isNull()) {
    sc.effects = doc["effects"].as<bool>();
    changed = true;
  }
  if (!doc["pin"].isNull() || !doc["buzzer"].isNull()) {
    int pin = doc["pin"] | (int)sc.pin;
    if (!soundPinAllowed(pin)) {
      netSendError(400, "pin not allowed for the buzzer on this board, see GET /api/sound \"pins\"");
      return;
    }
    sc.pin = pin;
    sc.enabled = doc["buzzer"] | sc.enabled;
    changed = true;
  }
  if (changed) {
    String err;
    if (!soundConfigure(sc, err)) {
      netSendError(400, err);
      return;
    }
    devSaveOutputs();
  }

  bool dryRun = doc["dryRun"] | false;
  int repeat = constrain(doc["repeat"] | 1, 1, 20);
  String err;
  const char *notes = doc["notes"] | (const char *)nullptr;
  const char *rtttl = doc["rtttl"] | (const char *)nullptr;
  const char *sfx = doc["sfx"] | (const char *)nullptr;
  int tempo = doc["tempo"] | 120;
  if (sfx) {
    int i = soundSfxFind(sfx);
    if (i < 0) {
      netSendError(400, String("unknown sfx '") + sfx + "', see GET /api/sound");
      return;
    }
    notes = SFX[i].notes;
    tempo = SFX[i].tempo;
  }

  if (dryRun) {
    if (!rtttl && !notes) {
      netSendError(400, "dryRun needs \"notes\", \"rtttl\" or \"sfx\"");
      return;
    }
    std::unique_ptr<Tone[]> tones(new (std::nothrow) Tone[SOUND_MAX_TONES]);
    if (!tones) {
      netSendError(429, "out of memory, try again");
      return;
    }
    int n = rtttl ? soundParseRtttl(rtttl, tones.get(), SOUND_MAX_TONES, err)
                  : soundParseNotes(notes, tempo, tones.get(), SOUND_MAX_TONES, err);
    if (n < 0) {
      netSendError(400, err);
      return;
    }
    JsonDocument out;
    out["ok"] = true;
    out["count"] = n;
    out["durationMs"] = soundDurationMs(tones.get(), n) * repeat;
    JsonArray seq = out["sequence"].to<JsonArray>();
    for (int i = 0; i < n; i++) {
      JsonArray t = seq.add<JsonArray>();
      t.add(tones[i].hz);
      t.add(tones[i].ms);
      t.add(tones[i].legato ? 1 : 0);
    }
    netSendJson(out);
    return;
  }

  if (doc["stop"] | false) {
    soundStop();
  } else if (rtttl) {
    if (!soundPlayRtttl(rtttl, repeat, err)) {
      netSendError(400, err);
      return;
    }
  } else if (notes) {  // explicit requests (sfx included) play even with effect sounds off
    if (!soundPlayNotes(notes, tempo, repeat, err)) {
      netSendError(400, err);
      return;
    }
  } else if (!doc["tone"].isNull()) {
    int hz = doc["tone"].as<int>();
    if (hz < 20 || hz > 20000) {
      netSendError(400, "tone must be 20-20000 Hz");
      return;
    }
    soundTone(hz, constrain(doc["ms"] | 200, 1, 10000));
  } else if (!changed) {
    netSendError(400, "sound needs \"notes\", \"rtttl\", \"sfx\", \"tone\", \"stop\", \"volume\", \"effects\", \"buzzer\" or \"pin\"");
    return;
  }
  JsonDocument out;
  soundState(out);
  netSendJson(out);
}

static void setupRoutes() {
  server.enableCORS(true);
  if (hooks.routes) hooks.routes(server);
  server.on("/", HTTP_GET, handleRoot);
  server.on("/setup", HTTP_GET, handleSetupPage);
  server.on("/api/info", HTTP_GET, handleInfo);
  server.on("/api/settings", HTTP_POST, handleSettings);
  server.on("/api/wifi", HTTP_GET, handleWifiGet);
  server.on("/api/wifi", HTTP_POST, handleWifiPost);
  server.on("/api/time", HTTP_POST, handleTime);
  server.on("/api/led", HTTP_GET, handleLedGet);
  server.on("/api/led", HTTP_POST, handleLedPost);
  server.on("/api/sound", HTTP_GET, handleSoundGet);
  server.on("/api/sound", HTTP_POST, handleSoundPost);
  server.on("/api/reboot", HTTP_POST, []() {
    restartRequested = true;
    netSendOk();
  });

  const char *probes[] = {"/hotspot-detect.html", "/library/test/success.html",  // Apple
                          "/generate_204",        "/gen_204",                     // Android
                          "/connecttest.txt",     "/ncsi.txt", "/redirect", "/fwlink",  // Windows
                          "/success.txt",         "/canonical.html"};              // Firefox
  for (const char *p : probes) server.on(p, HTTP_ANY, handleProbe);

  server.onNotFound([]() {
    if (server.method() == HTTP_OPTIONS) {  // CORS preflight (PUT/DELETE and JSON bodies)
      server.sendHeader("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
      server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
      server.send(204);
      return;
    }
    if (netSetupMode() && !server.uri().startsWith("/api/")) {
      portalLog("redirect");
      captiveRedirect();
      return;
    }
    netSendError(404, "not found: " + server.uri());
  });
}

// ------------------------------------------------------------------ lifecycle

void netBegin(const NetHooks &h) {
  hooks = h;
  WiFi.persistent(false);
  WiFi.setSleep(false);  // modem sleep adds hundreds of ms to every HTTP request
  WiFi.onEvent([](arduino_event_id_t event, arduino_event_info_t info) {
    if (event == ARDUINO_EVENT_WIFI_AP_STACONNECTED) Serial.println("[portal] client joined");
    else if (event == ARDUINO_EVENT_WIFI_AP_STADISCONNECTED) Serial.println("[portal] client left");
    else if (event == ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED)
      Serial.printf("[portal] client got %s\n", IPAddress(info.wifi_ap_staipassigned.ip.addr).toString().c_str());
  });
  setupRoutes();
  wifiOn = devWifiOn();
  if (wifiOn) {
    startRadio();
  } else {
    WiFi.mode(WIFI_OFF);
    Serial.println("[net] WiFi off (saved setting): no network until it is switched back on");
  }
}

void netLoop() {
  if (!wifiOn) {
    if (!timeNotified && netEpoch()) {  // a clock set before the radio went off still counts
      timeNotified = true;
      if (hooks.timeValid) hooks.timeValid(netEpoch());
    }
    return;
  }
  if (apMode) dnsServer.processNextRequest();
  server.handleClient();
  if (!timeNotified && netEpoch()) {  // whatever set it: NTP, or a phone on the setup network
    timeNotified = true;
    if (hooks.timeValid) hooks.timeValid(netEpoch());
  }
  if (apHeld || devSsid().isEmpty()) return;  // AP on purpose or nothing to join: no STA bookkeeping

  uint32_t now = millis();
  bool up = WiFi.status() == WL_CONNECTED;
  if (up && !connected) {
    onConnected();
  } else if (!up && connected) {
    // Restart the timeout on a drop: a short outage after hours online must not open the AP at once.
    connected = false;
    staStartedAt = now;
    Serial.println("[net] connection lost, reconnecting");
    emit(NET_DISCONNECTED, devSsid());
  }
  if (connected) return;
  if (!apMode && (int32_t)(now - staStartedAt) > (int32_t)NET_STA_TIMEOUT_MS) {
    // Open the setup AP; STA keeps retrying underneath (AP_STA) and the AP closes in onConnected().
    Serial.println("[net] could not join, opening the setup AP (STA keeps retrying)");
    staFailed = true;
    startAp();
    lastRetry = now;
  }
  // A reconnect attempt hops the radio to the home network's channel and drops a phone that is
  // using the portal, so retries pause while anyone is connected to the setup network.
  bool portalInUse = apMode && WiFi.softAPgetStationNum() > 0;
  if (portalInUse && !staPaused) {
    staPaused = true;
    WiFi.setAutoReconnect(false);
    WiFi.disconnect(false, false);
    Serial.println("[net] portal in use, pausing reconnects");
  } else if (apMode && !portalInUse && staPaused) {
    staPaused = false;
    WiFi.setAutoReconnect(true);
    WiFi.begin(devSsid().c_str(), devPass().c_str());
    lastRetry = now;
  } else if (apMode && !staPaused && (int32_t)(now - lastRetry) > (int32_t)NET_STA_RETRY_MS) {
    lastRetry = now;
    WiFi.reconnect();
  }
}
