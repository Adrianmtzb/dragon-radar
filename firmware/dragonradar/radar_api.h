#pragma once
// The radar's HTTP API (docs/api.md), on top of the common routes of net.cpp:
//   GET  /api/state  the sketch's state plus the "radar" object (radarStateJson)
//   POST /api/radar  {zoom, theme, sweep, volume, muted, power, resetHeading}: any subset -> {ok, radar}
//   POST /api/balls  {balls: [Ball, ...]} (0-7) or {demo: true} -> {ok, radar}
//   POST /api/sfx    {name} -> {ok, name, played}
// Every body is validated here (types, ranges, unknown fields, theme, sweep and sound names) before it
// reaches radar.h; a bad one gets {ok: false, error} with 400.
#include <ArduinoJson.h>
#include <WebServer.h>

// Call from NetHooks::routes, passing the sketch's builder of the common /api/state fields:
//   static void routes(WebServer &server) { radarApiRegister(server, stateJson); }
// It replaces a GET /api/state registered earlier, so where it goes in routes() does not matter.
void radarApiRegister(WebServer &server, void (*baseState)(JsonObject out));

// The validators behind the routes (also usable by the CLI): "" when the body is valid,
// otherwise the error message.
String radarApiCheckRadar(JsonObjectConst in);
String radarApiCheckBalls(JsonObjectConst in);
String radarApiCheckSfx(JsonObjectConst in);
