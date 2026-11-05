#pragma once
// LCD bring-up shared by every app on this base: the bus and panel driver picked by the board
// header (ST7789 on SPI by default, GC9A01 with LCD_DRIVER_GC9A01, CO5300 AMOLED on QSPI with
// LCD_DRIVER_CO5300), a full-screen canvas in the requested rotation and the brightness (PWM
// backlight, or the panel's brightness command on an AMOLED). What gets drawn stays in the app, which draws on the
// canvas returned by lcdBegin() and calls flush() on it.
#include <Arduino.h>
#include <Arduino_GFX_Library.h>

// Brings the panel up in `rotation` (0-3) with the backlight off, so the power-on garbage never
// shows. Returns the canvas, or nullptr when the panel or its framebuffer fails; the app turns
// the backlight on once it has drawn the first frame.
Arduino_Canvas *lcdBegin(uint8_t rotation);
Arduino_Canvas *lcdCanvas();       // nullptr before lcdBegin() or after a failed one
int16_t lcdWidth();                // canvas size in the current rotation
int16_t lcdHeight();
uint8_t lcdRotation();
bool lcdPortrait();                // rotations 0 and 2
// Backlight PWM duty 0-255 on LEDC channel 0 (timer 0); the buzzer keeps off that timer. On an
// AMOLED the same 0-255 goes to the panel's brightness command (0 = dark).
void lcdSetBacklight(uint8_t duty);
uint8_t lcdBacklight();            // last duty requested
uint32_t lcdBacklightDuty();       // duty the LEDC actually outputs (AMOLED: last level sent), for diagnostics
// Sends only a rectangle of the canvas to the panel (clipped; widened to even coordinates where the
// controller needs it), for apps that know which part of the frame changed.
void lcdFlushRect(int16_t x, int16_t y, int16_t w, int16_t h);
// Sends w x h contiguous pixels (row by row) to that window of the panel, bypassing the canvas. The
// caller keeps the window inside the screen (and on even coordinates where lcdFlushRect aligns).
void lcdPushRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t *pixels);
// The same with the pixels already byte-swapped (big-endian, as the panel takes them) in DMA-capable
// internal RAM: the bus sends the buffer as it is, which is about twice as fast.
void lcdPushRectBE(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t *pixels);
// Horizontal pixels hidden by a rounded corner of the glass at `dist` px from the top or bottom
// edge (0 on square glass): keeps text and bars clear of the bezel.
int16_t lcdEdgeInset(int16_t dist);
// Dumps the framebuffer for tools/screenshot.py: "SHOT w h", base64 of the RGB565 bytes
// (little-endian, as in memory) in 76-char lines, then "ENDSHOT".
void lcdShot(Print &out);
