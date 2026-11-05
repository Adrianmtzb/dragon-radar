# Dragon Radar

[English below](#english)

El Radar del Dragón del anime en la Waveshare ESP32-S3-Touch-AMOLED-1.75C: pantalla AMOLED redonda de
466×466 con la cuadrícula verde, siete esferas que parpadean con su número de estrellas, flechas en
el borde para las que quedan fuera, cuatro escalas (10 m, 100 m, 1 km y 10 km por celda) y el
«pi-PI» que se acelera cuando te acercas a la esfera más cercana. Panel web servido por el chip,
API HTTP, consola serie y servidor MCP.

Landing con instalador web y demo: <https://adrianmtzb.github.io/dragon-radar/>

> Proyecto de fans, sin afiliación con los titulares de los derechos de Dragon Ball. No usa arte ni
> audio con copyright: los gráficos se dibujan con código y los sonidos se sintetizan
> ([`docs/design.md`](docs/design.md) explica de dónde sale cada valor).

## Placa

| Placa | `BOARD=` | Hostname |
|---|---|---|
| Waveshare ESP32-S3-Touch-AMOLED-1.75C (CO5300, CST9217, QMI8658, AXP2101, ES8311 + NS4150B, ES7210) | `amoled` | `dragonradar.local` |

Ficha, pinout y trampas en [`docs/hardware.md`](docs/hardware.md).

## Controles

| Entrada | Clic / toque | Mantener |
|---|---|---|
| BOOT | aleja un nivel (del último vuelve al primero) | tarjeta de información |
| KEY2 | acerca un nivel | 600 ms: apaga o enciende el radar; **6 s: el AXP2101 apaga la placa** |
| Pantalla | toque: aleja; doble toque: información; deslizar: tema | fija el norte en la orientación actual |

El giroscopio gira el mapa (rumbo arriba). Cada pulsación hace el «brrr» del botón.

## Instalar

Desde el navegador: abre la landing en Chrome o Edge de escritorio, conecta la placa por USB-C y
pulsa **Instalar en tu placa**. O compílalo:

```bash
make deps                     # core esp32 3.2.0 y librerías con versión fijada
make build                    # compila en build/amoled
make flash PORT=/dev/cu.usbmodemXXXX
make monitor PORT=/dev/cu.usbmodemXXXX   # escribe 'help' en la consola
make shot PORT=/dev/cu.usbmodemXXXX OUT=build/shots/pantalla.png
make check                    # ficheros comunes, manifest, generados al día, coherencia y tests del MCP
make site NOBUILD=1           # arma _site/ con lo que haya en dist/ (como el CI)
make og                       # tarjeta social en _site/og.png
make mcp-install              # dependencias del servidor MCP
```

Sin WiFi guardada, la placa abre la red `DragonRadar-XXXX` con un portal para elegir la red.
Después responde en `http://dragonradar.local`.

## API y MCP

`GET /api/state`, `POST /api/radar` (`zoom`, `theme`, `sweep`, `volume`, `muted`, `power`,
`resetHeading`), `POST /api/balls` (hasta 7 esferas o `{"demo": true}`) y `POST /api/sfx`.
Contrato completo en [`docs/api.md`](docs/api.md).

El servidor MCP (`mcp/server.mjs`) ofrece `get_state`, `set_radar`, `set_balls` y `play_sfx`:

```json
{ "mcpServers": { "dragon-radar": {
  "command": "node", "args": ["mcp/server.mjs"],
  "env": { "DRAGONRADAR_HOSTS": "dragonradar.local" } } } }
```

## Estructura

| Ruta | Qué hay |
|---|---|
| `firmware/dragonradar/` | sketch de Arduino; `radar.*` es el dominio, el resto es la base común |
| `shared/radar.json`, `shared/sounds.json` | fuente única del aspecto, el comportamiento y los sonidos |
| `tools/preview/` | vista previa en el navegador y `renderer.js`, el dibujo que comparten la vista previa, el panel y la landing |
| `web/index.html` | panel web embebido |
| `docs/` | landing (`index.html`), `manifest.json` del instalador, diseño, API y hardware |
| `mcp/` | servidor MCP y sus tests |

Cómo contribuir: [`CONTRIBUTING.md`](CONTRIBUTING.md). Seguridad: [`SECURITY.md`](SECURITY.md).

## Licencia

MIT. Hecho por [adrianmb](https://adrianmb.dev).

---

## English

The Dragon Radar from the anime on the Waveshare ESP32-S3-Touch-AMOLED-1.75C: a 466×466 round AMOLED
with the green grid, seven blinking balls with their star numbers, edge arrows for the ones out of
view, four scales (10 m, 100 m, 1 km and 10 km per cell) and the "pi-PI" beep that speeds up as you
get closer to the nearest ball. A web panel served by the chip, an HTTP API, a serial console and
an MCP server.

Landing page with the web installer and a live demo: <https://adrianmtzb.github.io/dragon-radar/>

> Fan project, not affiliated with the rights holders of Dragon Ball. No copyrighted artwork or
> audio is used: the graphics are drawn in code and the sounds are synthesised.

**Controls.** BOOT: click zooms out (wrapping), hold shows the info card. KEY2: click zooms in,
hold 600 ms powers the radar off or on; holding it for 6 s makes the AXP2101 switch the board off.
Screen: tap zooms out, double tap shows the info card, swipe changes the theme, long press sets
north. The gyro turns the map.

**Install.** Open the landing page in desktop Chrome or Edge, plug the board in over USB-C and
press **Install on your board**, or build it with `make deps && make flash PORT=...` (see the
commands above). With no saved WiFi the board opens the `DragonRadar-XXXX` network with a setup
portal; then it answers at `http://dragonradar.local`.

**API and MCP.** See [`docs/api.md`](docs/api.md) and the MCP config above (`make mcp-install`
first). Documentation is in Spanish; code, comments and commits are in English.

MIT license. Made by [adrianmb](https://adrianmb.dev).
