# API del radar

Contrato entre el firmware (`radar.h`/`radar.cpp`), las rutas HTTP (`radar_api.cpp`), la web
embebida (`web/index.html`) y el MCP (`mcp/`). Convenciones de la base (sección 7 de
`esp32-base-proyecto.md`): JSON, `{"error": "..."}` con 4xx si algo no es válido, CORS abierto.

## Tipos

```jsonc
Ball  { "stars": 1-7, "bearingDeg": 0-359.9, "distanceM": 0.1-100000 }
Theme "z" | "classic" | "ptp"           // los de shared/radar.json
Sweep "line" | "ring" | "both" | "none" // sweep.modes de shared/radar.json
Power "on" | "boot" | "powering_off" | "off"
```

`bearingDeg` es el rumbo de la esfera respecto al norte del radar (el yaw 0 que fija `yaw0`), en
sentido horario. Pantalla con el rumbo arriba: el firmware resta el yaw del giroscopio.

## `GET /api/state`

Lo que ya devuelve la base (power, imu, keys, touch, sound...) más:

```jsonc
"radar": {
  "power": "on",
  "zoom": 1,                         // índice en zoomLevels
  "zoomLevels": [{ "cellM": 10, "label": "10 m" }, ...],
  "heading": 37.5,                   // yaw actual en grados (0-360)
  "theme": "z",
  "sweep": "line",                   // barrido: aguja, onda con cada pitido, las dos o ninguno
  "volume": 60, "muted": false,
  "mode": "demo",                    // "demo" = las 7 de radar.json; "manual" = las puestas por API
  "balls": [Ball, ...],              // hasta 7
  "nearestM": 25.0,                  // null sin esferas
  "beepIntervalMs": 1020             // null sin esferas o con el radar apagado
}
```

## `POST /api/radar`

Cambia cualquier subconjunto; responde el objeto `radar` completo.

```jsonc
{ "zoom": 0-3, "theme": "z", "sweep": "line", "volume": 0-100, "muted": true, "power": "on"|"off", "resetHeading": true }
```

`power: "on"` desde apagado reproduce la animación de encendido. `resetHeading` fija el norte del
radar en la orientación actual (como `yaw0`). Se guardan en Preferences: zoom, theme, sweep, volume, muted.

## `POST /api/balls`

```jsonc
{ "balls": [Ball, ...] }   // 0-7 esferas, pasa a mode "manual"; [] deja el radar sin esferas
{ "demo": true }           // vuelve a las 7 de demostración
```

Responde el objeto `radar`. Las esferas manuales se guardan en Preferences.

## `POST /api/sfx`

```jsonc
{ "name": "ball_found" }   // cualquier nombre de shared/sounds.json
```

## CLI serie (mismos efectos)

`radar` (estado), `zoom <0-3>`, `theme <z|classic|ptp>`, `sweep <line|ring|both|none>`, `vol <0-100>`, `mute`, `power on|off`,
`yaw0`, `ball <stars> <bearingDeg> <distanceM>`, `balls clear`, `balls demo`, `sfx <name>`.

## Funciones de `radar.h` (las implementa el firmware, las usan las rutas)

```cpp
void radarBegin();                       // tras lcd/sound/motion
void radarLoop(uint32_t nowMs);          // dibuja, pita, anima
void radarStateJson(JsonObject out);     // el objeto "radar" de arriba
const char *radarApply(JsonObjectConst in);       // POST /api/radar; nullptr o mensaje de error
const char *radarSetBalls(JsonObjectConst in);    // POST /api/balls; nullptr o mensaje de error
bool radarPlaySfx(const char *name);
```
