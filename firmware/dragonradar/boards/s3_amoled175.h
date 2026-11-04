#pragma once
// Waveshare ESP32-S3-Touch-AMOLED-1.75C (https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-1.75C):
// round 466x466 AMOLED with a CO5300 driver on QSPI, ESP32-S3R8 (8 MB octal PSRAM), 32 MB flash,
// CST9217 touch, QMI8658 IMU, AXP2101 PMIC (battery gauge, charger, VBUS sense) and an ES8311
// codec with an NS4150B amplifier on I2S. Same target as the 1.69 board, so board.h picks this
// header only when BOARD_S3_AMOLED175 is defined (BOARD=amoled in the Makefile).
// Pinout from the schematic (docs/hardware/), checked against Waveshare's pin_config.h.
#define BOARD_NAME "Waveshare ESP32-S3-Touch-AMOLED-1.75C"
#define BOARD_ID "s3-amoled175"
#define BOARD_SHORT "amoled"  // BOARD= in the Makefile

// Panel: CO5300 over QSPI. No backlight pin: an AMOLED sets its brightness with a panel command.
#define LCD_DRIVER_CO5300 1
#define PIN_LCD_CS 12
#define PIN_LCD_SCLK 38
#define PIN_LCD_SDIO0 4
#define PIN_LCD_SDIO1 5
#define PIN_LCD_SDIO2 6
#define PIN_LCD_SDIO3 7
#define PIN_LCD_RST 1
#define PIN_LCD_TE 13    // tearing effect output of the panel, unused for now
// QSPI clock. Waveshare's examples use the Arduino_GFX default of 40 MHz: 56.5 ms per full-frame
// flush (17.7 fps). 80 MHz measured 45.6 ms (21.9 fps) but the image is not verified at that speed;
// most of the flush is the driver's byte swap from PSRAM, not the bus.
#define LCD_QSPI_HZ 40000000

#define PIN_BOOT_BTN 0  // BOOT key, strapping pin, usable as a button after boot
// Second key. Key2 pulls the AXP2101 PWRON line low; a BSS138 inverts it onto GPIO 3 (SYS_OUT,
// 10K pull-up): PWRON idles high, so the FET conducts and GPIO 3 reads LOW at rest and HIGH while
// the key is down (schematic; see docs/hardware.md). Holding it for the PMIC's power-off time
// switches the board off.
#define PIN_KEY2 3
#define KEY2_ACTIVE_LEVEL HIGH

// I2C bus shared by the touch controller (0x5A), the IMU (0x6B), the PMIC (0x34) and the codec (0x18)
#define PIN_I2C_SDA 15
#define PIN_I2C_SCL 14
#define TOUCH_DRIVER_CST92XX 1
#define PIN_TP_RST 2
#define PIN_TP_INT 11
#define TOUCH_I2C_ADDR 0x5A
// The CST9217 reports panel coordinates once SensorLib mirrors both axes (as Waveshare does), so
// the calibration starts at 1:1. Still to be measured with crosses as on the other boards.
#define TOUCH_X_GAIN 1.0f
#define TOUCH_X_OFFSET 0
#define TOUCH_Y_GAIN 1.0f
#define TOUCH_Y_OFFSET 0
#define IMU_I2C_ADDR 0x6B  // SA0 tied to GND
// QMI_INT1 goes to GPIO 21 in the schematic (the pin table says 38, the QSPI clock); polled, unused.
#define PMIC_AXP2101 1
#define PMIC_I2C_ADDR 0x34
#define PIN_PMIC_IRQ -1  // AXP_IRQ is not routed to a GPIO

// Audio: ES8311 codec (I2C 0x18) fed over I2S, NS4150B amplifier enabled by PA_CTRL.
#define AUDIO_ES8311 1
#define CODEC_I2C_ADDR 0x18
#define PIN_I2S_MCLK 16
#define PIN_I2S_BCLK 9
#define PIN_I2S_LRCK 45
#define PIN_I2S_DOUT 8   // DSDIN on the codec: ESP32 -> DAC
#define PIN_I2S_DIN 10   // ASDOUT on the codec: microphones -> ESP32
#define PIN_PA_CTRL 46   // HIGH powers the speaker amplifier

// Panel geometry. Waveshare's examples pass offsets (6, 0, 0, 0) to Arduino_CO5300.
#define LCD_NATIVE_W 466
#define LCD_NATIVE_H 466
#define LCD_COL_OFFSET 6
#define LCD_ROW_OFFSET 0
#define LCD_COL_OFFSET2 0
#define LCD_ROW_OFFSET2 0
#define LCD_CORNER_RADIUS 233  // round glass
#define LCD_DEFAULT_ROTATION 0

#define HAS_RGB_LED 0
#define HAS_BUZZER 1   // the speaker: sound.cpp drives it through the codec instead of a PWM pin
#define HAS_TOUCH 1
#define HAS_IMU 1
#define HAS_BATTERY 1  // read from the AXP2101 fuel gauge, not an ADC divider
#define HAS_POWER_LATCH 0
#define HAS_KEY2 1
#define BOARD_PSRAM_MB 8

// The speaker is the only sound output: "pin" in the sound settings is the I2S data line.
#define PIN_BUZZER PIN_I2S_DOUT
#define BUZZER_DEFAULT_PIN PIN_I2S_DOUT
#define BUZZER_PINS {PIN_I2S_DOUT}
// No status LED on board. GPIO 17, 18 and 39-42 are free on test pads for an external WS2812.
#define RGB_LED_DEFAULT_PIN 17
#define RGB_LED_PINS {17, 18, 39, 40, 41, 42}
