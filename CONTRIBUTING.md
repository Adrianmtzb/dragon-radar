# Cómo contribuir

Gracias por pasarte. Es un proyecto pequeño y el proceso es ligero a propósito.

---

## Modelo de ramas

**Trunk-based**: una sola rama larga, `main`, siempre en estado publicable.

```
main ────●────●────●────●──── (siempre desplegable, protegida)
          \        /
           ●──●──●  feat/algo   (vida corta, se fusiona con squash)
```

- `main` está protegida. No se hace push directo.
- Cada cambio va en una rama corta que sale de `main` y vuelve por PR.
- Las PRs se fusionan con **squash**, para que `main` quede con un commit legible por cambio.
- Las versiones se marcan con **tags** `vX.Y.Z`, no con ramas. `make bump VERSION=X.Y.Z`
  actualiza `config.h` y `docs/manifest.json` a la vez; el tag dispara la release y la landing.

Prefijos de rama: `feat/`, `fix/`, `docs/`, `refactor/`, `chore/`.

---

## Idiomas

- **Código, comentarios, mensajes de commit, cadenas de la UI y logs: en inglés.**
- **Documentación (README, guías, CHANGELOG): en español.** La landing de `docs/` es
  bilingüe español/inglés con el marcado duplicado (`<span lang="es">` / `<span lang="en">`) y
  CSS que oculta la lengua que no toca.

---

## Antes de mandar la PR

El CI compila cada PR y comprueba que los ficheros generados están al día. Lo que **no** puede
comprobar es lo importante de este proyecto:

1. **Que funcione en la placa.** Los tiempos de los botones y del táctil, el giroscopio, el sonido
   y el rendimiento de la pantalla solo se ven con la placa delante. Cuenta en la PR qué probaste.
2. **Si tocaste el dibujo del radar**, compáralo con la vista previa (`tools/preview/index.html`)
   y adjunta una captura real con `make shot`.
3. **Si tocaste la red**, prueba los dos modos: arranque sin credenciales (portal) y con
   credenciales válidas (STA + mDNS).
4. **Si tocaste `web/index.html` o la landing**, ábrelas también en un móvil.

```bash
make deps     # una vez: core ESP32 3.2.0 y librerías fijadas
make build    # regenera y compila en build/amoled
make flash    # compila y sube (detecta el puerto usbmodem)
make monitor  # consola serie; escribe 'help'
make check    # lo mismo que valida el CI
```

### Ficheros generados

`radar_assets.h`, `sounds.h` y `web_assets.h` los escriben `tools/gen_radar.mjs`,
`tools/gen_sounds.mjs` y `tools/gen_web.mjs` a partir de `shared/radar.json`, `shared/sounds.json`
y `web/index.html`. `tools/preview/build.mjs` incrusta `tools/preview/renderer.js` y los JSON en la
vista previa y en el panel. Van commiteados, pero **no se editan a mano**: `make check` falla si no
cuadran con sus fuentes.

### Código común con los otros proyectos

`core.mk`, `board.h`, `boards/`, `lcd`, `hw`, `touch`, `led`, `sound`, `devsettings`, `net`, `cli`
y varias herramientas de `tools/` son idénticos en esp-led-matrix, ESPgotchi y este repo.
`core.lock.json` guarda el hash de cada uno: tras editar uno, `make core-lock` aquí y
`make core-sync` en los otros (ver [AGENTS.md](AGENTS.md#núcleo-común-corelockjson)).

### Los binarios del instalador

No están en el repositorio. `docs/` guarda la página y el `manifest.json`; los `.bin` los
compila y publica el CI al crear un tag.

---

## Cómo añadir un tema, un barrido o un sonido

1. Añádelo en `shared/radar.json` (`themes`, `sweep.modes`) o en `shared/sounds.json` (`sfx`).
2. Añádelo en la API (`docs/api.md` y `radar_api.cpp`), en el MCP (`mcp/tools.mjs`) y en las
   listas de la landing (`THEMES`, `SWEEP_MODES`, `SFX` y sus etiquetas en `docs/index.html`).
3. `node tools/preview/build.mjs`, `make check` (que compara todas las listas) y `make build`.

Nada de arte, fotogramas ni audio del anime en el repo: los colores y los tonos se miden y se
apuntan en `docs/design.md`, y el dibujo y el sonido se generan.

---

## Estilo

- C++ al estilo del código existente: sin excepciones ni `std::string`, nada de `delay()` en
  el loop, `millis()` para todo lo temporal.
- En la web, los datos se insertan con `textContent`/`createElement`, nunca con `innerHTML`.
- Comentarios solo donde explican un *por qué* que el código no cuenta.
- Sin firmas ni atribuciones de herramientas o asistentes en commits, PRs, código ni docs.
