# AGENTS.md

Contexto para asistentes de IA que trabajen en este repositorio. Lee también
[CONTRIBUTING.md](CONTRIBUTING.md): el modelo de ramas, los idiomas y el estilo aplican igual.

## Qué es esto

Firmware Arduino/C++ que convierte la **Waveshare ESP32-S3-Touch-AMOLED-1.75C** (ESP32-S3R8, 8 MB
de PSRAM octal, 32 MB de flash; AMOLED redonda CO5300 466×466 por QSPI, táctil CST9217, IMU
QMI8658, PMIC AXP2101 con batería, audio ES8311 + NS4150B, micrófonos ES7210) en el Radar del
Dragón del anime: esferas que parpadean, flechas en el borde, cuatro escalas, barrido opcional y
un pitido cuyo ritmo sigue la distancia a la esfera más cercana. Se controla con los dos botones,
la pantalla táctil, el panel web embebido, la API HTTP, la consola serie y el MCP. Ficha de la
placa y trampas: [docs/hardware.md](docs/hardware.md). Diseño del radar, fuentes y decisiones:
[docs/design.md](docs/design.md). Contrato de la API: [docs/api.md](docs/api.md).

Es un proyecto de fans: no metas en el repo arte, fotogramas ni audio del anime. Los valores de
`shared/` se midieron con ellos, pero los ficheros de origen no se guardan.

El pinout está en `firmware/dragonradar/boards/s3_amoled175.h` y lo elige `board.h` con
`-DBOARD_S3_AMOLED175` (`BOARD=amoled`). `board.h`, `boards/`, `lcd.*`, `hw.*`, `touch.*`, `led.*`,
`sound.*`, `devsettings.*`, `net.*` y `cli.*` son comunes con esp-led-matrix y ESPgotchi y se
mantienen idénticos byte a byte: lo propio de la app entra por macros de `config.h` o por hooks.
Lo propio de Dragon Radar: `dragonradar.ino` (entradas y arranque), `radar.*` (estado, dibujo,
pitido, animaciones), `radar_api.*` (rutas `/api/radar`, `/api/balls`, `/api/sfx`), `info.*`
(tarjeta de información), `motion.*` (IMU), `mic.*` (grabación con el ES7210 para medir el
altavoz) y `fonts/`.

El tooling común e idéntico está listado en `core.lock.json`: `core.mk` (todos los targets de
`make` salvo `mcp-install`), `tools/gen_web.mjs`, `tools/gen_sounds.mjs`, `tools/check-manifest.py`,
`tools/check_core.mjs`, `tools/screenshot.py`, `tools/esptool_small.py`, `LICENSE` y `.gitignore`
(salvo su bloque `>>> app`). `.github/workflows/ci.yml` es el de la base con la matriz de placas
dentro del bloque `>>> app`. Lo propio de la app va en el `Makefile` (variables antes del
`include core.mk`) o dentro de esos bloques; no cambies la parte común en un solo repo.

## Núcleo común (`core.lock.json`)

`core.lock.json` lista los ficheros comunes y su sha256 (`sketchFiles`, `repoFiles` y
`blockFiles`, estos sin el bloque de app). `make check` falla si uno cambia sin actualizar el lock.
Para cambiar un fichero común: edítalo y compruébalo (`make build`, `make check`), `make core-lock`,
y en el repo hermano `make core-sync OTHER=../dragon-radar`, `make build-all` y `make check`.
`make core-diff` tiene que salir sin diferencias. No edites los hashes a mano.

## Reglas del repo

- **Código, comentarios, commits, cadenas de UI y logs en inglés. Documentos en español.** La
  landing (`docs/index.html`) es bilingüe con el patrón `span[lang]`.
- **No añadas firmas ni atribuciones de IA** en commits, PRs, código ni docs.
- Identidad: solo el nickname `adrianmb`, `adrianmb.dev` y el usuario `Adrianmtzb` dentro de
  URLs. Ni nombres de pila, ni correos. El enlace de pago solo vive en el bloque de donación de la
  landing y en la línea discreta del portal (`DONATE_URL`).
- No añadas binarios a `docs/`: están ignorados a propósito y los genera el CI.
- Si no puedes verificar algo en hardware, dilo en lugar de darlo por bueno.

## Ficheros generados

| Fichero | Lo escribe | A partir de |
|---|---|---|
| `firmware/dragonradar/radar_assets.h` | `tools/gen_radar.mjs` | `shared/radar.json`, `shared/sounds.json` |
| `firmware/dragonradar/sounds.h` | `tools/gen_sounds.mjs` | `shared/sounds.json` |
| `firmware/dragonradar/web_assets.h` | `tools/gen_web.mjs` | `web/index.html` |
| bloques de `tools/preview/index.html` y `web/index.html` | `tools/preview/build.mjs` | `tools/preview/renderer.js`, `shared/*.json` |

Llevan aviso y están commiteados para que el sketch compile sin Node; editarlos a mano rompe
`make check`. La landing no lleva copia del renderer: `make site` copia
`tools/preview/renderer.js` a `_site/renderer.js` (`SITE_EXTRA` del `Makefile`) y la página lee
`shared/*.json` con `fetch`.

## Comandos

```bash
make deps        # core esp32:esp32@3.2.0 + GFX 1.6.4 + ArduinoJson 7 + SensorLib + XPowersLib
make build       # genera + compila en build/amoled
make flash       # sube por /dev/cu.usbmodem* (PORT=... con más de una placa)
make monitor     # serie 115200, escribe 'help'
make check       # núcleo común + manifest + generados al día + coherencia + tests del MCP
make dist        # binarios con los nombres publicados en dist/ (tras make build)
make shot        # captura la pantalla real (comando `shot`, sin pyserial)
make site        # compila y arma el instalador web en _site/, como el CI (NOBUILD=1 usa dist/)
make og          # tarjeta social de la landing (_site/og.png); la genera el CI, no se commitea
make mcp-install # dependencias del servidor MCP
node tools/preview/build.mjs   # incrusta renderer y JSON en la vista previa y el panel
node tools/preview/render.mjs  # capturas de la vista previa en build/preview/
node tools/landing_shot.mjs    # capturas de la landing en build/shots/ (Chrome sin interfaz)
```

- La compilación tarda unos minutos. **Nunca compiles dos veces a la vez el mismo sketch**:
  colisionan en la caché de arduino-cli.
- Si la subida no conecta: mantener BOOT, pulsar RST, soltar BOOT y repetir.

## Trampas ya pagadas

- **Key2 va al revés que en la S3 1.69**: es la tecla PWRON del AXP2101 invertida por un BSS138 hacia
  GPIO 3, que lee LOW en reposo y HIGH al pulsar (`KEY2_ACTIVE_LEVEL HIGH`). Mantenerla 6 s apaga
  la placa entera por el PMIC; la pulsación larga del firmware (600 ms) queda lejos.
- **El CO5300 necesita zonas de refresco con coordenadas pares**; con canvas completo y `flush()`
  de pantalla entera no afecta. El panel lleva offset de columna 6.
- **El flush es lento**: unos 14 fps con dibujo a 40 MHz; doblar el reloj QSPI solo gana un 20 %.
  No metas efectos que necesiten más.
- **Los micrófonos van al ES7210**, no al ES8311. En el bus I2C hay un `0x7E` sin identificar.
- **El brillo del AMOLED es un comando del panel**, no un PWM de backlight.
- **Los offsets del instalador** son bootloader `0x0`, particiones `0x8000`, boot_app0 `0xE000` y
  app `0x10000`; `tools/check-manifest.py` los vigila junto con la versión.
- **El gzip de `web_assets.h` no es reproducible entre máquinas**: `gen_web.mjs --check`
  descomprime y compara con las fuentes.
- **Nunca compares `now - marca` sin signo** si `marca` puede fijarse después de muestrear `now`:
  usa `(int32_t)(millis() - marca)`.

## Coherencia

`tools/check_consistency.mjs` compara temas, modos de barrido, niveles de zoom, sonidos, campos
de la API y rangos entre `docs/api.md`, `shared/`, las cabeceras generadas, `radar_api.cpp`, el
panel, el MCP y la landing (`THEMES`, `SWEEP_MODES`, `ZOOM_LABELS`, `SFX` y sus etiquetas en
`docs/index.html`). Si añades un tema, un barrido o un sonido, tócalos todos.

## Seguridad

Ver [SECURITY.md](SECURITY.md). La API no devuelve nunca la contraseña del WiFi, lo que llega por
la red se valida contra rangos reales y los datos se insertan en el DOM con `textContent`.
