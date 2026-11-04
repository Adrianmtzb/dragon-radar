#pragma once
// Waveshare ESP32-C6-LCD-1.47 (pinout from the official wiki + schematic): ST7789 172x320, no
// PSRAM, WS2812 status LED, no IMU. Selected by board.h when compiling for the esp32c6 target.
#define BOARD_NAME "Waveshare ESP32-C6-LCD-1.47"
#define BOARD_ID "c6-lcd147"
#define BOARD_SHORT "c6"  // BOARD= in the Makefile

#define PIN_LCD_MOSI 6
#define PIN_LCD_SCLK 7
#define PIN_LCD_CS 14
#define PIN_LCD_DC 15
#define PIN_LCD_RST 21
#define PIN_LCD_BL 22
#define PIN_SD_MISO 5   // microSD shares the SPI bus with the LCD
#define PIN_SD_CS 4
#define PIN_RGB_LED 8   // WS2812B
#define PIN_BOOT_BTN 9  // "BOOT" key, active low, 10K pull-up on board

// Panel: ST7789, 172x320 native, column offset 34 (the controller RAM is 240 wide).
#define LCD_NATIVE_W 172
#define LCD_NATIVE_H 320
#define LCD_COL_OFFSET 34
#define LCD_ROW_OFFSET 0
#define LCD_CORNER_RADIUS 0  // square glass
#define LCD_DEFAULT_ROTATION 1
// Waveshare's own init values for this 1.47" panel: porch, gate/VCOM/VRH voltages and gamma, as
// {command, data length, data...}. With only the generic ST7789 init, red and green sink in light
// tones (a light lavender shows as deep blue). Sent by lcdBegin() after the driver init.
// This panel's white still leans cold next to the S3's, and the analog gamma above is shared by
// all three channels: the ST7789 digital gamma table for blue (E3h, 64 steps, identity = 4 * step)
// scales blue alone to LCD_BLUE_PCT percent. Raise it toward 100 for a cooler white.
#define LCD_BLUE_PCT 86
#define LCD_B(i) (uint8_t)((i) * 4 * LCD_BLUE_PCT / 100)
#define LCD_PANEL_TUNING {                                                                           \
    0xB2, 5, 0x0C, 0x0C, 0x00, 0x33, 0x33,                                                           \
    0xB7, 1, 0x35, 0xBB, 1, 0x35, 0xC0, 1, 0x2C, 0xC2, 1, 0x01, 0xC3, 1, 0x13, 0xC4, 1, 0x20,        \
    0xC6, 1, 0x0F, 0xD0, 2, 0xA4, 0xA1, 0xD6, 1, 0xA1,                                               \
    0xE0, 14, 0xF0, 0x00, 0x04, 0x04, 0x04, 0x05, 0x29, 0x33, 0x3E, 0x38, 0x12, 0x12, 0x28, 0x30,   \
    0xE1, 14, 0xF0, 0x07, 0x0A, 0x0D, 0x0B, 0x07, 0x28, 0x33, 0x3E, 0x36, 0x14, 0x14, 0x29, 0x32,   \
    0xBA, 1, 0x04, /* digital gamma on: the blue look-up table below warms the white */             \
    0xE3, 64,                                                                                          \
    LCD_B(0), LCD_B(1), LCD_B(2), LCD_B(3), LCD_B(4), LCD_B(5), LCD_B(6), LCD_B(7), \
    LCD_B(8), LCD_B(9), LCD_B(10), LCD_B(11), LCD_B(12), LCD_B(13), LCD_B(14), LCD_B(15), \
    LCD_B(16), LCD_B(17), LCD_B(18), LCD_B(19), LCD_B(20), LCD_B(21), LCD_B(22), LCD_B(23), \
    LCD_B(24), LCD_B(25), LCD_B(26), LCD_B(27), LCD_B(28), LCD_B(29), LCD_B(30), LCD_B(31), \
    LCD_B(32), LCD_B(33), LCD_B(34), LCD_B(35), LCD_B(36), LCD_B(37), LCD_B(38), LCD_B(39), \
    LCD_B(40), LCD_B(41), LCD_B(42), LCD_B(43), LCD_B(44), LCD_B(45), LCD_B(46), LCD_B(47), \
    LCD_B(48), LCD_B(49), LCD_B(50), LCD_B(51), LCD_B(52), LCD_B(53), LCD_B(54), LCD_B(55), \
    LCD_B(56), LCD_B(57), LCD_B(58), LCD_B(59), LCD_B(60), LCD_B(61), LCD_B(62), LCD_B(63)}

#define HAS_RGB_LED 1
#define HAS_BUZZER 0
#define HAS_TOUCH 0
#define HAS_IMU 0
#define HAS_BATTERY 0
#define HAS_POWER_LATCH 0
#define BOARD_PSRAM_MB 0

// External outputs: the status LED defaults to the on-board WS2812; a passive buzzer can be wired
// to a free header GPIO (off until enabled). Allowed pins: the free GPIOs of the board.
#define RGB_LED_DEFAULT_PIN PIN_RGB_LED
#define RGB_LED_PINS {8, 18, 19, 20, 23, 0, 1, 2, 3}
#define BUZZER_DEFAULT_PIN 18
#define BUZZER_PINS {18, 19, 20, 23, 0, 1, 2, 3}
