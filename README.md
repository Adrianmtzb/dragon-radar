# Dragon Radar

Radar del dragón al estilo de Dragon Ball para la Waveshare ESP32-S3-Touch-AMOLED-1.75C: pantalla
AMOLED redonda de 466×466, táctil, IMU, batería y altavoz.

> En construcción: por ahora el firmware arranca la placa (pantalla, táctil, botones, IMU, PMIC,
> sonido, WiFi con portal de configuración y API) y muestra una pantalla de inicio provisional.

## Placa

| Placa | `BOARD=` |
|---|---|
| Waveshare ESP32-S3-Touch-AMOLED-1.75C | `amoled` |

Ficha, pinout y trampas en [`docs/hardware.md`](docs/hardware.md).

## Puesta en marcha

```bash
make deps                     # core esp32 3.2.0 y librerías con versión fijada
make build                    # compila en build/amoled
make flash PORT=/dev/cu.usbmodemXXXX
make monitor PORT=/dev/cu.usbmodemXXXX   # escribe 'help' en la consola
make shot PORT=/dev/cu.usbmodemXXXX OUT=build/shots/pantalla.png
make check                    # ficheros comunes, manifest y generados al día
```

Sin WiFi guardada, la placa abre la red `DragonRadar-XXXX` con un portal para elegir la red. Después
responde en `http://dragonradar.local` (`/api/state`, `/api/info`).

## Licencia

MIT. Hecho por [adrianmb](https://adrianmb.dev).
