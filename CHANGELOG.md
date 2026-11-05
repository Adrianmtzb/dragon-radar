# Changelog

Formato basado en [Keep a Changelog](https://keepachangelog.com/es-ES/1.1.0/).
Este proyecto sigue [Versionado Semántico](https://semver.org/lang/es/).

## [0.0.1] - 2026-11-04

Primera versión, todavía sin release.

### Añadido

- **Placa**: arranque de la Waveshare ESP32-S3-Touch-AMOLED-1.75C sobre la base común (`BOARD=amoled`):
  pantalla CO5300 466×466 por QSPI, táctil CST9217, IMU QMI8658, PMIC AXP2101 con batería y la tecla
  Key2, audio ES8311 + NS4150B y micrófonos ES7210. Ficha y trampas en `docs/hardware.md`.
- **Radar**: cuadrícula verde fija, siete esferas con su número de estrellas que parpadean con cada
  pitido, fusión de las que caen juntas, flechas en el borde para las que quedan fuera, marcador
  rojo en el centro y mapa con el rumbo arriba que gira con el giroscopio. Tres temas (`z`,
  `classic`, `ptp`), cuatro escalas (10 m, 100 m, 1 km y 10 km por celda) con transición, etiqueta
  de escala, animaciones de encendido y apagado, y barrido opcional (`line`, `ring`, `both`, `none`).
- **Sonido**: el «pi-PI» medido en el anime (2130 + 4260 Hz, onda triangular) con un ritmo que sigue
  la distancia a la esfera más cercana en escala logarítmica, el «brrr» de cada pulsación y los
  avisos de esfera cerca y encontrada. Todo sintetizado desde `shared/sounds.json`.
- **Controles**: BOOT aleja y mantenido muestra la tarjeta de información; Key2 acerca y mantenido
  600 ms apaga o enciende; en la pantalla, tocar aleja, doble toque muestra la información, deslizar
  cambia de tema y la pulsación larga fija el norte.
- **Panel web** embebido con el radar en vivo, dibujado con el mismo `tools/preview/renderer.js` que
  la vista previa; **API** `GET /api/state` y `POST /api/radar`, `/api/balls`, `/api/sfx`
  (`docs/api.md`); **consola serie** con los mismos comandos y **MCP** con `get_state`, `set_radar`,
  `set_balls` y `play_sfx`.
- **Landing** bilingüe en GitHub Pages con instalador web (esp-web-tools), demo en vivo con los
  mismos datos, gestos y sonidos que la placa, y tarjeta social generada por `tools/gen_og.mjs`.
- **CI**: compila, comprueba (`make check`), publica la release en cada tag `v*` y despliega la
  landing con los binarios.
- `tools/check_consistency.mjs` compara los nombres y rangos entre la API, `shared/`, el firmware,
  el panel, el MCP y la landing.
