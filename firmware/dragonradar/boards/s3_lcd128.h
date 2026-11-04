#pragma once
// Waveshare ESP32-S3-Touch-LCD-1.28 (and the non-touch ESP32-S3-LCD-1.28): GC9A01 240x240 round
// panel, ESP32-S3R2 (2 MB quad PSRAM), 16 MB flash, QMI8658 IMU and CST816S touch on I2C, battery
// charger with ADC divider, USB through a CH343 bridge. The target is the same as the 1.69 board,
// so board.h picks this header only when BOARD_S3_LCD128 is defined (BOARD=round in the Makefiles
// that offer it).
#define BOARD_NAME "Waveshare ESP32-S3-Touch-LCD-1.28"
#define BOARD_ID "s3-lcd128"
#define BOARD_SHORT "round"  // BOARD= in the Makefile

#define LCD_DRIVER_GC9A01 1
#define PIN_LCD_MOSI 11
#define PIN_LCD_SCLK 10
#define PIN_LCD_CS 9
#define PIN_LCD_DC 8
#define PIN_LCD_RST 14
#define PIN_LCD_BL 2
#define PIN_BOOT_BTN 0  // BOOT key, strapping pin, usable as a button after boot

// I2C bus shared by the IMU and the touch controller.
#define PIN_I2C_SDA 6
#define PIN_I2C_SCL 7
#define IMU_I2C_ADDR 0x6B
// The sensor sits rotated 180 degrees compared with the 1.69 board (upright, raw X reads -1 g).
#define IMU_NATIVE_AXES float nx = ay, ny = -ax
// Touch pins from the Waveshare wiki, not verified on hardware: HAS_TOUCH stays 0 until the
// pins and the calibration are measured with 'tcal'.
#define PIN_TP_RST 13
#define PIN_TP_INT 5
#define TOUCH_I2C_ADDR 0x15

#define PIN_BAT_ADC 1
#define BATTERY_DIVIDER 3  // 1:3 divider

#define LCD_NATIVE_W 240
#define LCD_NATIVE_H 240
#define LCD_COL_OFFSET 0
#define LCD_ROW_OFFSET 0
#define LCD_CORNER_RADIUS 120  // round glass
#define LCD_DEFAULT_ROTATION 0

#define HAS_RGB_LED 0
#define HAS_BUZZER 0
#define HAS_TOUCH 0
#define HAS_IMU 1
#define HAS_BATTERY 1
#define HAS_POWER_LATCH 0  // no power latch on this board
#define BOARD_PSRAM_MB 2

// External outputs: no status LED or buzzer on board, both can be wired to the header GPIOs (off
// until enabled). The quad PSRAM and flash use GPIO 26-32, so none of those are offered.
#define RGB_LED_DEFAULT_PIN 16
#define RGB_LED_PINS {16, 15, 17, 18, 21, 33}
#define BUZZER_DEFAULT_PIN 17
#define BUZZER_PINS {17, 15, 16, 18, 21, 33}
