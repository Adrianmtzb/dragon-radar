#pragma once
// Waveshare ESP32-S3-Touch-LCD-1.69 (pinout from https://docs.waveshare.com/ESP32-S3-Touch-LCD-1.69):
// ST7789V2 240x280 rounded glass, 8 MB octal PSRAM, CST816T touch and QMI8658C IMU on I2C, buzzer,
// battery with power latch. Selected by board.h when compiling for the esp32s3 target.
//
// Two hardware revisions exist. The GPIOs below are the current one (buzzer 42, PWR on 40/41,
// RTC INT 39). The first batch wired the buzzer to 33, SYS_EN to 35, SYS_OUT to 36 and RTC INT to
// 41; define BOARD_S3_TOUCH169_V1 to build for it. Not offered as a Makefile board yet: GPIO 33-37
// belong to the octal PSRAM (PSRAM=opi), so those pins need checking on a V1 board first.
#define BOARD_NAME "Waveshare ESP32-S3-Touch-LCD-1.69"
#define BOARD_ID "s3-touch169"
#define BOARD_SHORT "s3"  // BOARD= in the Makefile

#define PIN_LCD_MOSI 7  // "DIN" on the wiki
#define PIN_LCD_SCLK 6
#define PIN_LCD_CS 5
#define PIN_LCD_DC 4
#define PIN_LCD_RST 8
#define PIN_LCD_BL 15
#define PIN_BOOT_BTN 0  // BOOT key, strapping pin, usable as a button after boot

// I2C bus shared by the touch controller (0x15), the IMU QMI8658 (0x6B) and the RTC PCF85063 (0x51)
#define PIN_I2C_SDA 11
#define PIN_I2C_SCL 10
#define PIN_TP_RST 13
#define PIN_TP_INT 14
#define TOUCH_I2C_ADDR 0x15
#define IMU_I2C_ADDR 0x6B
// Touch to panel calibration (panel = raw * gain + offset, native portrait coordinates), measured
// with the 'tcal' CLI command in rotations 0 and 2 and averaged, which cancels the ~10 px
// "finger lands low and right" bias that flips sign between the two. X is 1:1, Y is stretched
// by 1.2 (raw 0 lands on panel row 29, raw 279 on row 261).
#define TOUCH_X_GAIN 1.0f
#define TOUCH_X_OFFSET -3
#define TOUCH_Y_GAIN 0.83f
#define TOUCH_Y_OFFSET 29

#ifdef BOARD_S3_TOUCH169_V1
#define PIN_BUZZER 33
#define PIN_SYS_EN 35   // keep HIGH to stay powered from the battery, LOW cuts the rail
#define PIN_SYS_OUT 36  // PWR key state: HIGH at rest, LOW while pressed
#else
#define PIN_BUZZER 42
#define PIN_SYS_EN 41
#define PIN_SYS_OUT 40
#endif
#define PIN_BAT_ADC 1  // VBAT = 3 x VADC (two-resistor divider)
#define BATTERY_DIVIDER 3
// no VBUS sense on this board: the ETA6098 charger has no status line routed to the ESP32, so
// "charging" is guessed from the battery voltage trend (see hw.cpp)

// Panel: ST7789V2, 240x280 native (rounded corners), 20-row offset inside the 240x320 RAM.
#define LCD_NATIVE_W 240
#define LCD_NATIVE_H 280
#define LCD_COL_OFFSET 0
#define LCD_ROW_OFFSET 20   // same 20-row offset in every rotation (0 and 40 were tried on hardware: both wrong)
#define LCD_CORNER_RADIUS 48  // measured with the 'corners' CLI test: the 8 px mark hides, 16 px shows
#define LCD_DEFAULT_ROTATION 1

#define HAS_RGB_LED 0
#define HAS_BUZZER 1
#define HAS_TOUCH 1
#define HAS_IMU 1
#define HAS_BATTERY 1
#define HAS_POWER_LATCH 1
#define BOARD_PSRAM_MB 8

// External outputs: no status LED on board, an external WS2812 can be wired to a header GPIO (off
// until enabled). The buzzer defaults to the on-board one. GPIO 33-37 are not offered: the octal
// PSRAM uses them.
#define RGB_LED_DEFAULT_PIN 18
#define RGB_LED_PINS {18, 17, 2, 3}
#define BUZZER_DEFAULT_PIN PIN_BUZZER
#define BUZZER_PINS {PIN_BUZZER, 18, 17, 2, 3}
