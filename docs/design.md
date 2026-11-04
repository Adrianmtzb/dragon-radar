# Diseño del radar

Cómo es el Radar del Dragón original (el de Bulma, en el manga y el anime de *Dragon Ball*, *Z*,
*GT* y *Super*), qué reproducimos y qué adaptamos a la pantalla AMOLED redonda de 466×466 de la
placa (`hardware.md`).

Los valores viven en dos ficheros que son la única fuente de verdad:

- `shared/radar.json`: paletas, geometría (relativa al radio R de la pantalla), niveles de zoom,
  esferas, parpadeo, arranque, transiciones, fuente de trazos y las siete esferas de ejemplo.
- `shared/sounds.json`: catálogo de sonidos en el formato de melodía de `sound.h` y la regla que
  relaciona el ritmo del pitido con la distancia.

De ahí salen `firmware/dragonradar/radar_assets.h` (`tools/gen_radar.mjs`),
`firmware/dragonradar/sounds.h` (`tools/gen_sounds.mjs`, el generador común de la base) y la vista
previa `tools/preview/index.html`, que es la referencia visual que tiene que igualar el firmware.

## Fuentes

| # | Fuente | Qué aporta |
|---|---|---|
| 1 | Dragon Ball Wiki (Fandom), artículo *Dragon Radar*, leído con la API: <https://dragonball.fandom.com/wiki/Dragon_Radar> | Las esferas emiten un pulso electromagnético débil (Bulma, manga vol. 1, cap. 1, p. 25). El radar "apunta flechas en la dirección de las esferas cercanas" y el botón de arriba hace zoom. Colores de la ficha: blanco y verde `#008000`. El radar del Ejército Red Ribbon no puede hacer zoom y solo sitúa la esfera en un radio de 100 km. |
| 2 | Dragon Ball Wiki Hispano, *Radar del Dragón*: <https://dragonball.fandom.com/es/wiki/Radar_del_Drag%C3%B3n> | Disco blanco con una perilla arriba y pantalla circular verde con cuadrícula. Las esferas son puntos amarillos. Pulsar el botón más de una vez amplía la imagen. Seis esferas juntas pueden salir como seis puntos o como un único punto con el número seis. |
| 3 | Dragon Ball Encyclopedia: <https://dragonballencyclopedia.com/wiki/Dragon_Radar> | Cada pulsación del botón aleja la vista y muestra más y más planeta. Hubo varios radares (Bulma, Turbo, el del Rey Piccolo). |
| 4 | Kanzenshuu, *Dragon Radar*: <https://www.kanzenshuu.com/wiki/Dragon_Radar> | La vista normal es una cuadrícula sobre pantalla verde ("greenscreen grid view"). Capítulo 1: el radar capta las ondas que emiten las esferas (<https://www.kanzenshuu.com/wiki/Dragon_Ball_Chapter_1>). |
| 5 | Bandai Tamashii Nations, PROPLICA Dragon Radar: <https://tamashiiweb.com/item/13034/> | Réplica oficial a escala (unos 100 mm): esferas con LED, sonido de arranque, el botón cambia de modo. |
| 6 | Fotogramas del wiki (1): `Radar.png` (DB clásico), `DBLocationsOnNamek.png` (Z, Namek), `DragonRadar(PtP).png` (*The Path to Power*), `GiruRadar.png` (GT) | Colores y medidas muestreados píxel a píxel (ver abajo). Las imágenes se usaron solo para medir y **no están en el repo**. |
| 7 | Réplicas de aficionados: tokio666/db-radar <https://github.com/tokio666/db-radar> (ESP32-S3, 5 niveles de 50 m a 1 km, puntos amarillos, triángulo rojo en el centro, flechas en el borde), Extra Long Division <https://www.hackster.io/extralongdivision/dragon-ball-radar-b8a9cc> (pita al pulsar y propone pitar más rápido cuanto más cerca), ThorAsgardDev/dragon-radar <https://github.com/ThorAsgardDev/dragon-radar> | Cómo han resuelto otros lo que el anime no define: escala por nivel, flechas, ritmo del pitido. |

## El original

**Carcasa.** Disco blanco o gris claro, del tamaño de la palma (la PROPLICA mide unos 100 mm), con
una perilla moleteada arriba que es el único botón. La pantalla ocupa casi toda la cara; la
rodea un aro de la carcasa, sin escala ni marcas impresas.

**Pantalla.** Cristal verde con una cuadrícula cuadrada fija. El tono cambia según la época:

| Época | Pantalla | Cuadrícula | Celdas a lo ancho | Esferas |
|---|---|---|---|---|
| *Dragon Ball* (1986) | verde oliva `#506840` | líneas claras `#829c75`, finas | ~12 | resplandor naranja `#e89848` con centro amarillo `#fde46e`, sin números |
| *Z* (Namek) | verde vivo `#008008`–`#0f8c14` | líneas oscuras `#004800`, ~2 px a R≈133 px | ~9 | disco blanco amarillento `#fffbe5`, halo `#ecef88`, número al lado (`4`, `1`, `1`) |
| *The Path to Power* (1996) | verde azulado `#00a878` | casi invisible, cruz en el centro | — | triángulos rojos en el borde apuntando hacia las esferas |
| *GT* (Giru) | verde oscuro `#2a5a06` | líneas oscuras | ~16 | triángulo amarillo `#c8b41f` en el centro, coordenadas "X 12.5 Y 9.2.1.2" con línea guía, reflejo de cristal arriba a la izquierda |

En el fotograma de Namek el centro de la pantalla cae en el **centro de una celda**, el paso de la
cuadrícula es ~0,22 R (unas 9 celdas a lo ancho) y el marcador del centro es un **triángulo rojo**
`#e80d07` de ~0,12 R de base, con la punta hacia arriba.

**Usuario.** El triángulo del centro es quien lleva el radar. La cuadrícula no gira.

**Esferas.** Puntos amarillos que parpadean. Cuando están juntas se ven como un solo punto, a
veces con el número de esferas (2). Las que están fuera de la vista se indican con flechas en
dirección a ellas (1), como los triángulos rojos del borde en *The Path to Power*.

**Zoom.** Cada pulsación del botón de arriba aleja la vista (1, 3); se ve más mapa y las esferas
se acercan al centro. Ninguna fuente explica cómo se vuelve a acercar ni da la escala de cada
nivel; la única cifra es el radio de 100 km del radar del Red Ribbon, que "no hace zoom" (1).

**Encendido.** Se enciende pulsando la perilla; en la serie la pantalla aparece ya con la
cuadrícula y las esferas. La PROPLICA tiene un sonido de arranque (5). No hay una animación de
encendido documentada.

**Sonido.** Pitidos electrónicos cortos y agudos ("pi... pi...") mientras busca. Ninguna fuente
fija la frecuencia ni que el ritmo dependa de la distancia: eso es una convención de detector de
metales que también proponen las réplicas (7).

**Escaneo.** No hay línea de barrido giratoria como en un radar de avión: los puntos aparecen y
parpadean. Tampoco hay texto en pantalla salvo los números de las esferas agrupadas (*Z*) y las
coordenadas de *GT*.

## Decisiones

| Aspecto | Original | Qué hacemos | Cómo (clave en `radar.json` / `sounds.json`) |
|---|---|---|---|
| Estilo por defecto | Varía por época | **Reproducimos** el de *Z* (Namek), el más reconocible y el que pide el proyecto: verde vivo con cuadrícula más oscura | `theme: "z"`; `classic` (DB 1986) y `ptp` como temas alternativos |
| Color de pantalla | `#0f8c14` plano con algo de viñeta | **Reproducimos** con un degradado radial `#18a020` → `#0c8a12` → `#04620a` | `themes.z.screen*`, `shading.midStop` |
| Aro / bisel | Aro de la carcasa alrededor del cristal | **Adaptamos**: la carcasa no existe en la pantalla; dibujamos solo un oscurecimiento en el borde (3,5 % de R) donde el cristal se une al aro. Fuera del círculo, negro AMOLED (píxeles apagados) | `shading.rimWidthR`, `themes.*.rim` |
| Reflejo del cristal | Visible en *GT* | **Reproducimos** muy sutil (7 % de alfa) | `shading.sheen` |
| Cuadrícula | Cuadrada, fija, ~9 celdas, centro en mitad de celda, líneas oscuras | **Reproducimos**: paso 0,222 R (51,7 px), línea 0,014 R (3,3 px), desplazada media celda, no gira con el rumbo | `grid`, `zoom.levels[].pitchR`, `lineWidthR` |
| Marcador | Triángulo rojo con la punta arriba | **Reproducimos**: triángulo isósceles 0,12 R × 0,11 R con el baricentro en el centro y borde oscuro. Mapa con el rumbo arriba: el giroscopio gira las esferas, no el triángulo | `marker`, `themes.z.marker` |
| Esferas | Disco claro con halo, parpadeo | **Reproducimos**: disco de 0,055 R y halo de 0,14 R con caída `(1 − d/r)^1.3`; el color pasa de `ballDim` a `ballCore` con el brillo | `ball` |
| Agrupadas | Un punto con número (*Z*) | **Reproducimos**: los puntos a menos de 0,08 R se funden en su centroide; en el tema *Z* todos los puntos llevan su número (como en Namek), en los otros solo los grupos | `ball.mergeR`, `themes.*.counts` |
| Fuera de la vista | "Flechas" (wiki), triángulos rojos en el borde (*PtP*) | **Reproducimos**: triángulo en 0,89 R apuntando hacia fuera en el rumbo de la esfera; flechas a menos de 6° se dibujan una vez | `arrow` |
| Parpadeo | Parpadean | **Reproducimos** sincronizado con el pitido: sube a brillo 1 en 30 ms y decae (τ = 380 ms) hasta 0,35. Sin pitido, cada 1000 ms | `blink` |
| Zoom | El botón de arriba aleja; escala desconocida | **Reproducimos** el gesto y **fijamos** 4 niveles: 10 m, 100 m, 1 km y 10 km por celda (radio visible 45 m, 450 m, 4,5 km y 45 km). Un botón aleja y al pasar del último vuelve al primero; el otro acerca (cuál es cuál lo decide el firmware según dónde quede cada uno en la caja). Arranca en 100 m | `zoom.levels`, `zoom.default`, `zoom.wrap` |
| Cambio de nivel | No se ve en detalle | **Adaptamos**: 280 ms; la escala se interpola en logaritmo, la cuadrícula vieja se encoge y se desvanece, la nueva aparece, y los puntos se deslizan a su sitio | `transitions.zoom` |
| Texto | Solo números de grupo (*Z*) o coordenadas (*GT*) | **Adaptamos**: tras un cambio de zoom se ve 1,2 s la escala ("1 km") debajo del marcador, con la misma fuente de trazos que los números. Desactivable. Sin coordenadas | `scaleLabel`, `font` |
| Encendido | Sin animación documentada | **Adaptamos** una secuencia de 1,5 s: la pantalla se enciende con dos parpadeos (0–240 ms), la cuadrícula se revela del centro hacia fuera (240–700), el marcador parpadea dos veces (700–940), un anillo de escaneo recorre la pantalla (940–1240) y aparecen las esferas (1240–1500) | `boot` |
| Barrido | No existe | **No lo ponemos**; queda un interruptor explícito apagado | `sweep.enabled: false` |
| Apagado | — | **Adaptamos**: las esferas se van en 120 ms y la pantalla funde a negro en 450 ms | `transitions.powerOff` |
| Pitido | "Pi" electrónico agudo, ritmo sin documentar | **Reproducimos** un pip senoidal de 2400 Hz y 55 ms; **adaptamos** el ritmo a la distancia (ver abajo) | `sfx.radar_beep`, `beep` |
| Sonidos de evento | La PROPLICA tiene sonido de arranque | **Adaptamos** arranque, clic de zoom (alejar más agudo, acercar más grave), esfera cerca (3 pips), esfera encontrada (4 notas que suben) y apagado | `sfx` |
| Tamaños | — | **Adaptamos**: todo relativo a R, así el firmware (R = 233 px) y la web dibujan lo mismo a cualquier tamaño | `refSizePx: 466` |

### Ritmo del pitido

El intervalo depende de la distancia `d` de la esfera más cercana, en metros y en escala
logarítmica, para que se note tanto a 5 km como a 10 m:

```
t = clamp((log10(farM) − log10(d)) / (log10(farM) − log10(nearM)), 0, 1)
intervalo = maxIntervalMs + (minIntervalMs − maxIntervalMs) · t^curvePow
```

Con `nearM = 3`, `farM = 10000`, 120–1400 ms y `curvePow = 1`:

| Distancia | 3 m | 10 m | 30 m | 100 m | 300 m | 1 km | 3 km | ≥ 10 km |
|---|---|---|---|---|---|---|---|---|
| Un pitido cada | 120 ms | 310 ms | 483 ms | 673 ms | 847 ms | 1037 ms | 1210 ms | 1400 ms |

La distancia es absoluta: hacer zoom no cambia el ritmo. Al bajar de 30 m suena `ball_close` una
vez y al bajar de 3 m `ball_found`; cada aviso se rearma cuando la distancia vuelve a pasar del
umbral × 1,3. Los puntos destellan con cada pitido.

## Cómo se dibuja (para el firmware)

Coordenadas en unidades de R con origen en el centro, x a la derecha, y hacia abajo; rumbos en
sentido horario desde el norte. El orden de capas y las fórmulas son las de `index.html`:

1. Fondo `background` (negro) y recorte al círculo de radio R.
2. Pantalla: degradado radial `screenCenter` (0) → `screen` (`midStop`) → `screenEdge` (1).
3. Cuadrícula: líneas verticales y horizontales en `(k + offsetCells) · pitchR`, ancho
   `lineWidthR`, color `grid`. Durante el arranque se recorta a un círculo que crece.
4. Reflejo: elipse con degradado de `sheen`, alfa `shading.sheen.alpha`.
5. Esferas. Escala `s = pitchR / cellM` (R por metro). Para cada esfera, `rel = rumbo − heading`,
   `r = d · s`, `x = sin(rel) · r`, `y = −cos(rel) · r`. Si `r > visibleR`, flecha en el borde; si
   no, se ordenan por distancia y cada una se une al primer grupo a menos de `mergeR` (el centroide
   se recalcula) o abre uno nuevo. Halo: alfa `glowAlpha · b · (1 − d/glowR)^glowFalloff`; núcleo
   `mix(ballDim, ballCore, b)`; número a `(digitDxR, digitDyR)` del centro.
6. Marcador encima de las esferas.
7. Anillo de escaneo (solo en el arranque), etiqueta de escala, oscurecimiento del borde.
8. Fundido a negro del arranque y del apagado.

Brillo del parpadeo `b(e)`, con `e` = ms desde el último pitido: `dim + (1 − dim) · e/rise` mientras
`e < rise`, y después `dim + (1 − dim) · exp(−(e − rise)/decay)`.

Transición de zoom con progreso `p = easeOutCubic(t / durationMs)`: escala
`s = exp(lerp(ln sDesde, ln sHasta, p))`; la cuadrícula vieja se dibuja con paso
`pitchR · s / sDesde` y alfa `1 − p`, la nueva con `pitchR · s / sHasta` y alfa `p`.

La fuente de los números es de trazos (polilíneas sobre una caja de 4×6), así que escala sin
mapas de bits y se dibuja igual en la web y en el panel.

## Sonidos

Formato de melodía de `sound.h`: tonos `HZ:MS`, silencios `R:MS` y `~square` / `~sine` para la
forma de onda; `gain` es el porcentaje del volumen del usuario. Se sintetizan como en el ES8311 de
`sound.cpp`: onda cuadrada o senoidal sin filtrar, rampa de amplitud de 2 ms, nivel
`0,915 · (volumen/100)²`. `tools/preview/render_wav.mjs` los escribe en `build/sounds/` a 24 kHz.

| Sonido | Notas | Duración |
|---|---|---|
| `power_on` | barrido cuadrado 600 → 1700 Hz (12 × 20 ms), pip a 2400 Hz en 700 ms (aparece el marcador), pip doble 2400/3200 Hz en 940 ms (anillo de escaneo) | 1070 ms |
| `radar_beep` | seno 2400 Hz, 55 ms | 55 ms |
| `zoom_out` / `zoom_in` | clic cuadrado 3600 Hz de 5 ms y blip senoidal de 1900 / 1400 Hz | 52 ms |
| `ball_close` | tres pips de 2400 Hz | 205 ms |
| `ball_found` | 1568, 2093, 2637 y 3136 Hz (sol-do-mi-sol) | 435 ms |
| `power_off` | barrido cuadrado 1800 → 380 Hz | 280 ms |

## Vista previa

`tools/preview/index.html` se abre con `file://`, sin servidor. `node tools/preview/build.mjs`
incrusta los dos JSON (con `--check` comprueba que están al día). Tiene las siete esferas de
ejemplo, los niveles de zoom (perilla, clic en la pantalla, `Espacio`/`↑`, `↓`), el arranque, el
parpadeo, el pitido con WebAudio, un deslizador de rumbo que simula el giroscopio y otro que
acerca o aleja todas las esferas para oír cómo cambia el ritmo. `node tools/preview/render.mjs`
saca las capturas de `build/preview/` con Chrome sin interfaz.

## Pendiente

- La frecuencia y el timbre exactos del pitido del anime no están documentados; se eligieron de
  oído. Si aparece una referencia mejor, se cambia en `sounds.json`.
- `sounds.h` no lleva la ganancia por sonido: está en `RADAR_SFX_GAIN` de `radar_assets.h` y el
  firmware tiene que aplicarla al llamar a `soundPlaySfx`.
- Las esferas de ejemplo son fijas. De dónde salen las esferas reales (GPS, balizas BLE, juego) se
  decide en otra tarea.
