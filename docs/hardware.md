# Hardware

Ficha de la placa del radar. Sacada de la documentación de Waveshare, de sus ejemplos de Arduino
y del esquemático (`hardware/ESP32-S3-Touch-AMOLED-1.75C-schematic.pdf`, tabla de pines en
`hardware/pin-table.png`). Lo marcado **sin verificar** falta comprobarlo en la placa.

- Docs: https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-1.75C
- Ejemplos: https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75C (`examples/arduino`,
  `libraries/board_config/pin_config.h`)

## Waveshare ESP32-S3-Touch-AMOLED-1.75C

| Parte | Detalle |
|---|---|
| SoC | ESP32-S3R8, 8 MB PSRAM octal (`PSRAM=opi`), **32 MB de flash** (verificado con esptool) |
| USB | USB-C al USB-Serial-JTAG nativo: `/dev/cu.usbmodem*` (`CDCOnBoot=cdc`) |
| Pantalla | AMOLED redonda 1.75", 466×466, driver **CO5300** por QSPI |
| Táctil | **CST9217** (I2C 0x5A), SensorLib `TouchDrvCST92xx` (verificado: `getModelName()` = CST9217) |
| IMU | QMI8658 en I2C **0x6B** (verificado: WHO_AM_I = 0x05; SA0 a GND) |
| PMIC | AXP2101 en I2C 0x34 (XPowersLib; verificado: chip id 0x4A): batería LiPo MX1.25, carga, VBUS, rieles |
| Audio | Códec ES8311 en I2C 0x18 (verificado: id 0x8311) + amplificador NS4150B + altavoz. Los dos micrófonos van a un **ES7210** (ADC, I2C 0x40), no al ES8311 |
| Botones | BOOT (GPIO 0) y Key2, la tecla PWRON del AXP2101, leída en SYS_OUT (GPIO 3) |

Bus I2C (SDA 15, SCL 14) escaneado en la placa: 0x18 ES8311, 0x34 AXP2101, 0x40 ES7210, 0x5A CST9217,
0x6B QMI8658 y **0x7E sin identificar** (no sale en el esquemático; quizá una segunda dirección de alguno
de los chips).

### Pines (esquemático)

| GPIO | Función |
|---|---|
| 1 | LCD_RESET |
| 2 | TP_RESET |
| 3 | SYS_OUT (Key2). **Al revés que en la S3 1.69: LOW en reposo y HIGH al pulsar** (verificado en reposo) |
| 4, 5, 6, 7 | QSPI SIO0..SIO3 |
| 38 | QSPI SCLK |
| 12 | LCD_CS |
| 13 | LCD_TE |
| 11 | TP_INT |
| 14 / 15 | I2C SCL / SDA (táctil, IMU, AXP2101, ES8311) |
| 8 | I2S DSDIN (datos hacia el ES8311, el DAC) |
| 9 | I2S SCLK (BCLK) |
| 10 | I2S hacia el ESP32 (micrófonos; Waveshare lo llama `PIN_ES7210_DIN`) |
| 16 | I2S MCLK |
| 45 | I2S LRCK (WS) |
| 46 | PA_CTRL (activa el amplificador; en alto para sonar) |
| 0 | BOOT |
| 19 / 20 | USB D− / D+ |
| 21 | QMI_INT1 según el esquemático (la tabla de pines lo pone en GPIO 38, que es QSPI SCLK); no se usa |
| 17, 18, 39–42 | Libres en pads de test |

### Key2 y SYS_OUT

Key2 lleva a masa la línea PWRON del AXP2101 (con pull-up propio). Un BSS138 invierte esa línea hacia
GPIO 3, que tiene un pull-up de 10K: en reposo PWRON está alta, el transistor conduce y GPIO 3 lee
**LOW**; al pulsar, PWRON baja, el transistor se corta y GPIO 3 lee **HIGH** (`KEY2_ACTIVE_LEVEL HIGH`).
Medido: `gpio 3 = 0` en reposo en todas las lecturas, y las pulsaciones salen como clics. Como es la
tecla de encendido del PMIC, **mantenerla 6 s apaga la placa** (el AXP2101 viene configurado así,
`powerOff=6s` en `status`): la pulsación larga del firmware (600 ms) queda muy lejos.

### Arduino

Waveshare usa el core 3.3.11, `GFX Library for Arduino` 1.6.4, `SensorLib` 0.3.3 y
`XPowersLib` 0.2.6; "ESP32S3 Dev Module", 32 MB, PSRAM OPI, USB CDC On Boot.

Este repo compila con el **core 3.2.0** de la base y esas mismas versiones de las librerías: GFX 1.6.4
ya trae `Arduino_CO5300`, así que no hace falta subir el core. FQBN (`BOARD=amoled`):
`esp32:esp32:esp32s3:CDCOnBoot=cdc,PartitionScheme=app5M_fat24M_32MB,FlashSize=32M,PSRAM=opi`
(dos apps de 4,5 MB para OTA y 22 MB de FAT).

Constructor de la pantalla en sus ejemplos (los cuatro números finales son offsets del panel):

```cpp
Arduino_DataBus *bus = new Arduino_ESP32QSPI(LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
Arduino_CO5300 *gfx = new Arduino_CO5300(bus, LCD_RESET, 0, 466, 466, 6, 0, 0, 0);
gfx->begin();
gfx->setBrightness(200);  // AMOLED: el brillo es un comando del panel, no hay backlight PWM
```

Táctil en sus ejemplos: reset (TP_RST bajo 30 ms, alto 50 ms), `touch.begin(Wire, 0x5A, 15, 14)`,
`setMaxCoordinates(466, 466)`, `setMirrorXY(true, true)`.

Su ejemplo de LVGL redondea las zonas de refresco a coordenadas pares (x1/y1 pares, x2/y2
impares): el CO5300 lo necesita. Con un canvas completo y `flush()` de pantalla entera no afecta.

Rendimiento medido con el comando `fps` (canvas de 466×466 en PSRAM, `flush()` de pantalla entera):

| Reloj QSPI | flush | dibujo + flush |
|---|---|---|
| 40 MHz (el de Waveshare, el que usa el firmware) | 56,5 ms (17,7 fps) | 71,5 ms (14,0 fps) |
| 80 MHz | 45,6 ms (21,9 fps) | 60,7 ms (16,5 fps) |

Doblar el reloj solo gana un 20 %: la mayor parte del tiempo es el driver de Arduino_GFX
(`Arduino_ESP32QSPI::writePixels`), que copia y voltea los bytes desde PSRAM en bloques de 1024
píxeles con una transacción síncrona cada uno. La imagen a 80 MHz **no está verificada a ojo**.

### Audio

El firmware configura el ES8311 como esclavo, MCLK = 256 × fs desde GPIO 16, I2S de 16 bits a
24 kHz, volumen digital a 0 dB (el nivel se fija en software). El amplificador NS4150B (PA_CTRL,
GPIO 46) solo se enciende mientras suena: verificado `gpio 46 = 1` durante un `beep` y `0` después.

### Alimentación medida

En USB con la batería conectada: VBUS ≈ 4,95 V, sistema ≈ 4,38 V, batería 4,17 V al 100 %, cargador
en estado `done`. El firmware no toca ningún riel: solo fija el objetivo de carga en 4,2 V (lo mismo
que el ejemplo de Waveshare) y enciende las medidas del ADC del PMIC.

### Pendiente de verificar en la placa

- **Sin verificar**: la imagen real del panel (centrado con el offset de columna 6 de Waveshare,
  colores, brillo). Las capturas de `make shot` son el framebuffer, no lo que muestra el cristal.
- **Sin verificar**: la calibración del táctil (1:1 con `setMirrorXY(true, true)`). Un toque real dio
  `raw=(442, 335)`, dentro del rango 0–465, pero falta medirlo con cruces como en las otras placas.
- **Sin verificar**: la orientación de los ejes del QMI8658 respecto a la pantalla (`IMU_NATIVE_AXES`).
- **Sin verificar**: que el altavoz suene; la señal llega al códec y el amplificador se activa, pero
  nadie lo ha escuchado todavía.
