#include "lcd.h"
#include "config.h"

static const uint32_t LCD_SPI_HZ = 80000000;
static const uint8_t BL_CHANNEL = 0;  // channels 2n and 2n+1 share a timer: the buzzer takes channel 2
static const uint32_t BL_FREQ_HZ = 5000;

static Arduino_DataBus *bus = nullptr;
static Arduino_GFX *panel = nullptr;
static Arduino_TFT *tft = nullptr;  // the same panel, for address windows (lcdFlushRect)
#if defined(LCD_DRIVER_CO5300) && LCD_DRIVER_CO5300
static Arduino_CO5300 *amoled = nullptr;  // brightness is a panel command, not a PWM backlight
#endif
static Arduino_Canvas *canvas = nullptr;
static int16_t W = LCD_NATIVE_H, H = LCD_NATIVE_W;
static uint8_t rot = LCD_DEFAULT_ROTATION;
static uint8_t blLevel = 0;

Arduino_Canvas *lcdBegin(uint8_t rotation) {
  rot = rotation & 3;
  W = (rot & 1) ? LCD_NATIVE_H : LCD_NATIVE_W;
  H = (rot & 1) ? LCD_NATIVE_W : LCD_NATIVE_H;
#if defined(LCD_DRIVER_CO5300) && LCD_DRIVER_CO5300
  // AMOLED on QSPI. The rotation is applied by the panel's MADCTL, so the canvas is W x H as usual.
  bus = new Arduino_ESP32QSPI(PIN_LCD_CS, PIN_LCD_SCLK, PIN_LCD_SDIO0, PIN_LCD_SDIO1, PIN_LCD_SDIO2, PIN_LCD_SDIO3);
  amoled = new Arduino_CO5300(bus, PIN_LCD_RST, rot, LCD_NATIVE_W, LCD_NATIVE_H, LCD_COL_OFFSET, LCD_ROW_OFFSET,
                              LCD_COL_OFFSET2, LCD_ROW_OFFSET2);
  panel = tft = amoled;
  Arduino_Canvas *c = new Arduino_Canvas(W, H, panel, 0, 0, 0);
#ifndef LCD_QSPI_HZ
#define LCD_QSPI_HZ 40000000
#endif
  // The framebuffer (W x H x 2 bytes, 434 KB at 466x466) only fits in PSRAM.
  if (!c->begin(LCD_QSPI_HZ)) {
    Serial.println("[lcd] panel init failed");
    return nullptr;
  }
  lcdSetBacklight(0);  // dark until the app has drawn its first frame
#else
  bus = new Arduino_ESP32SPI(PIN_LCD_DC, PIN_LCD_CS, PIN_LCD_SCLK, PIN_LCD_MOSI, GFX_NOT_DEFINED, FSPI);
#if defined(LCD_DRIVER_GC9A01) && LCD_DRIVER_GC9A01
  panel = tft = new Arduino_GC9A01(bus, PIN_LCD_RST, rot, true, LCD_NATIVE_W, LCD_NATIVE_H);
#else
  panel = tft = new Arduino_ST7789(bus, PIN_LCD_RST, rot, true, LCD_NATIVE_W, LCD_NATIVE_H, LCD_COL_OFFSET,
                                   LCD_ROW_OFFSET, LCD_COL_OFFSET2, LCD_ROW_OFFSET2);
#endif
  Arduino_Canvas *c = new Arduino_Canvas(W, H, panel, 0, 0, 0);
  // Backlight attached and dark before the panel initialises, so nothing half-drawn shows.
  if (!ledcAttachChannel(PIN_LCD_BL, BL_FREQ_HZ, 8, BL_CHANNEL)) Serial.println("[lcd] backlight PWM attach failed");
  lcdSetBacklight(0);
  if (!c->begin(LCD_SPI_HZ)) {
    // No panel or no framebuffer: the app gets nullptr and skips drawing instead of crashing.
    Serial.println("[lcd] panel init failed");
    return nullptr;
  }
#ifdef LCD_PANEL_TUNING
  // Panel-specific registers (voltages, VCOM, gamma) from the board header, sent after the generic init.
  static const uint8_t tuning[] = LCD_PANEL_TUNING;
  bus->beginWrite();
  for (size_t i = 0; i + 1 < sizeof(tuning);) {
    uint8_t cmd = tuning[i++], n = tuning[i++];
    bus->writeCommand(cmd);
    for (uint8_t k = 0; k < n && i < sizeof(tuning); k++) bus->write(tuning[i++]);
  }
  bus->endWrite();
#endif
#endif
  canvas = c;
  canvas->fillScreen(0);
  canvas->flush();
  return canvas;
}

Arduino_Canvas *lcdCanvas() { return canvas; }
int16_t lcdWidth() { return W; }
int16_t lcdHeight() { return H; }
uint8_t lcdRotation() { return rot; }
bool lcdPortrait() { return (rot & 1) == 0; }

void lcdSetBacklight(uint8_t duty) {
  blLevel = duty;
#if defined(LCD_DRIVER_CO5300) && LCD_DRIVER_CO5300
  if (amoled) amoled->setBrightness(duty);
#else
  ledcWrite(PIN_LCD_BL, duty);
#endif
}

uint8_t lcdBacklight() { return blLevel; }
#if defined(LCD_DRIVER_CO5300) && LCD_DRIVER_CO5300
uint32_t lcdBacklightDuty() { return amoled ? blLevel : 0; }  // no readback: the last value sent to the panel
#else
uint32_t lcdBacklightDuty() { return ledcRead(PIN_LCD_BL); }
#endif

// Pixels per strip of a partial flush: rows of the rectangle are gathered from the framebuffer
// (PSRAM) into an internal buffer so the bus sends them in a few long transactions.
#define LCD_STRIP_PX 8192

void lcdFlushRect(int16_t x, int16_t y, int16_t w, int16_t h) {
  if (!canvas || !tft) return;
  int16_t x0 = max<int16_t>(0, x), y0 = max<int16_t>(0, y);
  int16_t x1 = min<int16_t>(W, x + w), y1 = min<int16_t>(H, y + h);
#if defined(LCD_DRIVER_CO5300) && LCD_DRIVER_CO5300
  // The CO5300 takes windows that start on an even column and row and span an even size.
  x0 &= ~1;
  y0 &= ~1;
  x1 = min<int16_t>(W, (x1 + 1) & ~1);
  y1 = min<int16_t>(H, (y1 + 1) & ~1);
#endif
  if (x1 <= x0 || y1 <= y0) return;
  uint16_t *fb = canvas->getFramebuffer();
  const int16_t rw = x1 - x0, rh = y1 - y0;
  if (rw == W) {  // whole rows are contiguous in the framebuffer
    tft->draw16bitRGBBitmap(0, y0, fb + (int32_t)y0 * W, W, rh);
    return;
  }
  static uint16_t *strip = nullptr;
  if (!strip) strip = (uint16_t *)heap_caps_malloc(LCD_STRIP_PX * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!strip) {
    tft->draw16bitRGBBitmap(0, y0, fb + (int32_t)y0 * W, W, rh);  // no buffer: whole rows
    return;
  }
  const int16_t rowsPerStrip = max(1, LCD_STRIP_PX / rw);
  tft->startWrite();
  tft->writeAddrWindow(x0, y0, rw, rh);
  for (int16_t r = 0; r < rh;) {
    int16_t n = min<int16_t>(rowsPerStrip, rh - r);
    for (int16_t k = 0; k < n; k++) memcpy(strip + k * rw, fb + (int32_t)(y0 + r + k) * W + x0, rw * 2);
    bus->writePixels(strip, (uint32_t)n * rw);
    r += n;
  }
  tft->endWrite();
}

void lcdPushRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t *pixels) {
  if (!tft || w <= 0 || h <= 0) return;
  tft->startWrite();
  tft->writeAddrWindow(x, y, w, h);
  bus->writePixels(pixels, (uint32_t)w * h);
  tft->endWrite();
}

void lcdPushRectBE(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t *pixels) {
  if (!tft || w <= 0 || h <= 0) return;
  tft->startWrite();
  tft->writeAddrWindow(x, y, w, h);
  bus->writeBytes((uint8_t *)pixels, (uint32_t)w * h * 2);  // no copy: DMA straight from `pixels`
  tft->endWrite();
}

int16_t lcdEdgeInset(int16_t dist) {
  const int32_t r = LCD_CORNER_RADIUS;
  if (!r || dist >= r) return 0;
  if (dist < 0) dist = 0;
  int32_t dy = r - dist;
  return (int16_t)(r - (int32_t)sqrtf((float)(r * r - dy * dy)));
}

void lcdShot(Print &out) {
  if (!canvas) return;
  static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  const uint8_t *p = (const uint8_t *)canvas->getFramebuffer();
  size_t n = (size_t)W * H * 2;
  out.printf("SHOT %d %d\n", W, H);
  char line[77];
  uint8_t col = 0;
  for (size_t i = 0; i < n; i += 3) {
    uint32_t v = (uint32_t)p[i] << 16 | (i + 1 < n ? p[i + 1] : 0) << 8 | (i + 2 < n ? p[i + 2] : 0);
    line[col++] = B64[(v >> 18) & 63];
    line[col++] = B64[(v >> 12) & 63];
    line[col++] = i + 1 < n ? B64[(v >> 6) & 63] : '=';
    line[col++] = i + 2 < n ? B64[v & 63] : '=';
    if (col >= 76) {
      line[col] = 0;
      out.println(line);
      col = 0;
    }
  }
  if (col) {
    line[col] = 0;
    out.println(line);
  }
  out.println("ENDSHOT");
}
