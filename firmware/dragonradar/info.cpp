#include "info.h"
#include "config.h"
#include "lcd.h"  // Arduino_GFX (GFXfont) before the vendored fonts
#include "devsettings.h"
#include "fonts/FreeSans12pt7b.h"
#include "fonts/FreeSans9pt7b.h"
#include "fonts/FreeSansBold18pt7b.h"
#include "hw.h"
#include "lcd.h"
#include "net.h"
#include "radar.h"

static bool showing = false, fullCard = false;
static uint32_t shownAt = 0, showFor = 0, lastRefresh = 0;
static String lastKey;

static uint16_t rgb565(uint32_t c) { return ((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0) | ((c >> 3) & 0x001F); }

// The text of the card, one entry per line (empty strings are skipped): what it shows, and the key
// that tells a refresh whether anything changed.
struct Line {
  String text;
  const GFXfont *font;
  uint8_t color;  // 0 text, 1 accent, 2 dim
  int16_t gapBefore;
};

static int buildLines(Line *out, bool full) {
  int n = 0;
  out[n++] = {"Dragon Radar", &FreeSansBold18pt7b, 1, 0};
  out[n++] = {String("v") + FW_VERSION, &FreeSans12pt7b, 0, 6};
  out[n++] = {BOARD_NAME, &FreeSans9pt7b, 2, 4};
  if (!full) return n;
  if (!netWifiEnabled()) {
    out[n++] = {"WiFi off", &FreeSans12pt7b, 2, 22};
  } else if (netConnected()) {
    out[n++] = {"WiFi " + netSsid() + "  " + String(netRssi()) + " dBm", &FreeSans12pt7b, 0, 22};
    out[n++] = {"http://" + netIp() + "/", &FreeSans12pt7b, 0, 6};
    out[n++] = {"http://" + devHostname() + ".local/", &FreeSans12pt7b, 0, 6};
  } else if (netApMode()) {
    out[n++] = {"Setup " + netApSsid() + ", open", &FreeSans12pt7b, 0, 22};
    out[n++] = {"http://" + netIp() + "/", &FreeSans12pt7b, 0, 6};
    out[n++] = {"http://" + devHostname() + ".local/", &FreeSans12pt7b, 2, 6};
  } else {
    out[n++] = {"WiFi connecting...", &FreeSans12pt7b, 2, 22};
  }
  if (hwHasBattery() && hwBatteryPct() >= 0) {
    char b[48];
    const int mv = hwBatteryMv();
    snprintf(b, sizeof(b), "Battery %d%%  %d.%02d V%s", hwBatteryPct(), mv / 1000, (mv % 1000) / 10,
             hwBatteryCharging() ? "  charging" : "");
    out[n++] = {b, &FreeSans12pt7b, 0, 14};
  }
  out[n++] = {"adrianmb.dev", &FreeSans12pt7b, 1, 22};
  out[n++] = {"tap to close", &FreeSans9pt7b, 2, 10};
  return n;
}

static void draw() {
  Arduino_Canvas *c = lcdCanvas();
  if (!c) return;
  Line lines[12];
  const int n = buildLines(lines, fullCard);
  uint32_t text, accent, dim;
  radarCardColors(text, accent, dim);
  const uint16_t colors[3] = {rgb565(text), rgb565(accent), rgb565(dim)};
  radarCardBackground(c->getFramebuffer(), 0.42f);
  const int16_t W = lcdWidth(), H = lcdHeight();
  // Heights first, to centre the block vertically on the round glass.
  int16_t heights[12], total = 0;
  for (int i = 0; i < n; i++) {
    int16_t x1, y1;
    uint16_t w, h;
    c->setFont(lines[i].font);
    c->getTextBounds("Hg", 0, 0, &x1, &y1, &w, &h);
    heights[i] = h;
    total += h + (i ? lines[i].gapBefore : 0);
  }
  int16_t y = (H - total) / 2;
  for (int i = 0; i < n; i++) {
    if (i) y += lines[i].gapBefore;
    int16_t x1, y1, bx1, by1;
    uint16_t w, h, bw, bh;
    c->setFont(lines[i].font);
    c->getTextBounds("Hg", 0, 0, &bx1, &by1, &bw, &bh);  // baseline offset of the line box
    c->getTextBounds(lines[i].text.c_str(), 0, 0, &x1, &y1, &w, &h);
    // Keep the line inside the circle: shrink nothing, but warn in the log if it would not fit.
    const int16_t mid = y + heights[i] / 2 - H / 2, half = (int16_t)sqrtf(max(0.0f, 225.0f * 225.0f - mid * mid));
    if ((int16_t)w > 2 * half) Serial.printf("[info] line '%s' is wider than the glass at y=%d\n", lines[i].text.c_str(), y);
    c->setTextColor(colors[lines[i].color]);
    c->setCursor((W - (int16_t)w) / 2 - x1, y - by1);
    c->print(lines[i].text);
    y += heights[i];
  }
  c->setFont(nullptr);
  lcdFlushRect(0, 0, W, H);
}

static String contentKey() {
  Line lines[12];
  const int n = buildLines(lines, fullCard);
  String k;
  for (int i = 0; i < n; i++) k += lines[i].text + "|";
  return k;
}

void infoShow(bool full, uint32_t ms) {
  if (!radarIsOn()) return;  // the screen is dark: nothing to show it on
  fullCard = full;
  showing = true;
  shownAt = millis();
  showFor = ms;
  radarSuspend(true);
  lastKey = contentKey();
  lastRefresh = shownAt;
  draw();
  Serial.printf("[info] shown (%s card, %u ms)\n", full ? "full" : "short", (unsigned)ms);
}

void infoHide() {
  if (!showing) return;
  showing = false;
  radarSuspend(false);
  Serial.printf("[info] closed after %u ms\n", (unsigned)(millis() - shownAt));
}

bool infoShowing() { return showing; }

void infoLoop() {
  if (!showing) return;
  const uint32_t now = millis();
  if (now - shownAt >= showFor) {
    infoHide();
    return;
  }
  if (now - lastRefresh >= 1000) {  // RSSI, battery, a WiFi state change
    lastRefresh = now;
    String k = contentKey();
    if (k != lastKey) {
      lastKey = k;
      draw();
    }
  }
}
