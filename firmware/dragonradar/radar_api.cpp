#include "radar_api.h"
// Builds only once radar.h (the radar itself) is in the sketch, so the sketch keeps compiling
// whatever order the two land in. Without it, calling radarApiRegister() fails at link time.
#if __has_include("radar.h")
#include <math.h>
#include "net.h"
#include "radar.h"
#include "radar_assets.h"
#include "sounds.h"

// Field names and ranges of docs/api.md. tools/check_consistency.mjs compares these lists with
// the contract, the web panel and the MCP, so keep them as plain string arrays.
static const char *const RADAR_KEYS[] = {"zoom", "theme", "sweep", "volume", "muted", "power", "resetHeading"};
// sweep.modes of shared/radar.json (check_consistency.mjs compares them).
static const char *const SWEEP_MODES[] = {"line", "ring", "both", "none"};
static const char *const BALLS_KEYS[] = {"balls", "demo"};
static const char *const BALL_KEYS[] = {"stars", "bearingDeg", "distanceM"};
static const char *const SFX_KEYS[] = {"name"};
static const char *const POWER_VALUES[] = {"on", "off"};
static constexpr int MAX_BALLS = 7;
static constexpr int BALL_STARS_MAX = 7;
static constexpr float BALL_BEARING_MAX_DEG = 360.0f;  // exclusive
static constexpr float BALL_DISTANCE_MIN_M = 0.1f;
static constexpr float BALL_DISTANCE_MAX_M = 100000.0f;
static constexpr int VOLUME_MAX = 100;

static void (*baseStateFn)(JsonObject) = nullptr;

template <size_t N>
static bool listHas(const char *const (&list)[N], const char *s) {
  for (size_t i = 0; i < N; i++)
    if (!strcmp(list[i], s)) return true;
  return false;
}

template <size_t N>
static String listJoin(const char *const (&list)[N]) {
  String s;
  for (size_t i = 0; i < N; i++) {
    if (i) s += ", ";
    s += list[i];
  }
  return s;
}

// "" when every key of `in` is in the list.
template <size_t N>
static String unknownKeys(JsonObjectConst in, const char *const (&list)[N], const char *what) {
  for (JsonPairConst kv : in)
    if (!listHas(list, kv.key().c_str()))
      return String("unknown field \"") + kv.key().c_str() + "\" in " + what + " (expected " + listJoin(list) + ")";
  return "";
}

static String themeNames() {
  String s;
  for (int i = 0; i < RADAR_THEME_COUNT; i++) {
    if (i) s += ", ";
    s += RADAR_THEMES[i].name;
  }
  return s;
}

static bool themeKnown(const char *name) {
  for (int i = 0; i < RADAR_THEME_COUNT; i++)
    if (!strcmp(RADAR_THEMES[i].name, name)) return true;
  return false;
}

static bool sfxKnown(const char *name) {
  for (int i = 0; i < SFX_COUNT; i++)
    if (!strcmp(SFX[i].name, name)) return true;
  return false;
}

static String sfxNames() {
  String s;
  for (int i = 0; i < SFX_COUNT; i++) {
    if (i) s += ", ";
    s += SFX[i].name;
  }
  return s;
}

// A JSON number (integer or not) in [lo, hi] (hi excluded when hiOpen).
static bool numberIn(JsonVariantConst v, float lo, float hi, bool hiOpen, float &out) {
  if (!v.is<float>() || v.is<bool>()) return false;
  out = v.as<float>();
  return isfinite(out) && out >= lo && (hiOpen ? out < hi : out <= hi);
}

// ------------------------------------------------------------------ validators

String radarApiCheckRadar(JsonObjectConst in) {
  String err = unknownKeys(in, RADAR_KEYS, "/api/radar");
  if (err.length()) return err;
  JsonVariantConst v;
  v = in["zoom"];
  if (!v.isNull() && (!v.is<int>() || v.as<int>() < 0 || v.as<int>() >= RADAR_ZOOM_COUNT))
    return String("zoom must be an integer 0-") + (RADAR_ZOOM_COUNT - 1);
  v = in["theme"];
  if (!v.isNull() && (!v.is<const char *>() || !themeKnown(v.as<const char *>())))
    return "theme must be one of " + themeNames();
  v = in["sweep"];
  if (!v.isNull() && (!v.is<const char *>() || !listHas(SWEEP_MODES, v.as<const char *>())))
    return "sweep must be one of " + listJoin(SWEEP_MODES);
  v = in["volume"];
  if (!v.isNull() && (!v.is<int>() || v.as<int>() < 0 || v.as<int>() > VOLUME_MAX))
    return String("volume must be an integer 0-") + VOLUME_MAX;
  v = in["muted"];
  if (!v.isNull() && !v.is<bool>()) return "muted must be true or false";
  v = in["power"];
  if (!v.isNull() && (!v.is<const char *>() || !listHas(POWER_VALUES, v.as<const char *>())))
    return "power must be \"on\" or \"off\"";
  v = in["resetHeading"];
  if (!v.isNull() && !v.is<bool>()) return "resetHeading must be true or false";
  return "";
}

String radarApiCheckBalls(JsonObjectConst in) {
  String err = unknownKeys(in, BALLS_KEYS, "/api/balls");
  if (err.length()) return err;
  JsonVariantConst balls = in["balls"], demo = in["demo"];
  if (!balls.isNull() && !demo.isNull()) return "send either balls or demo, not both";
  if (!demo.isNull()) return demo.is<bool>() && demo.as<bool>() ? "" : "demo must be true";
  if (balls.isNull()) return "expected {\"balls\": [...]} or {\"demo\": true}";
  if (!balls.is<JsonArrayConst>()) return "balls must be an array";
  JsonArrayConst arr = balls.as<JsonArrayConst>();
  if ((int)arr.size() > MAX_BALLS) return String("at most ") + MAX_BALLS + " balls, got " + arr.size();
  int i = 0;
  for (JsonVariantConst b : arr) {
    String at = String("balls[") + i++ + "]";
    if (!b.is<JsonObjectConst>()) return at + " must be an object {stars, bearingDeg, distanceM}";
    JsonObjectConst o = b.as<JsonObjectConst>();
    err = unknownKeys(o, BALL_KEYS, at.c_str());
    if (err.length()) return err;
    JsonVariantConst stars = o["stars"];
    if (!stars.is<int>() || stars.as<int>() < 1 || stars.as<int>() > BALL_STARS_MAX)
      return at + ".stars must be an integer 1-" + BALL_STARS_MAX;
    float f;
    if (!numberIn(o["bearingDeg"], 0, BALL_BEARING_MAX_DEG, true, f)) return at + ".bearingDeg must be a number 0-359.9";
    if (!numberIn(o["distanceM"], BALL_DISTANCE_MIN_M, BALL_DISTANCE_MAX_M, false, f))
      return at + ".distanceM must be a number 0.1-100000";
  }
  return "";
}

String radarApiCheckSfx(JsonObjectConst in) {
  String err = unknownKeys(in, SFX_KEYS, "/api/sfx");
  if (err.length()) return err;
  JsonVariantConst v = in["name"];
  if (!v.is<const char *>() || !sfxKnown(v.as<const char *>())) return "name must be one of " + sfxNames();
  return "";
}

// ------------------------------------------------------------------ routes

// Reads the body as a JSON object; false after answering the error.
static bool readObject(JsonDocument &doc) {
  if (!netReadBody(doc)) return false;
  if (!doc.is<JsonObject>()) {
    netSendError(400, "expected a JSON object");
    return false;
  }
  return true;
}

static void sendRadar() {
  JsonDocument doc;
  doc["ok"] = true;
  radarStateJson(doc["radar"].to<JsonObject>());
  netSendJson(doc);
}

static void handleState() {
  JsonDocument doc;
  JsonObject o = doc.to<JsonObject>();
  if (baseStateFn) baseStateFn(o);
  o["ok"] = true;
  radarStateJson(o["radar"].to<JsonObject>());
  netSendJson(doc);
}

static void handleRadar() {
  JsonDocument doc;
  if (!readObject(doc)) return;
  JsonObjectConst in = doc.as<JsonObjectConst>();
  String err = radarApiCheckRadar(in);
  if (err.length()) return netSendError(400, err);
  if (const char *e = radarApply(in)) return netSendError(400, e);
  sendRadar();
}

static void handleBalls() {
  JsonDocument doc;
  if (!readObject(doc)) return;
  JsonObjectConst in = doc.as<JsonObjectConst>();
  String err = radarApiCheckBalls(in);
  if (err.length()) return netSendError(400, err);
  if (const char *e = radarSetBalls(in)) return netSendError(400, e);
  sendRadar();
}

static void handleSfx() {
  JsonDocument doc;
  if (!readObject(doc)) return;
  JsonObjectConst in = doc.as<JsonObjectConst>();
  String err = radarApiCheckSfx(in);
  if (err.length()) return netSendError(400, err);
  const char *name = in["name"];
  JsonDocument out;
  out["ok"] = true;
  out["name"] = name;
  out["played"] = radarPlaySfx(name);  // false while the radar is off or muted
  netSendJson(out);
}

void radarApiRegister(WebServer &server, void (*baseState)(JsonObject out)) {
  baseStateFn = baseState;
  server.removeRoute("/api/state", HTTP_GET);  // the sketch's own one, if it registered it first
  server.on("/api/state", HTTP_GET, handleState);
  server.on("/api/radar", HTTP_POST, handleRadar);
  server.on("/api/balls", HTTP_POST, handleBalls);
  server.on("/api/sfx", HTTP_POST, handleSfx);
}

#else
#warning "radar_api.cpp: radar.h not found, the radar routes are not built"
#endif
