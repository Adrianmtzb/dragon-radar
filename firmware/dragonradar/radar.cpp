// The per-pixel loops below run several times faster at -O3 than at the core's default -Os.
#pragma GCC optimize("O3")
#include "radar.h"
#include <Preferences.h>
#include <esp_heap_caps.h>
#include <math.h>
#include "config.h"
#include "devsettings.h"
#include "lcd.h"
#include "motion.h"
#include "net.h"
#include "radar_assets.h"
#include "sound.h"
#include "sounds.h"

// ====================================================================== geometry and constants
// Canvas coordinates as in the preview: origin top-left, pixel (i, j) covers [i, i+1) x [j, j+1),
// the screen centre is (CX, CY) and R is the screen radius. Radar units are fractions of R.

static const int W = LCD_NATIVE_W, H = LCD_NATIVE_H;
static const float CX = W / 2.0f, CY = H / 2.0f, R = W / 2.0f;
static const int MAX_BALLS = 7;
// Heading = YAW_SIGN x gyro yaw. The QMI8658 Z axis points out of the screen (it reads +1 g with the
// board lying face up), so turning the board clockwise seen from above is a negative yaw rate.
static const float YAW_SIGN = -1.0f;
static const uint32_t FRAME_MIN_MS = 16;  // render cap: ~60 fps
static const float FULL_FRAME_SHARE = 0.75f;  // dirty area above this share of the screen: redraw it all

struct Ball {
  uint8_t stars;
  float bearingDeg;
  float distanceM;
};

enum Power : uint8_t { P_ON, P_BOOT, P_POWERING_OFF, P_OFF };
static const char *const POWER_NAMES[] = {"on", "boot", "powering_off", "off"};

// ====================================================================== state

static Ball balls[MAX_BALLS];
static int ballCount = 0;
static bool manual = false;
static uint8_t zoom = RADAR_ZOOM_DEFAULT, theme = RADAR_THEME_DEFAULT;
static uint8_t sweep = RADAR_SWEEP_DEFAULT;
static uint32_t sweepEpoch = 0;  // the line starts pointing north at this time (end of the boot)
static int8_t zoomFrom = -1;  // level a zoom transition comes from, -1 when none
static uint32_t zoomStart = 0, labelAt = 0;
static bool labelShown = false;
static bool muted = false;
static uint8_t savedVolume = 255;
static Power power = P_OFF;
static uint32_t bootStart = 0, offStart = 0;
static uint32_t blinkAt = 0, lastBeep = 0;
static bool blinkEver = false, beepEver = false;
static bool closeArmed = true, foundArmed = true;
static bool beepsPaused = false;
static bool suspended = false;  // another screen has the panel (radarSuspend)
static uint32_t beepCount = 0;  // radar beeps since boot (CLI 'radar')
static float blinkFreeze = -1;    // debug: fixed blink brightness (CLI 'blink'), -1 = live
static float headingFreeze = -1;  // debug: fixed heading (CLI 'heading'), -1 = gyro
// Debug stills (CLI 'still'), to compare single frames with the preview's: a moment of the boot
// timeline, or a zoom transition at a given progress.
static uint8_t stillKind = 0;  // 0 live, 1 boot, 2 zoom
static float stillSweepDeg = -1;  // debug: fixed angle of the sweep line, -1 = live
static float stillRingP = -1;     // debug: fixed progress of the sweep ring, -1 = live
static int32_t stillBootT = 0;
static int stillFrom = 0;
static float stillP = 0;

// Render bookkeeping.
// PSRAM, per theme: gradient + grid + sheen, and gradient + sheen; unclipped (the rim pass clips).
static uint16_t *bgGridT[RADAR_THEME_COUNT], *bgPlainT[RADAR_THEME_COUNT];
static int bgRows[RADAR_THEME_COUNT];  // rows built so far (H = ready)
static uint16_t *bgGrid = nullptr, *bgPlain = nullptr;  // the current theme's
static uint8_t *sheenMap = nullptr;                    // sheen alpha (0-255) over its bounding box
static int sheenX0 = 0, sheenY0 = 0, sheenW = 0, sheenH = 0;
static bool fullPending = true;
static uint32_t lastFrameMs = 0;
static uint32_t bgKeyPrev = 0xFFFFFFFF;
static uint8_t lastBrightness = 255;
static bool offDrawn = false;

// Stats for the CLI.
static uint32_t statFrames = 0, statFull = 0, statWindowStart = 0, statWindowFrames = 0;
static float statFps = 0;
static uint32_t statComposeUs = 0, statFlushUs = 0, statPixels = 0;
static uint32_t statFullComposeUs = 0, statFullFlushUs = 0;          // last full frame
static uint32_t statFullFrameUs = 0, statFullMaxUs = 0;              // its wall time, and the worst one
static uint32_t statPrepUs = 0, statRects = 0;                       // describe + dirty pass of the last frame
static uint32_t statRenderSumUs = 0, statGapSumUs = 0, statLoopN = 0, statLastEndUs = 0;  // render() vs the rest
static uint32_t statPartSumUs = 0, statPartN = 0, statPartMaxUs = 0;  // partial frames since the last report
// Compose time by stage (background, grid, arrows, dots, marker, ping, label, rim): last frame and
// last full frame.
static uint32_t profUs[8], profFullUs[8];
static const char *const PROF_NAMES[8] = {"bg", "grid+sweep", "arrows", "dots", "marker", "ping", "label", "rim"};

// ====================================================================== small helpers

static inline float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
static inline float rampf(float t, float a, float b) { return b > a ? clamp01((t - a) / (b - a)) : (t >= b ? 1 : 0); }
static inline float easeOutCubic(float p) { return 1 - (1 - p) * (1 - p) * (1 - p); }
static inline float rad(float d) { return d * (float)M_PI / 180.0f; }
static inline float wrap360(float d) {
  d = fmodf(d, 360.0f);
  return d < 0 ? d + 360.0f : d;
}
static inline float wrapPi(float a) {  // to (-pi, pi]
  a = fmodf(a + (float)M_PI, 2 * (float)M_PI);
  if (a < 0) a += 2 * (float)M_PI;
  return a - (float)M_PI;
}

static const RadarPalette &pal() { return RADAR_THEMES[theme]; }
static float scaleOf(int level) { return RADAR_ZOOM[level].pitchR / RADAR_ZOOM[level].cellM; }  // R per metre

// RGB565 <-> 8-bit channels through tables (radarBegin fills them). The framebuffer holds native
// uint16 values (little-endian in memory).
static uint8_t TO5[256], TO6[256], FROM5[32], FROM6[64];

static void initTables() {
  for (int i = 0; i < 256; i++) {
    TO5[i] = (uint8_t)((i * 31 + 127) / 255);
    TO6[i] = (uint8_t)((i * 63 + 127) / 255);
  }
  for (int i = 0; i < 32; i++) FROM5[i] = (uint8_t)((i << 3) | (i >> 2));
  for (int i = 0; i < 64; i++) FROM6[i] = (uint8_t)((i << 2) | (i >> 4));
}

static inline void unpack(uint16_t c, int &r, int &g, int &b) {
  r = FROM5[c >> 11];
  g = FROM6[(c >> 5) & 63];
  b = FROM5[c & 31];
}
static inline uint16_t pack(int r, int g, int b) {
  r = r < 0 ? 0 : r > 255 ? 255 : r;
  g = g < 0 ? 0 : g > 255 ? 255 : g;
  b = b < 0 ? 0 : b > 255 ? 255 : b;
  return (uint16_t)(TO5[r] << 11 | TO6[g] << 5 | TO5[b]);
}
// 4x4 ordered dither for the smooth static layers (the radial gradient would band in RGB565).
static const uint8_t BAYER[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
static inline uint16_t packDither(float r, float g, float b, int x, int y) {
  float t = (BAYER[y & 3][x & 3] + 0.5f) / 16.0f;
  int r5 = (int)(r * (31.0f / 255.0f) + t), g6 = (int)(g * (63.0f / 255.0f) + t), b5 = (int)(b * (31.0f / 255.0f) + t);
  r5 = r5 < 0 ? 0 : r5 > 31 ? 31 : r5;
  g6 = g6 < 0 ? 0 : g6 > 63 ? 63 : g6;
  b5 = b5 < 0 ? 0 : b5 > 31 ? 31 : b5;
  return (uint16_t)(r5 << 11 | g6 << 5 | b5);
}

struct Rgb {
  float r, g, b;
};
static inline Rgb rgbOf(uint32_t c) { return {(float)((c >> 16) & 255), (float)((c >> 8) & 255), (float)(c & 255)}; }
static inline Rgb mixRgb(const Rgb &a, const Rgb &b, float t) {
  return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}

// Integer colour for the per-pixel blends.
struct RgbI {
  int r, g, b;
};
static inline RgbI toI(const Rgb &c) { return {(int)(c.r + 0.5f), (int)(c.g + 0.5f), (int)(c.b + 0.5f)}; }

// Alpha blend of `c` over the pixel, alpha 0-256.
static inline void blendI(uint16_t &px, const RgbI &c, int a) {
  if (a <= 0) return;
  if (a >= 256) {
    px = (uint16_t)(TO5[c.r] << 11 | TO6[c.g] << 5 | TO5[c.b]);
    return;
  }
  int r = FROM5[px >> 11], g = FROM6[(px >> 5) & 63], b = FROM5[px & 31];
  r += ((c.r - r) * a + 128) >> 8;
  g += ((c.g - g) * a + 128) >> 8;
  b += ((c.b - b) * a + 128) >> 8;
  px = (uint16_t)(TO5[r] << 11 | TO6[g] << 5 | TO5[b]);
}

// Fast RGB565 blend with a 5-bit alpha (0-32), for the zoom transition's grids: they last 280 ms
// and the old one fades out, so the coarser alpha does not show.
static inline uint16_t blend565(uint16_t dst, uint16_t src, uint32_t a5) {
  // Per channel in the 5/6-bit domain, rounded to nearest (truncating darkens every blend by half
  // a step).
  const int a = (int)a5;
  const int dr = dst >> 11, dg = (dst >> 5) & 63, db = dst & 31;
  const int r = dr + ((((src >> 11) - dr) * a + 16) >> 5);
  const int g = dg + (((((src >> 5) & 63) - dg) * a + 16) >> 5);
  const int b = db + ((((src & 31) - db) * a + 16) >> 5);
  return (uint16_t)(r << 11 | g << 5 | b);
}

// Alpha blend of `c` over the pixel, alpha 0-1.
static inline void blendPx(uint16_t &px, const Rgb &c, float a) {
  if (a <= 0.002f) return;
  blendI(px, toI(c), (int)(a * 256 + 0.5f));
}

// ====================================================================== clip rectangle

// Drawing target. Frames are composed strip by strip in internal RAM (the framebuffer lives in PSRAM,
// where scattered writes miss the cache): `fb` points at a virtual origin so that fb[y * fbStride + x]
// is pixel (x, y) of the current strip, whose stride is the width of the rectangle being drawn.
static uint16_t *fb = nullptr;
static int fbStride = W;
static uint16_t *canvasFb = nullptr;  // the canvas framebuffer: filled only for 'shot'
static const int STRIP_PX = 8192;
// Two strips: one is composed while the push task (core 0) sends the other to the panel.
static uint16_t stripBufs[2][STRIP_PX] __attribute__((aligned(16)));  // DMA source: internal, aligned
static uint16_t *stripBuf = stripBufs[0];
struct Rect {
  int x0, y0, x1, y1;  // half-open
  int area() const { return (x1 - x0) * (y1 - y0); }
  bool empty() const { return x1 <= x0 || y1 <= y0; }
};
static Rect clipR = {0, 0, W, H};

static inline bool clipBox(float fx0, float fy0, float fx1, float fy1, int &x0, int &y0, int &x1, int &y1) {
  x0 = max(clipR.x0, (int)floorf(fx0));
  y0 = max(clipR.y0, (int)floorf(fy0));
  x1 = min(clipR.x1, (int)ceilf(fx1));
  y1 = min(clipR.y1, (int)ceilf(fy1));
  return x1 > x0 && y1 > y0;
}

// ====================================================================== static layers

// Sheen of radar.json shading.sheen: an elliptical radial gradient of white, rotated, upper left.
static float sheenAlphaAt(float px, float py) {
#if RADAR_SHEEN_ENABLED
  const float th = rad(RADAR_SHEEN_ANGLE_DEG), ct = cosf(th), st = sinf(th);
  float dx = px - (CX + RADAR_SHEEN_CX_R * R), dy = py - (CY + RADAR_SHEEN_CY_R * R);
  float u = dx * ct + dy * st, v = (-dx * st + dy * ct) / (RADAR_SHEEN_RY_R / RADAR_SHEEN_RX_R);
  float d = sqrtf(u * u + v * v) / (RADAR_SHEEN_RX_R * R);
  return d >= 1 ? 0 : RADAR_SHEEN_ALPHA * (1 - d);
#else
  return 0;
#endif
}

static void buildSheenMap() {
#if RADAR_SHEEN_ENABLED
  const float th = rad(RADAR_SHEEN_ANGLE_DEG);
  const float rx = RADAR_SHEEN_RX_R * R, ry = RADAR_SHEEN_RY_R * R;
  const float hw = rx * fabsf(cosf(th)) + ry * fabsf(sinf(th)), hh = rx * fabsf(sinf(th)) + ry * fabsf(cosf(th));
  const float cx = CX + RADAR_SHEEN_CX_R * R, cy = CY + RADAR_SHEEN_CY_R * R;
  sheenX0 = max(0, (int)floorf(cx - hw) - 1);
  sheenY0 = max(0, (int)floorf(cy - hh) - 1);
  sheenW = min(W, (int)ceilf(cx + hw) + 1) - sheenX0;
  sheenH = min(H, (int)ceilf(cy + hh) + 1) - sheenY0;
  sheenMap = (uint8_t *)heap_caps_malloc(sheenW * sheenH, MALLOC_CAP_SPIRAM);
  if (!sheenMap) return;
  for (int y = 0; y < sheenH; y++)
    for (int x = 0; x < sheenW; x++)
      sheenMap[y * sheenW + x] = (uint8_t)lroundf(sheenAlphaAt(sheenX0 + x + 0.5f, sheenY0 + y + 0.5f) * 255.0f / RADAR_SHEEN_ALPHA);
#endif
}

static inline float sheenAt(int x, int y) {
  if (!sheenMap) return 0;
  x -= sheenX0;
  y -= sheenY0;
  if (x < 0 || y < 0 || x >= sheenW || y >= sheenH) return 0;
  return sheenMap[y * sheenW + x] * (RADAR_SHEEN_ALPHA / 255.0f);
}

// Coverage of pixel [i, i+1) by the grid lines of pitch `pitch` px and width `lw` px (one axis).
static void gridCoverage(float pitch, float lw, float *cov) {
  for (int i = 0; i < W; i++) cov[i] = 0;
  const float off = RADAR_GRID_OFFSET_CELLS * pitch, hw = lw / 2;
  const int n = (int)ceilf(R / pitch) + 1;
  for (int k = -n; k <= n; k++) {
    float v = k * pitch + off;
    if (fabsf(v) > R) continue;  // as the preview: lines beyond the radius are not drawn
    float b0 = CX + v - hw, b1 = CX + v + hw;
    for (int i = max(0, (int)floorf(b0)); i < min(W, (int)ceilf(b1)); i++) {
      float o = min((float)i + 1, b1) - max((float)i, b0);
      if (o > 0) cov[i] = min(1.0f, cov[i] + o);
    }
  }
}

static float gradT(float d) { return d >= R ? 1 : d / R; }

// Builds rows [y0, y1) of both backgrounds of theme `t`: gradient + sheen, and gradient + grid +
// sheen (the grid of every zoom level is the same on screen: only its meaning changes).
static void buildRows(int t, int y0, int y1) {
  if (!bgGridT[t]) bgGridT[t] = (uint16_t *)heap_caps_malloc(W * H * 2, MALLOC_CAP_SPIRAM);
  if (!bgPlainT[t]) bgPlainT[t] = (uint16_t *)heap_caps_malloc(W * H * 2, MALLOC_CAP_SPIRAM);
  if (!sheenMap) buildSheenMap();
  uint16_t *gp = bgGridT[t], *pp = bgPlainT[t];
  if (!gp || !pp) return;
  static float cov[LCD_NATIVE_W];
  static bool covReady = false;
  if (!covReady) {
    const RadarZoomLevel &L = RADAR_ZOOM[RADAR_ZOOM_DEFAULT];
    gridCoverage(L.pitchR * R, max(1.0f, L.lineWidthR * R), cov);
    covReady = true;
  }
  const RadarPalette &p = RADAR_THEMES[t];
  const Rgb c0 = rgbOf(p.screenCenter888), c1 = rgbOf(p.screen888), c2 = rgbOf(p.screenEdge888);
  const Rgb grid = rgbOf(p.grid888), white = rgbOf(p.sheen888);
  for (int y = y0; y < y1; y++) {
    float py = y + 0.5f - CY;
    for (int x = 0; x < W; x++) {
      float px = x + 0.5f - CX, tt = gradT(sqrtf(px * px + py * py));
      Rgb g = tt < RADAR_SHADE_MID_STOP ? mixRgb(c0, c1, tt / RADAR_SHADE_MID_STOP)
                                        : mixRgb(c1, c2, (tt - RADAR_SHADE_MID_STOP) / (1 - RADAR_SHADE_MID_STOP));
      float c = cov[x] + cov[y] - cov[x] * cov[y];  // union of the vertical and horizontal lines
      Rgb gg = c > 0 ? mixRgb(g, grid, c) : g;
      float sh = sheenAt(x, y);
      if (sh > 0) {
        g = mixRgb(g, white, sh);
        gg = mixRgb(gg, white, sh);
      }
      pp[y * W + x] = packDither(g.r, g.g, g.b, x, y);
      gp[y * W + x] = packDither(gg.r, gg.g, gg.b, x, y);
    }
  }
  bgRows[t] = max(bgRows[t], y1);
}

static void buildTheme(int t) {
  if (bgRows[t] >= H) return;
  uint32_t t0 = millis();
  buildRows(t, bgRows[t], H);
  Serial.printf("[radar] theme %s backgrounds in %u ms\n", RADAR_THEMES[t].name, (unsigned)(millis() - t0));
}

// Some rows of a theme not built yet, between frames, so a later theme change is instant.
static void buildIdle() {
  for (int t = 0; t < RADAR_THEME_COUNT; t++)
    if (bgRows[t] < H) {
      buildRows(t, bgRows[t], min(H, bgRows[t] + 6));
      return;
    }
}

static bool selectTheme() {
  if (bgRows[theme] < H) buildTheme(theme);
  bgGrid = bgGridT[theme];
  bgPlain = bgPlainT[theme];
  return bgGrid && bgPlain && bgRows[theme] >= H;
}

// ====================================================================== frame description

// Centres (R units, around the group centre) of the n spheres of a cluster: 1 alone, 2 side by
// side, 3-6 on a ring (point up when odd, flat top when even), 7 a ring of six around one.
static void clusterOffsets(int n, float *ox, float *oy) {
  const float s = RADAR_BALL_CLUSTER_SPACING_R;
  if (n <= 1) {
    ox[0] = oy[0] = 0;
    return;
  }
  int ring = n, k0 = 0;
  float rr = s / (2 * sinf((float)M_PI / n));
  if (n == 7) {
    ox[0] = oy[0] = 0;
    ring = 6;
    k0 = 1;
    rr = s;
  }
  const float start = -(float)M_PI / 2 + (ring % 2 ? 0 : (float)M_PI / ring);
  for (int k = 0; k < ring; k++) {
    const float a = start + 2 * (float)M_PI * k / ring;
    ox[k0 + k] = cosf(a) * rr;
    oy[k0 + k] = sinf(a) * rr;
  }
}

enum ElemType : uint8_t { E_ARROW, E_DOT, E_MARKER, E_PING, E_LABEL };

struct Elem {
  uint8_t type;
  uint8_t n;       // dot: ball count; label: zoom level
  uint8_t stars;   // dot: star number of its nearest ball
  float x, y;      // dot centre (px); arrow: angle (rad) in x
  float b, a;      // blink brightness, alpha
  float r;         // ping radius (px)
  Rect box;
  uint32_t sig;    // quantised parameters: equal sig and box = same pixels
};

static const int MAX_ELEMS = 24;

struct Frame {
  Power power;
  int32_t bootT, offT;  // -1 when not in that phase
  float scale;          // R per metre
  int grids;            // grids to draw: 1, or 2 during a zoom transition
  float gridPitch[2], gridAlpha[2];
  float gridR;          // boot reveal radius (R units); >= 1.5 means no clip
  bool transition;
  bool line;            // sweep line on, pointing at lineDeg (clockwise from up)
  float lineDeg;
  bool ring;            // sweep ring on, radius ringR px, alpha ringA
  float ringR, ringA;
  Elem el[MAX_ELEMS];
  int nEl;
};

static Frame cur, prev;
static bool prevValid = false;

static uint32_t hashf(uint32_t h, float v, float q) { return (h ^ (uint32_t)(int32_t)lroundf(v * q)) * 16777619u; }

static void boxAround(Elem &e, float x0, float y0, float x1, float y1) {
  e.box.x0 = max(0, (int)floorf(x0) - 1);
  e.box.y0 = max(0, (int)floorf(y0) - 1);
  e.box.x1 = min(W, (int)ceilf(x1) + 1);
  e.box.y1 = min(H, (int)ceilf(y1) + 1);
}

static float blinkLevel(uint32_t now) {
  if (blinkFreeze >= 0) return blinkFreeze;
  if (!blinkEver) return RADAR_BLINK_DIM_LEVEL;
  float e = (float)(now - blinkAt);
  const float dim = RADAR_BLINK_DIM_LEVEL;
  if (e < RADAR_BLINK_RISE_MS) return dim + (1 - dim) * (e / RADAR_BLINK_RISE_MS);
  return dim + (1 - dim) * expf(-(e - RADAR_BLINK_RISE_MS) / RADAR_BLINK_DECAY_MS);
}

// Brightness of a dot `e` ms after the sweep hand crossed it: the blink's rise and decay, with no
// floor (the beep blink keeps the dim level: the dot shows max of the two).
static float flashLevel(float e) {
  if (e < 0) return 0;
  if (e < RADAR_BLINK_RISE_MS) return e / RADAR_BLINK_RISE_MS;
  return expf(-(e - RADAR_BLINK_RISE_MS) / RADAR_BLINK_DECAY_MS);
}

// The hand's flash on something at screen angle `deg`, merged with the beep blink `b`.
static float withFlash(const Frame &f, float b, float deg) {
  if (!f.line || !RADAR_SWEEP_LINE_DOT_FLASH) return b;
  float since = wrap360(f.lineDeg - deg) / 360.0f * RADAR_SWEEP_LINE_PERIOD_MS;
  return max(b, roundf(flashLevel(since) * 128) / 128);
}

static float headingDeg() { return headingFreeze >= 0 ? headingFreeze : wrap360(YAW_SIGN * motionYaw()); }

static float textWidthPx(const char *s, float heightR) {
  float k = heightR * R / RADAR_FONT_BOX_H;
  int n = strlen(s);
  return n ? ((n - 1) * RADAR_FONT_ADVANCE + RADAR_FONT_BOX_W) * k : 0;
}

// Fills the frame description for `now`: phases, scale, grids and the dynamic elements.
static void describe(Frame &f, uint32_t now) {
  f.power = stillKind == 1 ? P_BOOT : power;
  f.bootT = stillKind == 1 ? stillBootT : power == P_BOOT ? (int32_t)(now - bootStart) : -1;
  f.offT = stillKind == 0 && power == P_POWERING_OFF ? (int32_t)(now - offStart) : -1;
  f.nEl = 0;
  f.transition = false;
  f.grids = 1;
  f.scale = scaleOf(zoom);
  f.gridPitch[0] = RADAR_ZOOM[zoom].pitchR * R;
  f.gridAlpha[0] = 1;
  const int from = stillKind == 2 ? stillFrom : zoomFrom;
  if (from >= 0) {
    float zp = stillKind == 2 ? stillP : clamp01((float)(now - zoomStart) / RADAR_ZOOM_ANIM_DURATION_MS);
    if (zp < 1) {
      float p = easeOutCubic(zp), sF = scaleOf(from), sT = scaleOf(zoom);
      f.scale = expf(logf(sF) + (logf(sT) - logf(sF)) * p);
      f.grids = 2;
      f.gridPitch[0] = RADAR_ZOOM[from].pitchR * R * f.scale / sF;
      f.gridAlpha[0] = 1 - p;
      f.gridPitch[1] = RADAR_ZOOM[zoom].pitchR * R * f.scale / sT;
      f.gridAlpha[1] = p;
      f.transition = true;
    } else if (stillKind != 2) {
      zoomFrom = -1;
    }
  }
  f.gridR = f.bootT >= 0 ? rampf(f.bootT, RADAR_BOOT_GRID_FROM_MS, RADAR_BOOT_GRID_TO_MS) * 1.02f : 1.5f;
  // Sweep: only while on (not during the boot or power-off timelines).
  const bool scanning = f.power == P_ON && f.bootT < 0 && f.offT < 0;
  f.line = scanning && (sweep == RADAR_SWEEP_LINE || sweep == RADAR_SWEEP_BOTH);
  f.lineDeg = stillSweepDeg >= 0 ? stillSweepDeg : 360.0f * ((now - sweepEpoch) % RADAR_SWEEP_LINE_PERIOD_MS) / RADAR_SWEEP_LINE_PERIOD_MS;
  f.ring = false;
  if (scanning && (sweep == RADAR_SWEEP_RING || sweep == RADAR_SWEEP_BOTH) && (blinkEver || stillRingP >= 0)) {
    // A ring from the centre on every beep (or blink, while muted), out to maxR over durationMs.
    float rp = stillRingP >= 0 ? stillRingP : (float)(now - blinkAt) / RADAR_SWEEP_RING_DURATION_MS;
    if (rp < 1) {
      f.ring = true;
      f.ringR = max(1.0f, rp * RADAR_SWEEP_RING_MAX_R * R);
      f.ringA = RADAR_SWEEP_RING_ALPHA * (1 - rp);
    }
  }
  if (power == P_OFF) return;

  const RadarPalette &p = pal();
  // Balls and edge arrows.
  float ballsA = 1;
  if (f.bootT >= 0) ballsA = rampf(f.bootT, RADAR_BOOT_BALLS_FROM_MS, RADAR_BOOT_BALLS_TO_MS);
  if (f.offT >= 0) ballsA = 1 - rampf(f.offT, 0, RADAR_POWER_OFF_BALLS_OUT_MS);
  if (ballsA > 0 && ballCount) {
    const float b = blinkLevel(now), heading = headingDeg();
    int order[MAX_BALLS];
    for (int i = 0; i < ballCount; i++) order[i] = i;
    for (int i = 1; i < ballCount; i++)  // by distance, nearest first
      for (int j = i; j > 0 && balls[order[j]].distanceM < balls[order[j - 1]].distanceM; j--) {
        int t = order[j];
        order[j] = order[j - 1];
        order[j - 1] = t;
      }
    float arrows[MAX_BALLS];
    int nArrows = 0;
    struct Dot {
      float x, y;
      int n;
      uint8_t stars;  // of the first (nearest) ball of the group
    } dots[MAX_BALLS];
    int nDots = 0;
    for (int k = 0; k < ballCount; k++) {
      const Ball &bl = balls[order[k]];
      float rel = rad(bl.bearingDeg - heading), r = bl.distanceM * f.scale;
      if (r > RADAR_BALL_VISIBLE_R) {
        bool near = false;
        for (int i = 0; i < nArrows; i++)
          if (fabsf(wrapPi(arrows[i] - rel)) < rad(RADAR_ARROW_MERGE_DEG)) near = true;
        if (!near) arrows[nArrows++] = rel;
        continue;
      }
      float x = sinf(rel) * r, y = -cosf(rel) * r;
      int hit = -1;
      for (int i = 0; i < nDots && hit < 0; i++)
        if (hypotf(dots[i].x - x, dots[i].y - y) < RADAR_BALL_MERGE_R) hit = i;
      if (hit >= 0) {
        Dot &d = dots[hit];
        d.x = (d.x * d.n + x) / (d.n + 1);
        d.y = (d.y * d.n + y) / (d.n + 1);
        d.n++;
      } else {
        dots[nDots++] = {x, y, 1, bl.stars};
      }
    }
    const float bq = roundf(b * 128) / 128;  // brightness steps finer than the eye sees
    for (int i = 0; i < nArrows && f.nEl < MAX_ELEMS; i++) {
      Elem &e = f.el[f.nEl++];
      e.type = E_ARROW;
      e.x = arrows[i];
      e.b = withFlash(f, bq, wrap360(arrows[i] * 180.0f / (float)M_PI));
      e.a = ballsA;
      e.n = 0;
      float r1 = (RADAR_ARROW_RADIUS_R + RADAR_ARROW_LENGTH_R / 2) * R;
      float cx = CX + sinf(e.x) * RADAR_ARROW_RADIUS_R * R, cy = CY - cosf(e.x) * RADAR_ARROW_RADIUS_R * R;
      float ext = max(r1 - RADAR_ARROW_RADIUS_R * R, RADAR_ARROW_WIDTH_R / 2 * R) + 1;
      boxAround(e, cx - ext, cy - ext, cx + ext, cy + ext);
      e.sig = hashf(hashf(hashf(E_ARROW, e.x, 2000), e.b, 128), ballsA, 128);
    }
    for (int i = 0; i < nDots; i++) {
      // A group of n balls is a little cluster of n spheres; the rightmost one carries the digit.
      float ox[MAX_BALLS], oy[MAX_BALLS];
      const int n = min(dots[i].n, MAX_BALLS);
      clusterOffsets(n, ox, oy);
      int lead = 0;
      for (int k = 1; k < n; k++)
        if (ox[k] > ox[lead] + 1e-4f || (fabsf(ox[k] - ox[lead]) <= 1e-4f && oy[k] < oy[lead])) lead = k;
      // The hand lights the group as it passes (on top of the beep blink).
      const float eb = withFlash(f, bq, wrap360(atan2f(dots[i].x, -dots[i].y) * 180.0f / (float)M_PI));
      for (int k = 0; k < n && f.nEl < MAX_ELEMS; k++) {
        Elem &e = f.el[f.nEl++];
        e.type = E_DOT;
        e.x = CX + (dots[i].x + ox[k]) * R;
        e.y = CY + (dots[i].y + oy[k]) * R;
        e.n = k == lead ? dots[i].n : 0;  // 0: no digit
        e.stars = dots[i].stars;
        e.b = eb;
        e.a = ballsA;
        float g = RADAR_BALL_GLOW_R * R + 1;
        boxAround(e, e.x - g, e.y - g, e.x + g, e.y + g);
        e.sig = hashf(hashf(hashf(hashf(hashf(hashf(E_DOT, e.x, 8), e.y, 8), e.n, 1), e.stars, 1), e.b, 128), ballsA, 128);
      }
    }
    (void)p;
  }

  // Marker: blinks in during boot.
  float markerA = 1;
  if (f.bootT >= 0) {
    if (f.bootT < (int32_t)RADAR_BOOT_MARKER_FROM_MS) markerA = 0;
    else if (f.bootT < (int32_t)RADAR_BOOT_MARKER_TO_MS) {
      float slot = (RADAR_BOOT_MARKER_TO_MS - RADAR_BOOT_MARKER_FROM_MS) / (2.0f * RADAR_BOOT_MARKER_BLINKS);
      markerA = ((int)floorf((f.bootT - RADAR_BOOT_MARKER_FROM_MS) / slot)) % 2 == 0 ? 1 : 0;
    }
  }
  if (markerA > 0 && f.nEl < MAX_ELEMS) {
    Elem &e = f.el[f.nEl++];
    e.type = E_MARKER;
    e.a = markerA;
    float h = RADAR_MARKER_HEIGHT_R * R, w = RADAR_MARKER_BASE_R / 2 * R, m = RADAR_MARKER_EDGE_R * R + 2;
    boxAround(e, CX - w - m, CY - 2.0f / 3 * h - m, CX + w + m, CY + h / 3 + m);
    e.sig = hashf(E_MARKER, markerA, 128);
  }

  // Boot: one scan ring from the centre to the edge.
  if (f.bootT >= (int32_t)RADAR_BOOT_PING_FROM_MS && f.bootT < (int32_t)RADAR_BOOT_PING_TO_MS && f.nEl < MAX_ELEMS) {
    float pp = rampf(f.bootT, RADAR_BOOT_PING_FROM_MS, RADAR_BOOT_PING_TO_MS);
    Elem &e = f.el[f.nEl++];
    e.type = E_PING;
    e.r = max(1.0f, pp * R);
    e.a = 0.85f * (1 - pp);
    float ext = e.r + RADAR_BOOT_PING_WIDTH_R * R / 2 + 1;
    boxAround(e, CX - ext, CY - ext, CX + ext, CY + ext);
    e.sig = hashf(hashf(E_PING, e.r, 4), e.a, 128);
  }

  // Scale label after a zoom change.
#if RADAR_SCALE_LABEL_ENABLED
  if (f.bootT < 0 && f.offT < 0 && labelShown && stillKind == 0) {
    float e = (float)(now - labelAt);
    if (e <= RADAR_SCALE_LABEL_SHOW_MS + RADAR_SCALE_LABEL_FADE_MS && f.nEl < MAX_ELEMS) {
      float a = e < RADAR_SCALE_LABEL_SHOW_MS ? 1 : 1 - (e - RADAR_SCALE_LABEL_SHOW_MS) / RADAR_SCALE_LABEL_FADE_MS;
      Elem &el = f.el[f.nEl++];
      el.type = E_LABEL;
      el.n = zoom;
      el.a = a;
      const char *s = RADAR_ZOOM[zoom].label;
      float w = textWidthPx(s, RADAR_SCALE_LABEL_HEIGHT_R), h = RADAR_SCALE_LABEL_HEIGHT_R * R;
      float x0 = CX - w / 2, y0 = CY + (RADAR_SCALE_LABEL_Y_R - RADAR_SCALE_LABEL_HEIGHT_R / 2) * R;
      float m = RADAR_SCALE_LABEL_STROKE_R * R + 1;
      boxAround(el, x0 - m, y0 - m, x0 + w + m, y0 + h + m);
      el.sig = hashf(hashf(E_LABEL, zoom, 1), a, 128);
    } else {
      labelShown = false;
    }
  }
#endif
}

// ====================================================================== rasterising

// Stroked polylines of the radar font: the pixel coverage of a round-capped stroke of half-width
// hw is clamp(hw + 0.5 - d, 0, 1), with d the distance to the nearest segment of the whole text.
struct Seg {
  float ax, ay, bx, by;
};

static int textSegments(const char *s, float x, float y, float heightR, Seg *out, int max) {
  const float k = heightR * R / RADAR_FONT_BOX_H;
  int n = 0;
  for (int i = 0; s[i]; i++) {
    for (int gi = 0; gi < RADAR_FONT_GLYPH_COUNT; gi++) {
      const RadarGlyph &g = RADAR_FONT_GLYPHS[gi];
      if (g.ch != s[i]) continue;
      for (int si = 0; si < g.strokeCount; si++) {
        const RadarStroke &st = RADAR_FONT_STROKES[g.firstStroke + si];
        for (int pi = 0; pi + 1 < st.count && n < max; pi++) {
          const float *p0 = &RADAR_FONT_POINTS[2 * (st.first + pi)];
          out[n++] = {x + (i * RADAR_FONT_ADVANCE + p0[0]) * k, y + p0[1] * k, x + (i * RADAR_FONT_ADVANCE + p0[2]) * k,
                      y + p0[3] * k};
        }
      }
      break;
    }
  }
  return n;
}

static inline float segDist2(float px, float py, const Seg &s) {
  float dx = s.bx - s.ax, dy = s.by - s.ay, l2 = dx * dx + dy * dy;
  float t = l2 > 0 ? clamp01(((px - s.ax) * dx + (py - s.ay) * dy) / l2) : 0;
  float ex = s.ax + dx * t - px, ey = s.ay + dy * t - py;
  return ex * ex + ey * ey;
}

static void strokeText(const char *s, float x, float y, float heightR, float strokeR, const Rgb &c, float alpha) {
  if (alpha <= 0.002f) return;
  static Seg segs[48];  // static: the loop task's stack is small, and only it renders
  int n = textSegments(s, x, y, heightR, segs, 48);
  if (!n) return;
  const float hw = max(1.0f, strokeR * R) / 2;
  float minX = 1e9, minY = 1e9, maxX = -1e9, maxY = -1e9;
  for (int i = 0; i < n; i++) {
    minX = min(minX, min(segs[i].ax, segs[i].bx));
    maxX = max(maxX, max(segs[i].ax, segs[i].bx));
    minY = min(minY, min(segs[i].ay, segs[i].by));
    maxY = max(maxY, max(segs[i].ay, segs[i].by));
  }
  int x0, y0, x1, y1;
  if (!clipBox(minX - hw - 1, minY - hw - 1, maxX + hw + 1, maxY + hw + 1, x0, y0, x1, y1)) return;
  const float lim = (hw + 0.5f) * (hw + 0.5f), m = hw + 0.5f;
  static float bx0[48], by0[48], bx1[48], by1[48];
  for (int i = 0; i < n; i++) {
    bx0[i] = min(segs[i].ax, segs[i].bx) - m;
    bx1[i] = max(segs[i].ax, segs[i].bx) + m;
    by0[i] = min(segs[i].ay, segs[i].by) - m;
    by1[i] = max(segs[i].ay, segs[i].by) + m;
  }
  const RgbI ci = toI(c);
  for (int py = y0; py < y1; py++) {
    const float fy = py + 0.5f;
    for (int px = x0; px < x1; px++) {
      const float fx = px + 0.5f;
      float d2 = 1e9;
      for (int i = 0; i < n && d2 > 0.01f; i++)
        if (fx >= bx0[i] && fx <= bx1[i] && fy >= by0[i] && fy <= by1[i]) d2 = min(d2, segDist2(fx, fy, segs[i]));
      if (d2 >= lim) continue;
      float cov = clamp01(hw + 0.5f - sqrtf(d2));
      blendI(fb[py * fbStride + px], ci, (int)(cov * alpha * 256 + 0.5f));
    }
  }
}

// Filled (and optionally stroked) triangle with analytic edge coverage.
static void triangle(const float *xs, const float *ys, const Rgb &fill, float fillA, const Rgb *edge, float edgeW) {
  float minX = min(xs[0], min(xs[1], xs[2])), maxX = max(xs[0], max(xs[1], xs[2]));
  float minY = min(ys[0], min(ys[1], ys[2])), maxY = max(ys[0], max(ys[1], ys[2]));
  const float m = (edge ? edgeW / 2 : 0) + 1;
  int x0, y0, x1, y1;
  if (!clipBox(minX - m, minY - m, maxX + m, maxY + m, x0, y0, x1, y1)) return;
  // Edge normals pointing out, whatever the winding.
  float area = (xs[1] - xs[0]) * (ys[2] - ys[0]) - (ys[1] - ys[0]) * (xs[2] - xs[0]);
  float sgn = area > 0 ? 1 : -1;
  float nx[3], ny[3], nc[3];
  Seg segs[3];
  for (int i = 0; i < 3; i++) {
    int j = (i + 1) % 3;
    float ex = xs[j] - xs[i], ey = ys[j] - ys[i], l = sqrtf(ex * ex + ey * ey);
    nx[i] = sgn * ey / l;
    ny[i] = -sgn * ex / l;
    nc[i] = -(nx[i] * xs[i] + ny[i] * ys[i]);
    segs[i] = {xs[i], ys[i], xs[j], ys[j]};
  }
  const float hw = edgeW / 2;
  for (int py = y0; py < y1; py++)
    for (int px = x0; px < x1; px++) {
      float fx = px + 0.5f, fy = py + 0.5f;
      float sd = -1e9;
      for (int i = 0; i < 3; i++) sd = max(sd, nx[i] * fx + ny[i] * fy + nc[i]);
      uint16_t &dst = fb[py * fbStride + px];
      if (sd < 0.5f) blendPx(dst, fill, clamp01(0.5f - sd) * fillA);
      if (edge && sd > -hw - 1 && sd < hw + 1) {
        float d2 = min(segDist2(fx, fy, segs[0]), min(segDist2(fx, fy, segs[1]), segDist2(fx, fy, segs[2])));
        float cov = clamp01(hw + 0.5f - sqrtf(d2));
        blendPx(dst, *edge, cov * fillA);
      }
    }
}

static void drawArrow(const Elem &e) {
  const RadarPalette &p = pal();
  const float r0 = (RADAR_ARROW_RADIUS_R - RADAR_ARROW_LENGTH_R / 2) * R, r1 = (RADAR_ARROW_RADIUS_R + RADAR_ARROW_LENGTH_R / 2) * R;
  const float w = RADAR_ARROW_WIDTH_R / 2 * R, c = cosf(e.x), s = sinf(e.x);
  // ctx.rotate(rel): local (u, v) -> (u c - v s, u s + v c), around the centre.
  const float lu[3] = {0, w, -w}, lv[3] = {-r1, -r0, -r0};
  float xs[3], ys[3];
  for (int i = 0; i < 3; i++) {
    xs[i] = CX + lu[i] * c - lv[i] * s;
    ys[i] = CY + lu[i] * s + lv[i] * c;
  }
  triangle(xs, ys, rgbOf(p.arrow888), e.a * (0.45f + 0.55f * e.b), nullptr, 0);
}

// Glow and core alpha (0-256) by squared distance from the dot centre, for the brightness and
// alpha of this frame (all dots share them): the preview's 9-stop gradient, alpha = glowAlpha * b *
// (1 - t)^falloff at t = i/8 and linear between the stops; the core disc with an anti-aliased edge.
static const float GLOW_PX = RADAR_BALL_GLOW_R * (LCD_NATIVE_W / 2.0f);
static const int DOT_LUT_N = (int)((GLOW_PX + 1.5f) * (GLOW_PX + 1.5f)) + 2;
// A few tables at once: with the sweep hand every dot has its own brightness. 0-255 per entry,
// read back as 0-256 (v + v / 128).
static const int DOT_LUT_SLOTS = 8;
static uint8_t glowLuts[DOT_LUT_SLOTS][DOT_LUT_N], coreLuts[DOT_LUT_SLOTS][DOT_LUT_N];
static float lutB[DOT_LUT_SLOTS], lutA[DOT_LUT_SLOTS];
static uint32_t lutUsed[DOT_LUT_SLOTS], lutClock = 0;
static bool lutInit = false;

static int dotLuts(float b, float a) {
  if (!lutInit) {
    for (int i = 0; i < DOT_LUT_SLOTS; i++) lutB[i] = lutA[i] = -1;
    lutInit = true;
  }
  lutClock++;
  int slot = 0;
  for (int i = 0; i < DOT_LUT_SLOTS; i++) {
    if (lutB[i] == b && lutA[i] == a) {
      lutUsed[i] = lutClock;
      return i;
    }
    if (lutUsed[i] < lutUsed[slot]) slot = i;  // least recently used
  }
  lutB[slot] = b;
  lutA[slot] = a;
  lutUsed[slot] = lutClock;
  const float gr = GLOW_PX, cr = RADAR_BALL_CORE_R * R;
  float stops[9];
  for (int i = 0; i <= 8; i++) stops[i] = a * RADAR_BALL_GLOW_ALPHA * b * powf(1 - i / 8.0f, RADAR_BALL_GLOW_FALLOFF);
  for (int i = 0; i < DOT_LUT_N; i++) {
    float d = sqrtf(i + 0.5f), t = d / gr * 8;
    int k = (int)t;
    float g = k >= 8 ? 0 : stops[k] + (stops[k + 1] - stops[k]) * (t - k);
    g *= clamp01(gr + 0.5f - d);
    glowLuts[slot][i] = (uint8_t)min(255.0f, g * 255 + 0.5f);
    coreLuts[slot][i] = (uint8_t)min(255.0f, clamp01(cr + 0.5f - d) * a * 255 + 0.5f);
  }
  return slot;
}

static void drawDot(const Elem &e) {
  const RadarPalette &p = pal();
  const float gr = GLOW_PX;
  int x0, y0, x1, y1;
  if (clipBox(e.x - gr - 1, e.y - gr - 1, e.x + gr + 1, e.y + gr + 1, x0, y0, x1, y1)) {
    const int slot = dotLuts(e.b, e.a);
    const uint8_t *glowLut = glowLuts[slot], *coreLut = coreLuts[slot];
    const RgbI glowI = toI(rgbOf(p.ballGlow888));
    const RgbI coreI = toI(mixRgb(rgbOf(p.ballDim888), rgbOf(p.ballCore888), e.b));
    const uint16_t glow = pack(glowI.r, glowI.g, glowI.b), core = pack(coreI.r, coreI.g, coreI.b);
    const float gr1 = gr + 1;
    for (int py = y0; py < y1; py++) {
      const float dy = py + 0.5f - e.y, dy2 = dy * dy;
      if (dy2 >= gr1 * gr1) continue;
      uint16_t *row = fb + py * fbStride;
      const float half = sqrtf(gr1 * gr1 - dy2);  // the glow disc on this row
      const int rx0 = max(x0, (int)floorf(e.x - half)), rx1 = min(x1, (int)ceilf(e.x + half));
      for (int px = rx0; px < rx1; px++) {
        const float dx = px + 0.5f - e.x;
        const int i = (int)(dx * dx + dy2);
        if (i >= DOT_LUT_N) continue;
        // 5-bit alpha: one step of the glow is under one RGB565 level.
        const uint32_t g = (glowLut[i] + 4) >> 3, c = (coreLut[i] + 4) >> 3;
        if (g) row[px] = blend565(row[px], glow, g);
        if (c) row[px] = c >= 32 ? core : blend565(row[px], core, c);
      }
    }
  }
  // stars: the star number of the dot's nearest ball; all: every dot's ball count; clusters: the
  // count of merged dots only; none: no digits.
  uint8_t counts = p.counts;
  if (!e.n) return;  // a cluster sphere without the digit
  if (counts == RADAR_COUNTS_STARS || counts == RADAR_COUNTS_ALL || (counts == RADAR_COUNTS_CLUSTERS && e.n > 1)) {
    char s[4];
    snprintf(s, sizeof(s), "%u", counts == RADAR_COUNTS_STARS ? e.stars : e.n);
    const float h = RADAR_BALL_DIGIT_HEIGHT_R;
    strokeText(s, e.x + RADAR_BALL_DIGIT_DX_R * R, e.y + (RADAR_BALL_DIGIT_DY_R - h / 2) * R, h, RADAR_BALL_DIGIT_STROKE_R,
               rgbOf(p.digits888), e.a * (0.55f + 0.45f * e.b));
  }
}

static void drawMarker(const Elem &e) {
  const RadarPalette &p = pal();
  const float h = RADAR_MARKER_HEIGHT_R * R, w = RADAR_MARKER_BASE_R / 2 * R;
  const float xs[3] = {CX, CX + w, CX - w}, ys[3] = {CY - 2.0f / 3 * h, CY + h / 3, CY + h / 3};
  Rgb edge = rgbOf(p.markerEdge888);
  triangle(xs, ys, rgbOf(p.marker888), e.a, &edge, max(1.0f, RADAR_MARKER_EDGE_R * R));
}

static void drawPing(const Elem &e) {
  const float hw = RADAR_BOOT_PING_WIDTH_R * R / 2, ro = e.r + hw + 1;
  int x0, y0, x1, y1;
  if (!clipBox(CX - ro, CY - ro, CX + ro, CY + ro, x0, y0, x1, y1)) return;
  const Rgb c = rgbOf(pal().ping888);
  const float ri = max(0.0f, e.r - hw - 1);
  for (int py = y0; py < y1; py++) {
    float dy = py + 0.5f - CY;
    for (int px = x0; px < x1; px++) {
      float dx = px + 0.5f - CX, d2 = dx * dx + dy * dy;
      if (d2 < ri * ri || d2 > ro * ro) continue;
      float cov = clamp01(hw + 0.5f - fabsf(sqrtf(d2) - e.r));
      blendPx(fb[py * fbStride + px], c, cov * e.a);
    }
  }
}

static void drawLabel(const Elem &e) {
  const char *s = RADAR_ZOOM[e.n].label;
  float w = textWidthPx(s, RADAR_SCALE_LABEL_HEIGHT_R);
  strokeText(s, CX - w / 2, CY + (RADAR_SCALE_LABEL_Y_R - RADAR_SCALE_LABEL_HEIGHT_R / 2) * R, RADAR_SCALE_LABEL_HEIGHT_R,
             RADAR_SCALE_LABEL_STROKE_R, rgbOf(pal().text888), e.a);
}

// ---------------------------------------------------------------- sweep
// Angles are degrees clockwise from up (north on screen). Fast atan2 for the per-pixel angle of the
// trail (max error ~0.25 degrees, invisible in a soft gradient).
static inline float angleDeg(float x, float y) {  // screen vector (x right, y down)
  const float ax = fabsf(x), ay = fabsf(y);
  const float mn = min(ax, ay), mx = max(ax, ay) + 1e-9f, z = mn / mx;
  float a = z * (0.7853982f + 0.273f * (1 - z));  // atan(z), z in [0, 1]
  if (ay > ax) a = 1.5707963f - a;                  // angle from the x axis, first quadrant
  // atan2(x, -y): clockwise from up.
  float t;
  if (x >= 0) t = y <= 0 ? 1.5707963f - a : 1.5707963f + a;
  else t = y <= 0 ? 4.712389f + a : 4.712389f - a;
  return t * (180.0f / (float)M_PI);
}

// A wedge between the rays at a0 and a1 (degrees clockwise from up, a1 - a0 <= 180) within a radius,
// widened by pad px; the ray directions are worked out once.
struct Wedge {
  float d0x, d0y, d1x, d1y, rmax, pad;
};

static Wedge makeWedge(float a0, float a1, float rmax, float pad) {
  return {sinf(rad(a0)), -cosf(rad(a0)), sinf(rad(a1)), -cosf(rad(a1)), rmax, pad};
}

// The x-range (relative to the centre) of row `py` inside the wedge. False when empty.
static bool wedgeSpan(float py, const Wedge &w, float &x0, float &x1) {
  const float ro = w.rmax + w.pad;
  if (fabsf(py) >= ro) return false;
  const float wo = sqrtf(ro * ro - py * py);
  x0 = -wo;
  x1 = wo;
  // Inside: cross(d0, p) >= -pad and cross(d1, p) <= pad, with cross(d, p) = d.x * py - d.y * px,
  // which is linear in px.
  for (int k = 0; k < 2; k++) {
    const float dx = k ? w.d1x : w.d0x, dy = k ? w.d1y : w.d0y;
    const float c = -dy, rhs = k ? w.pad - dx * py : -w.pad - dx * py;
    const bool ge = k == 0;
    if (fabsf(c) < 1e-6f) {
      if (ge ? 0 < rhs : 0 > rhs) return false;
      continue;
    }
    const float v = rhs / c;
    if ((c > 0) == ge) x0 = max(x0, v);  // px >= v
    else x1 = min(x1, v);                // px <= v
  }
  return x1 > x0;
}

static bool wedgeSpan(float py, float a0, float a1, float rmax, float pad, float &x0, float &x1) {
  return wedgeSpan(py, makeWedge(a0, a1, rmax, pad), x0, x1);
}

// Angle of every pixel centre, clockwise from up, in 1/65536 of a turn (PSRAM, built once).
static uint16_t *angleMap = nullptr;
// Trail alpha (0-256) by how far behind the hand a pixel is, in 1/1024 of a turn.
static uint16_t trailLut[1024];

static void buildAngleMap() {
  angleMap = (uint16_t *)heap_caps_malloc(W * H * 2, MALLOC_CAP_SPIRAM);
  if (angleMap)
    for (int y = 0; y < H; y++)
      for (int x = 0; x < W; x++) {
        float a = atan2f(x + 0.5f - CX, -(y + 0.5f - CY));  // clockwise from up
        if (a < 0) a += 2 * (float)M_PI;
        angleMap[y * W + x] = (uint16_t)((uint32_t)lroundf(a / (2 * (float)M_PI) * 65536.0f) & 0xFFFF);
      }
  for (int i = 0; i < 1024; i++) {
    float d = (i + 0.5f) * 360.0f / 1024;
    trailLut[i] = d <= RADAR_SWEEP_LINE_TRAIL_DEG
                      ? (uint16_t)(RADAR_SWEEP_LINE_TRAIL_ALPHA * 256 * (1 - d / RADAR_SWEEP_LINE_TRAIL_DEG) + 0.5f)
                      : 0;
  }
}

// Trail (a fading wedge behind the hand) and the hand itself, in the theme's sweep colour.
static void drawSweepLine(float deg) {
  if (!angleMap) return;
  const float trail = RADAR_SWEEP_LINE_TRAIL_DEG, hw = max(1.0f, RADAR_SWEEP_LINE_LINE_WIDTH_R * R) / 2;
  const float ux = sinf(rad(deg)), uy = -cosf(rad(deg));
  const Rgb cf = rgbOf(pal().sweep888);
  const RgbI c = toI(cf);
  const uint16_t c565 = pack(c.r, c.g, c.b);
  const int handA = (int)(RADAR_SWEEP_LINE_ALPHA * 256 + 0.5f);
  const uint16_t hand16 = (uint16_t)((uint32_t)lroundf(deg / 360.0f * 65536.0f) & 0xFFFF);
  const Wedge wd = makeWedge(deg - trail, deg, R, 1.5f);
  const float reach = hw + 0.5f;
  for (int y = clipR.y0; y < clipR.y1; y++) {
    const float py = y + 0.5f - CY;
    uint16_t *row = fb + y * fbStride;
    // Trail: 5-bit alpha is enough for a wash of at most 28 %: each step is under one RGB565 level.
    float sx0, sx1;
    if (wedgeSpan(py, wd, sx0, sx1)) {
      const int x0 = max(clipR.x0, (int)floorf(CX + sx0)), x1 = min(clipR.x1, (int)ceilf(CX + sx1));
      const uint16_t *ang = angleMap + y * W;
      const uint8_t *bayer = BAYER[y & 3];
      for (int x = x0; x < x1; x++) {
        const uint16_t behind = (uint16_t)(hand16 - ang[x]);  // how far behind the hand, mod one turn
        // 5-bit alpha with ordered dither, so the soft gradient does not band.
        const uint32_t a5 = (trailLut[behind >> 6] * 32u + bayer[x & 3] * 16u + 8u) >> 8;
        if (a5) row[x] = blend565(row[x], c565, a5);
      }
    }
    // Hand: only the pixels within reach of the ray, solved per row.
    int hx0, hx1;
    if (fabsf(uy) > 1e-4f) {
      float a0 = (py * ux - reach) / uy, a1 = (py * ux + reach) / uy;
      if (a0 > a1) {
        float t = a0;
        a0 = a1;
        a1 = t;
      }
      hx0 = (int)floorf(CX + a0);
      hx1 = (int)ceilf(CX + a1) + 1;
    } else if (fabsf(py * ux) < reach) {
      hx0 = 0;
      hx1 = W;
    } else {
      continue;
    }
    hx0 = max(hx0, clipR.x0);
    hx1 = min(hx1, clipR.x1);
    for (int x = hx0; x < hx1; x++) {
      const float px = x + 0.5f - CX;
      const float across = fabsf(px * uy - py * ux), along = px * ux + py * uy;
      if (across < reach && along > -reach && along < R)
        blendI(row[x], c, (int)(handA * clamp01(reach - across) * clamp01(along + reach)));
    }
  }
}

static void drawSweepRing(float r, float a) {
  const float hw = RADAR_SWEEP_RING_WIDTH_R * R / 2, ro = r + hw + 1, ri = max(0.0f, r - hw - 1);
  const RgbI c = toI(rgbOf(pal().sweep888));
  for (int y = clipR.y0; y < clipR.y1; y++) {
    const float py = y + 0.5f - CY;
    if (fabsf(py) >= ro) continue;
    const float wo = sqrtf(ro * ro - py * py), wi = fabsf(py) < ri ? sqrtf(ri * ri - py * py) : 0;
    uint16_t *row = fb + y * fbStride;
    for (int side = 0; side < 2; side++) {
      // [-wo, -wi] then [wi, wo]; one run [-wo, wo] when the row misses the inner circle
      const float s0 = side ? wi : -wo, s1 = side ? wo : (wi > 0 ? -wi : wo);
      const int x0 = max(clipR.x0, (int)floorf(CX + s0)), x1 = min(clipR.x1, (int)ceilf(CX + s1));
      for (int x = x0; x < x1; x++) {
        const float px = x + 0.5f - CX, d = sqrtf(px * px + py * py);
        float cov = clamp01(hw + 0.5f - fabsf(d - r));
        if (cov > 0) blendI(row[x], c, (int)(a * cov * 256));
      }
      if (wi == 0) break;
    }
  }
}

// Animated grid (zoom transitions) over the plain background. The line colour is pre-mixed with the
// sheen at each pixel, which gives exactly the preview's order (grid under the sheen). Row by row:
// the framebuffer is in PSRAM, where walking a column misses the cache on every pixel.
static void drawGridAnimated(float pitch, float alpha) {
  if (alpha < 1 / 64.0f || pitch < 0.015f * R) return;  // below the 5-bit alpha of the blend
  static float cv[LCD_NATIVE_W];
  static uint16_t cvI[LCD_NATIVE_W];  // coverage 0-256
  static int cols[LCD_NATIVE_W];
  static uint16_t lineBySheen[256];   // line colour (RGB565) under each sheen level (sheenMap byte)
  const float lw = max(1.0f, RADAR_ZOOM[0].lineWidthR * R);
  gridCoverage(pitch, lw, cv);  // square screen: the same positions on both axes
  int nc = 0;
  for (int x = 0; x < W; x++) {
    cvI[x] = (uint16_t)(cv[x] * 256 + 0.5f);
    if (cvI[x] && x >= clipR.x0 && x < clipR.x1) cols[nc++] = x;
  }
  const Rgb grid = rgbOf(pal().grid888), white = rgbOf(pal().sheen888);
  static int lutTheme = -1;
  if (lutTheme != theme) {
    for (int i = 0; i < 256; i++) {
      Rgb c = mixRgb(grid, white, i * (RADAR_SHEEN_ALPHA / 255.0f));
      lineBySheen[i] = pack((int)(c.r + 0.5f), (int)(c.g + 0.5f), (int)(c.b + 0.5f));
    }
    lutTheme = theme;
  }
  const int a256 = (int)(alpha * 256 + 0.5f);
  for (int y = clipR.y0; y < clipR.y1; y++) {
    uint16_t *row = fb + y * fbStride;
    const bool sheenRow = sheenMap && y >= sheenY0 && y < sheenY0 + sheenH;
    const uint8_t *sm = sheenRow ? sheenMap + (y - sheenY0) * sheenW - sheenX0 : nullptr;
    const int cy = cvI[y];
    if (cy) {  // a horizontal line: the whole row, union with the vertical lines
      for (int x = clipR.x0; x < clipR.x1; x++) {
        const int cx = cvI[x], u = cx + cy - ((cx * cy) >> 8);
        const int s8 = sm && x >= sheenX0 && x < sheenX0 + sheenW ? sm[x] : 0;
        const uint32_t a5 = ((a256 * u) + 1024) >> 11;
        if (a5) row[x] = blend565(row[x], lineBySheen[s8], a5);
      }
    } else {
      for (int i = 0; i < nc; i++) {
        const int x = cols[i];
        const int s8 = sm && x >= sheenX0 && x < sheenX0 + sheenW ? sm[x] : 0;
        const uint32_t a5 = ((a256 * cvI[x]) + 1024) >> 11;
        if (a5) row[x] = blend565(row[x], lineBySheen[s8], a5);
      }
    }
  }
}

// Background of the clip rectangle for the frame.
static void composeBackground(const Frame &f) {
  for (int y = clipR.y0; y < clipR.y1; y++) {
    uint16_t *dst = fb + y * fbStride + clipR.x0;
    const int n = clipR.x1 - clipR.x0;
    if (f.transition || f.gridR <= 0) {
      memcpy(dst, bgPlain + y * W + clipR.x0, n * 2);
    } else if (f.gridR >= 1.5f) {
      memcpy(dst, bgGrid + y * W + clipR.x0, n * 2);
    } else {
      // Boot: the grid revealed inside a growing circle (anti-aliased edge, as a canvas clip). Only
      // the pixels near the circle's edge need a distance; the rest is a plain copy of either layer.
      const float gr = f.gridR * R, dy = y + 0.5f - CY;
      memcpy(dst, bgPlain + y * W + clipR.x0, n * 2);
      if (gr + 1 <= fabsf(dy)) continue;
      const float wo = sqrtf(max(0.0f, (gr + 1) * (gr + 1) - dy * dy));
      const float wi = gr - 1 > fabsf(dy) ? sqrtf((gr - 1) * (gr - 1) - dy * dy) : 0;
      const int xo0 = max(clipR.x0, (int)floorf(CX - wo)), xo1 = min(clipR.x1, (int)ceilf(CX + wo));
      const int xi0 = max(xo0, (int)ceilf(CX - wi)), xi1 = min(xo1, (int)floorf(CX + wi));
      if (xi1 > xi0) memcpy(dst + (xi0 - clipR.x0), bgGrid + y * W + xi0, (xi1 - xi0) * 2);
      for (int x = xo0; x < xo1; x++) {
        if (x == xi0 && xi1 > xi0) {
          x = xi1 - 1;
          continue;
        }
        float dx = x + 0.5f - CX, d = sqrtf(dx * dx + dy * dy);
        float cov = clamp01(gr - d + 0.5f);
        uint16_t pg = bgGrid[y * W + x], pp = bgPlain[y * W + x];
        if (cov >= 1) dst[x - clipR.x0] = pg;
        else if (cov <= 0) dst[x - clipR.x0] = pp;
        else {
          int r1, g1, b1, r2, g2, b2;
          unpack(pp, r1, g1, b1);
          unpack(pg, r2, g2, b2);
          dst[x - clipR.x0] = pack(r1 + (int)((r2 - r1) * cov), g1 + (int)((g2 - g1) * cov), b1 + (int)((b2 - b1) * cov));
        }
      }
    }
  }
  if (f.transition) {
    uint32_t t = micros();
    for (int i = 0; i < f.grids; i++) drawGridAnimated(f.gridPitch[i], f.gridAlpha[i]);
    uint32_t us = micros() - t;
    profUs[1] += us;
    profUs[0] -= us;  // composeRect counts the whole call as background
  }
}

// Rim (dark edge where the glass meets the casing) and the round clip. Both depend only on the
// position, so rimMap holds, for every pixel of the edge band, the clip coverage (high byte) and the
// rim weight (low byte), each 0-255; rimSpan[y] is the inner run of each row that needs nothing.
// As the canvas does it, everything is drawn through the round clip (coverage c) and the rim is
// itself a circle of radius R, so at the very edge its alpha is covered twice (c * c).
static uint16_t *rimMap = nullptr;  // band pixels of every row, left run then right run
static int32_t rimRow[LCD_NATIVE_H];  // index of each row's first entry
static int16_t rimSpan0[LCD_NATIVE_H], rimSpan1[LCD_NATIVE_H];  // inner run that needs nothing
static int16_t rimOut0[LCD_NATIVE_H], rimOut1[LCD_NATIVE_H];    // [out0, out1): pixels with some glass

static void rimEntry(float d, float r0, uint16_t &e) {
  float t = (d - r0) / (R - r0);
  float ra = t <= 0 ? 0 : t < 0.5f ? 0.55f * t / 0.5f : 0.55f + 0.45f * clamp01((t - 0.5f) / 0.5f);
  float cov = clamp01(R + 0.5f - d);
  float k = cov * cov * ra;
  e = (uint16_t)((int)(cov * 255 + 0.5f) << 8 | (int)(k * 255 + 0.5f));
}

static void buildRimMap() {
  const float r0 = R * (1 - RADAR_RIM_WIDTH_R * 2), r0sq = r0 * r0;
  int32_t total = 0;
  for (int pass = 0; pass < 2; pass++) {
    total = 0;
    for (int y = 0; y < H; y++) {
      const float dy = y + 0.5f - CY, dy2 = dy * dy;
      int o0 = W, o1 = 0;
      for (int x = 0; x < W; x++) {
        const float dx = x + 0.5f - CX;
        if (sqrtf(dx * dx + dy2) < R + 0.5f) {
          o0 = min(o0, x);
          o1 = x + 1;
        }
      }
      int i0 = o1, i1 = o1;  // no inner run: the whole [o0, o1) is band
      if (dy2 < r0sq) {
        float xi = sqrtf(r0sq - dy2) - 1;
        int a0 = (int)ceilf(CX - xi), a1 = (int)floorf(CX + xi);
        if (a1 > a0) {
          i0 = a0;
          i1 = a1;
        }
      }
      if (o1 <= o0) o0 = o1 = i0 = i1 = 0;
      rimOut0[y] = o0;
      rimOut1[y] = o1;
      rimSpan0[y] = i0;
      rimSpan1[y] = i1;
      rimRow[y] = total;
      for (int x = o0; x < o1; x++) {
        if (x >= i0 && x < i1) continue;
        if (pass) {
          const float dx = x + 0.5f - CX;
          rimEntry(sqrtf(dx * dx + dy2), r0, rimMap[total]);
        }
        total++;
      }
    }
    if (!pass) {
      rimMap = (uint16_t *)heap_caps_malloc(total * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
      if (!rimMap) rimMap = (uint16_t *)heap_caps_malloc(total * 2, MALLOC_CAP_SPIRAM);
      if (!rimMap) return;
    }
  }
  Serial.printf("[radar] rim table: %d edge pixels\n", (int)total);
}

static inline void rimPixel(uint16_t &px, uint16_t m, const RgbI &rim) {
  const int cov = m >> 8, k = m & 255;
  if (cov == 255 && k == 0) return;
  if (cov == 0) {
    px = 0;
    return;
  }
  int r = FROM5[px >> 11], g = FROM6[(px >> 5) & 63], b = FROM5[px & 31];
  // (v * keep + rim * k * 255) / 65025, with 1/65025 ~ 258 / 2^24 (the sum stays below 2^32).
  const uint32_t keep = cov * (255 - k), kr = k * 255;
  r = (int)(((uint32_t)r * keep + rim.r * kr) * 258u >> 24);
  g = (int)(((uint32_t)g * keep + rim.g * kr) * 258u >> 24);
  b = (int)(((uint32_t)b * keep + rim.b * kr) * 258u >> 24);
  px = (uint16_t)(TO5[r] << 11 | TO6[g] << 5 | TO5[b]);
}

static void finishRim() {
  if (!rimMap) return;
  const RgbI rim = toI(rgbOf(pal().rim888));
  for (int y = clipR.y0; y < clipR.y1; y++) {
    uint16_t *row = fb + y * fbStride;
    const int o0 = rimOut0[y], o1 = rimOut1[y], i0 = rimSpan0[y], i1 = rimSpan1[y];
    // Outside the glass: AMOLED black.
    if (o1 <= o0) {
      memset(row + clipR.x0, 0, (clipR.x1 - clipR.x0) * 2);
      continue;
    }
    if (o0 > clipR.x0) memset(row + clipR.x0, 0, (min(o0, clipR.x1) - clipR.x0) * 2);
    if (o1 < clipR.x1) memset(row + max(o1, clipR.x0), 0, (clipR.x1 - max(o1, clipR.x0)) * 2);
    const uint16_t *m = rimMap + rimRow[y];  // entries for [o0, i0) then [i1, o1)
    for (int x = max(o0, clipR.x0); x < min(i0, clipR.x1); x++) rimPixel(row[x], m[x - o0], rim);
    const uint16_t *m2 = m + (i0 - o0);
    for (int x = max(i1, clipR.x0); x < min(o1, clipR.x1); x++) rimPixel(row[x], m2[x - i1], rim);
  }
}

static void composeStrip(const Frame &f, const Rect &r) {
  clipR = r;
  uint32_t t = micros();
  composeBackground(f);
  uint32_t t2 = micros();
  profUs[0] += t2 - t;
  t = t2;
  // The sweep goes over the screen and under the dots, as a scan of the glass.
  if (f.line) drawSweepLine(f.lineDeg);
  if (f.ring) drawSweepRing(f.ringR, f.ringA);
  t2 = micros();
  profUs[1] += t2 - t;
  t = t2;
  for (int i = 0; i < f.nEl; i++) {
    const Elem &e = f.el[i];
    if (e.box.x1 <= r.x0 || e.box.x0 >= r.x1 || e.box.y1 <= r.y0 || e.box.y0 >= r.y1) continue;
    switch (e.type) {
      case E_ARROW: drawArrow(e); break;
      case E_DOT: drawDot(e); break;
      case E_MARKER: drawMarker(e); break;
      case E_PING: drawPing(e); break;
      case E_LABEL: drawLabel(e); break;
    }
    t2 = micros();
    profUs[2 + e.type] += t2 - t;
    t = t2;
  }
  finishRim();
  profUs[7] += micros() - t;
  clipR = {0, 0, W, H};
}

static uint32_t pushUs = 0;  // time the push task spent on the bus in this frame

struct PushJob {
  uint8_t buf;
  int16_t x, y, w, h;
};
static QueueHandle_t pushJobs = nullptr, pushFree = nullptr;
static TaskHandle_t pushTask = nullptr;

static void pushLoop(void *) {
  PushJob j;
  for (;;) {
    if (xQueueReceive(pushJobs, &j, portMAX_DELAY) != pdTRUE) continue;
    uint32_t t = micros();
    lcdPushRectBE(j.x, j.y, j.w, j.h, stripBufs[j.buf]);
    pushUs += micros() - t;
    xQueueSend(pushFree, &j.buf, portMAX_DELAY);
  }
}

static void pushBegin() {
  pushJobs = xQueueCreate(2, sizeof(PushJob));
  pushFree = xQueueCreate(2, sizeof(uint8_t));
  for (uint8_t i = 0; i < 2; i++) xQueueSend(pushFree, &i, 0);
  // Core 0, below the synth task (3): the bus transfer is mostly a wait for the DMA.
  if (pushJobs && pushFree) xTaskCreatePinnedToCore(pushLoop, "lcdpush", 3072, nullptr, 2, &pushTask, 0);
}

// Waits until both strips are back, i.e. the bus is idle (before anything else talks to the panel).
static void pushDrain() {
  if (!pushTask) return;
  uint8_t a, b;
  xQueueReceive(pushFree, &a, portMAX_DELAY);
  xQueueReceive(pushFree, &b, portMAX_DELAY);
  xQueueSend(pushFree, &a, 0);
  xQueueSend(pushFree, &b, 0);
}

// Composes rectangle `r` (even coordinates) in strips of internal RAM and sends each strip to the
// panel, composing the next strip while the push task sends the previous one. The canvas framebuffer
// is not kept up to date: radarCli('shot') composes into it on demand.
static void composeRect(const Frame &f, const Rect &r) {
  const int w = r.x1 - r.x0;
  const int rows = max(2, (STRIP_PX / w) & ~1);
  for (int y = r.y0; y < r.y1; y += rows) {
    const int y1 = min(r.y1, y + rows);
    uint8_t bi = 0;
    if (pushTask) xQueueReceive(pushFree, &bi, portMAX_DELAY);
    stripBuf = stripBufs[bi];
    fbStride = w;
    fb = stripBuf - (y * w + r.x0);
    composeStrip(f, {r.x0, y, r.x1, y1});
    // To big-endian, two pixels per word (w and the strip height are even).
    uint32_t *wp = (uint32_t *)stripBuf;
    for (int i = 0, n = w * (y1 - y) / 2; i < n; i++) wp[i] = ((wp[i] & 0x00FF00FFu) << 8) | ((wp[i] >> 8) & 0x00FF00FFu);
    PushJob j = {bi, (int16_t)r.x0, (int16_t)y, (int16_t)w, (int16_t)(y1 - y)};
    if (pushTask) {
      xQueueSend(pushJobs, &j, portMAX_DELAY);
    } else {
      uint32_t t = micros();
      lcdPushRectBE(j.x, j.y, j.w, j.h, stripBuf);
      pushUs += micros() - t;
    }
  }
  fb = nullptr;
}

// ====================================================================== frame loop

static const int BAND = 8;    // rows per band of a sweep's dirty region
static const int MAX_RECTS = 512;  // a sweep frame: ~60 bands per wedge, ~120 per ring, + elements

static bool rectsOverflow = false;  // more dirty pieces than MAX_RECTS: redraw the whole frame

static void addRect(Rect *rects, int &nr, int x0, int y0, int x1, int y1) {
  x0 = max(0, x0);
  y0 = max(0, y0);
  x1 = min(W, x1);
  y1 = min(H, y1);
  if (x1 <= x0 || y1 <= y0) return;
  if (nr < MAX_RECTS) rects[nr++] = {x0, y0, x1, y1};
  else rectsOverflow = true;
}

// A band of a sweep's staircase: joined to the band just above it (`last`, -1 for none) while the
// union wastes little, which keeps the rectangle count (and the merge pass below) small.
static void addBand(Rect *rects, int &nr, int &last, int x0, int y0, int x1, int y1) {
  x0 = max(0, x0);
  x1 = min(W, x1);
  if (x1 <= x0) return;
  if (last >= 0 && rects[last].y1 == y0) {
    Rect &a = rects[last];
    const Rect u = {min(a.x0, x0), a.y0, max(a.x1, x1), min(H, y1)};
    const int both = a.area() + (x1 - x0) * (min(H, y1) - y0);
    if (u.area() <= both + both / 8) {
      a = u;
      return;
    }
  }
  addRect(rects, nr, x0, y0, x1, y1);
  last = nr - 1;
}

// Bands covering the wedge from a0 to a1 (degrees, a1 - a0 <= 180) within radius R, widened by pad.
static void addWedge(Rect *rects, int &nr, float a0, float a1, float pad) {
  const Wedge wd = makeWedge(a0, a1, R, pad);
  int last = -1;
  for (int y = 0; y < H; y += BAND) {
    float bx0 = 1e9, bx1 = -1e9;
    for (int k = 0; k <= BAND; k += BAND / 2) {  // the span is convex in y: sample the band edges and inside
      const int yy = min(H - 1, y + k);
      float x0, x1;
      for (int e = 0; e < 2; e++)
        if (wedgeSpan(yy + (e ? 1.0f : 0.0f) - CY, wd, x0, x1)) {
          bx0 = min(bx0, x0);
          bx1 = max(bx1, x1);
        }
    }
    if (bx1 > bx0) addBand(rects, nr, last, (int)floorf(CX + bx0) - 1, y, (int)ceilf(CX + bx1) + 1, y + BAND);
    else last = -1;
  }
}

// Bands covering the annulus between radii ri and ro (left and right runs of each band).
static void addAnnulus(Rect *rects, int &nr, float ri, float ro) {
  int lastL = -1, lastR = -1;
  for (int y = 0; y < H; y += BAND) {
    const float y0 = y - CY, y1 = y + BAND - CY;  // band edges relative to the centre
    const float nearY = (y0 <= 0 && y1 >= 0) ? 0 : min(fabsf(y0), fabsf(y1)), farY = max(fabsf(y0), fabsf(y1));
    if (nearY >= ro) {
      lastL = lastR = -1;
      continue;
    }
    const float wo = sqrtf(ro * ro - nearY * nearY);       // widest outer run in the band
    const float wi = farY < ri ? sqrtf(ri * ri - farY * farY) : 0;  // narrowest inner hole in the band
    if (wi <= 0) {
      addBand(rects, nr, lastL, (int)floorf(CX - wo) - 1, y, (int)ceilf(CX + wo) + 1, y + BAND);
      lastR = -1;
    } else {
      addBand(rects, nr, lastL, (int)floorf(CX - wo) - 1, y, (int)ceilf(CX - wi) + 1, y + BAND);
      addBand(rects, nr, lastR, (int)floorf(CX + wi) - 1, y, (int)ceilf(CX + wo) + 1, y + BAND);
    }
  }
}

// The parts of the screen the sweep changed between frames a and b: the hand and its trail as one
// wedge (when `line`), the old and new rings as annuli (when `ring`).
// The trail's alpha moves by under one 5-bit step per frame (trailAlpha x the hand's few degrees /
// trailDeg), so it is repainted whole only every TRAIL_REFRESH_DEG of travel (at most 0.04 of
// alpha out of date, about one step); in between, only the
// strip the hand swept since the last frame (and the hand itself) is redrawn.
static const float TRAIL_REFRESH_DEG = 10;
static float trailRefDeg = -1;  // hand angle at the last whole-trail repaint, -1 = none yet

static void addSweepDirt(Rect *rects, int &nr, const Frame &a, const Frame &b, bool line, bool ring, bool &full) {
  const float hw = max(1.0f, RADAR_SWEEP_LINE_LINE_WIDTH_R * R) / 2 + 2;
  if (line && (a.line || b.line)) {
    if (a.line && b.line) {
      const float moved = wrap360(b.lineDeg - a.lineDeg);
      const float sinceRef = trailRefDeg < 0 ? 360 : wrap360(b.lineDeg - trailRefDeg);
      if (moved + RADAR_SWEEP_LINE_TRAIL_DEG > 179) {
        full = true;
        trailRefDeg = b.lineDeg;
      } else if (sinceRef >= TRAIL_REFRESH_DEG || sinceRef + RADAR_SWEEP_LINE_TRAIL_DEG > 179) {
        // From the end of the trail as it was last painted to the new hand.
        const float from = trailRefDeg < 0 || sinceRef + RADAR_SWEEP_LINE_TRAIL_DEG > 179 ? a.lineDeg : trailRefDeg;
        addWedge(rects, nr, from - RADAR_SWEEP_LINE_TRAIL_DEG, b.lineDeg, hw);
        trailRefDeg = b.lineDeg;
      } else {
        addWedge(rects, nr, a.lineDeg, b.lineDeg, hw);  // the hand's own strip, both positions
      }
    } else {
      const Frame &on = a.line ? a : b;  // appeared or disappeared: its whole wedge
      addWedge(rects, nr, on.lineDeg - RADAR_SWEEP_LINE_TRAIL_DEG, on.lineDeg, hw);
    }
  }
  if (!ring) return;
  const float rw = RADAR_SWEEP_RING_WIDTH_R * R / 2 + 2;
  if (a.ring && b.ring && fabsf(a.ringR - b.ringR) < 4 * rw) {
    // The ring moved a few pixels: one annulus covers both positions.
    addAnnulus(rects, nr, max(0.0f, min(a.ringR, b.ringR) - rw), max(a.ringR, b.ringR) + rw);
    return;
  }
  if (a.ring) addAnnulus(rects, nr, max(0.0f, a.ringR - rw), a.ringR + rw);
  if (b.ring) addAnnulus(rects, nr, max(0.0f, b.ringR - rw), b.ringR + rw);
}

static void alignEven(Rect &r) {
  r.x0 &= ~1;
  r.y0 &= ~1;
  r.x1 = min(W, (r.x1 + 1) & ~1);
  r.y1 = min(H, (r.y1 + 1) & ~1);
}

static bool sameElem(const Elem &a, const Elem &b) {
  return a.type == b.type && a.sig == b.sig && a.box.x0 == b.box.x0 && a.box.y0 == b.box.y0 && a.box.x1 == b.box.x1 &&
         a.box.y1 == b.box.y1;
}

static uint32_t backgroundKey(const Frame &f) {
  uint32_t k = theme * 7919u + (f.power == P_OFF ? 1 : 0);
  if (f.gridR < 1.5f) k = hashf(k ^ 0x1000, f.gridR, 2000);
  if (f.transition) k = hashf(hashf(k ^ 0x2000, f.gridPitch[0], 64), f.gridAlpha[0], 512);
  return k;
}

static void render(uint32_t now) {
  Arduino_Canvas *c = lcdCanvas();
  if (!c) return;
  canvasFb = c->getFramebuffer();
  static int shownTheme = -1;
  if (shownTheme != theme) {
    if (!selectTheme()) return;
    shownTheme = theme;
    fullPending = true;
  }
  const uint32_t tPrep = micros();
  describe(cur, now);

  if (cur.power == P_OFF && stillKind == 0) {
    if (!offDrawn) {
      memset(canvasFb, 0, W * H * 2);
      lcdFlushRect(0, 0, W, H);
      offDrawn = true;
      prevValid = false;
    }
    return;
  }
  offDrawn = false;

  uint32_t bgKey = backgroundKey(cur);
  bool full = fullPending || !prevValid || bgKey != bgKeyPrev;
  static Rect rects[MAX_RECTS];
  int nr = 0;
  rectsOverflow = false;
  if (!full) {
    const bool lineChanged = (cur.line || prev.line) && (cur.line != prev.line || cur.lineDeg != prev.lineDeg);
    const bool ringChanged =
        (cur.ring || prev.ring) && (cur.ring != prev.ring || cur.ringR != prev.ringR || cur.ringA != prev.ringA);
    if (lineChanged || ringChanged) addSweepDirt(rects, nr, prev, cur, lineChanged, ringChanged, full);
  }
  if (!full) {
    for (int i = 0; i < cur.nEl; i++) {
      bool same = false;
      for (int j = 0; j < prev.nEl && !same; j++) same = sameElem(cur.el[i], prev.el[j]);
      if (!same && nr < MAX_RECTS) rects[nr++] = cur.el[i].box;
    }
    for (int j = 0; j < prev.nEl; j++) {
      bool same = false;
      for (int i = 0; i < cur.nEl && !same; i++) same = sameElem(cur.el[i], prev.el[j]);
      if (!same && nr < MAX_RECTS) rects[nr++] = prev.el[j].box;
    }
    // Merge rectangles whose union wastes little: overlapping or touching ones, but not two
    // bands of a sweep's staircase that only meet at a corner.
    for (int i = 0; i < nr; i++)
      for (int j = i + 1; j < nr; j++) {
        Rect &a = rects[i], &b = rects[j];
        if (a.x0 > b.x1 + 4 || b.x0 > a.x1 + 4 || a.y0 > b.y1 + 4 || b.y0 > a.y1 + 4) continue;
        const Rect u = {min(a.x0, b.x0), min(a.y0, b.y0), max(a.x1, b.x1), max(a.y1, b.y1)};
        const int ix = max(0, min(a.x1, b.x1) - max(a.x0, b.x0)), iy = max(0, min(a.y1, b.y1) - max(a.y0, b.y0));
        const int both = a.area() + b.area() - ix * iy;
        if (u.area() > both + both / 16 + 64) continue;
        a = u;
        rects[j] = rects[--nr];
        j = i;  // `a` grew: check it against the rest again
      }
    int area = 0;
    for (int i = 0; i < nr; i++) {
      alignEven(rects[i]);
      area += rects[i].area();
    }
    if (area > FULL_FRAME_SHARE * W * H || rectsOverflow || nr >= MAX_RECTS) full = true;
  }
  if (full) {
    rects[0] = {0, 0, W, H};
    nr = 1;
    if (cur.line) trailRefDeg = cur.lineDeg;
  }
  if (nr == 0) {  // nothing changed
    prev = cur;
    return;
  }
  memset(profUs, 0, sizeof(profUs));
  pushUs = 0;
  uint32_t t0 = micros();
  statPrepUs = t0 - tPrep;
  statRects = nr;
  uint32_t px = 0;
  for (int i = 0; i < nr; i++) {
    composeRect(cur, rects[i]);
    px += rects[i].area();
  }
  pushDrain();
  if (full) memcpy(profFullUs, profUs, sizeof(profUs));
  // Frame time t2 - t0; the bus time overlaps with composing, so "compose" = frame time - bus time.
  uint32_t t2 = micros(), t1 = t2 - min(pushUs, t2 - t0);
  statComposeUs = t1 - t0;
  statFlushUs = t2 - t1;
  statPixels = px;
  statFrames++;
  if (full) {
    statFull++;
    statFullComposeUs = t1 - t0;
    statFullFlushUs = pushUs;
    statFullFrameUs = t2 - t0;
    statFullMaxUs = max(statFullMaxUs, t2 - t0);
  } else {
    statPartSumUs += t2 - t0;
    statPartN++;
    statPartMaxUs = max(statPartMaxUs, t2 - t0);
  }
  statWindowFrames++;
  if ((int32_t)(now - statWindowStart) >= 1000) {
    statFps = statWindowFrames * 1000.0f / (now - statWindowStart);
    statWindowStart = now;
    statWindowFrames = 0;
  }
  prev = cur;
  prevValid = true;
  bgKeyPrev = bgKey;
  fullPending = false;
}

// The last frame, composed into the canvas framebuffer for lcdShot() (tools/screenshot.py).
static void composeToCanvas() {
  Arduino_Canvas *c = lcdCanvas();
  if (!c || !prevValid) return;
  canvasFb = c->getFramebuffer();
  fb = canvasFb;
  fbStride = W;
  composeStrip(prev, {0, 0, W, H});
  fb = nullptr;
}

// ====================================================================== sound

static int sfxIdOf(const char *name) {
  for (int i = 0; i < SFX_COUNT; i++)
    if (name && !strcmp(SFX[i].name, name)) return i;
  return -1;
}

static int gainOf(const char *name) {
  for (int i = 0; i < RADAR_SFX_GAIN_COUNT; i++)
    if (!strcmp(RADAR_SFX_GAIN[i].name, name)) return RADAR_SFX_GAIN[i].gain;
  return 100;
}

// Press sounds. Every physical press plays RADAR_PRESS_SFX on its down edge (radarPressDown) while
// the radar is on (off it is silent); the action that follows (zoom, power) plays its own effect, carrying on from
// the part it shares with the press sound. A press sound has priority over the beep: the next beep
// waits until it is over. Mute applies to them too; muted, the effect goes to the synth as a dry run
// so the log still says when it would have started.
#define RADAR_PRESS_SFX "button"
static const uint32_t PRESS_FRESH_MS = 1500;  // an effect this soon after a press belongs to it
static uint32_t pressAt = 0;                  // millis() of the last press edge
static bool pressSeen = false;
static bool pressUsed = true;                 // its zoom already continued the press sound
static const char *pressSrc = "";
static uint32_t pressGuardUntil = 0;          // the beep waits until then
static bool pressGuard = false;
static struct {
  bool pending = false;
  uint32_t request = 0, offsetMs = 0, callMs = 0, callUs = 0;
  int32_t sincePressMs = -1;
  const char *name = "", *src = "";
} startReport;

static uint32_t sfxDurationMs(int id) {
  static Tone t[96];
  String err;
  int n = soundParseNotes(SFX[id].notes, SFX[id].tempo, t, 96, err);
  return n > 0 ? soundDurationMs(t, n) : 0;
}

// Plays an effect at the user volume x its gain from `offsetMs` in, unless muted (then a dry run).
// Ignores the power state. `guard`: the beep waits until it is over. Returns false when muted.
static bool playSfxFrom(const char *name, uint32_t offsetMs, bool guard) {
  int id = sfxIdOf(name);
  if (id < 0) return false;
  const int vol = soundConfig().volume * gainOf(name) / 100;
  const bool beep = !strcmp(name, RADAR_BEEP_SFX);
  if (beep) {  // every beep: no log, no dry run
    if (muted) return false;
    soundPlaySfx((SfxId)id, vol);
    return true;
  }
  const uint32_t now = millis();
  soundPlaySfxFrom((SfxId)id, vol, offsetMs, muted);
  const uint32_t dur = sfxDurationMs(id) > offsetMs ? sfxDurationMs(id) - offsetMs : 0;
  if (guard) {  // what the synth may still add before it: an amplifier settle and one block
    pressGuardUntil = now + dur + 30;
    pressGuard = true;
  }
  startReport.pending = true;
  startReport.request = soundLastRequest();
  startReport.offsetMs = offsetMs;
  startReport.callMs = now;
  startReport.callUs = micros();
  startReport.sincePressMs = pressSeen && now - pressAt < PRESS_FRESH_MS ? (int32_t)(now - pressAt) : -1;
  startReport.name = SFX[id].name;
  startReport.src = pressSrc;
  if (muted) Serial.printf("[radar] sfx %s from %u ms: muted, dry run\n", name, (unsigned)offsetMs);
  else Serial.printf("[radar] sfx %s from %u ms at volume %d\n", name, (unsigned)offsetMs, vol);
  return !muted;
}

static bool playSfx(const char *name) { return playSfxFrom(name, 0, false); }

// Once the synth has taken the latest effect: when its first sample comes out, from the press edge.
static void startReportStep() {
  if (!startReport.pending) return;
  SoundStartInfo i;
  if (!soundLastStart(i) || i.request != startReport.request) {
    if (millis() - startReport.callMs > 500) startReport.pending = false;  // no codec or superseded
    return;
  }
  startReport.pending = false;
  const float takeMs = (int32_t)(i.takenUs - i.requestUs) / 1000.0f;
  const float firstMs = takeMs + i.settleMs + i.queueMs + i.leadRestMs;
  Serial.printf("[sound] %s%s: synth took it after %.2f ms, amplifier %s (settle %u ms), DMA queue <= %u ms",
                startReport.name, i.dryRun ? " (dry run, muted)" : "", takeMs, i.paWasOn ? "on" : "off",
                (unsigned)i.settleMs, (unsigned)i.queueMs);
  if (i.leadRestMs) Serial.printf(", opening rest %u ms", (unsigned)i.leadRestMs);
  Serial.printf(" -> first sample <= %.1f ms after the call", firstMs);
  if (startReport.sincePressMs >= 0)
    Serial.printf(", <= %.1f ms after the %s press edge", startReport.sincePressMs + firstMs, startReport.src);
  Serial.printf(", lasts %u ms\n", (unsigned)i.durationMs);
}

void radarPressDown(const char *src) {
  pressAt = millis();
  pressSeen = true;
  pressUsed = false;
  pressSrc = src;
  // Off (or booting, or powering off) the radar is silent: only power on/off has a sound then.
  if (power != P_ON) {
    pressUsed = true;
    return;
  }
  playSfxFrom(RADAR_PRESS_SFX, 0, true);
}

// The effect of the action a press started. `carryOn`: it carries on from where the press sound is,
// over the part they share (zoom_in/zoom_out open with the press sound). Otherwise it plays from its
// start: power_on/power_off come with the 600 ms hold, after the 240 ms press sound, in step with
// the screen's timeline.
static void playPressAction(const char *name, bool carryOn) {
  const uint32_t now = millis();
  // A zoom from a physical press is already heard: the press sound on the down edge is its only
  // sound (playing the zoom effect on top of it sounded twice). Zooms from the web or CLI play it.
  if (carryOn && pressSeen && !pressUsed && now - pressAt < PRESS_FRESH_MS) {
    pressUsed = true;
    return;
  }
  playSfxFrom(name, 0, true);
}

static float nearestM() {
  float d = 1e30f;
  for (int i = 0; i < ballCount; i++) d = min(d, balls[i].distanceM);
  return d;
}

static uint32_t beepIntervalMs(float d) {
  const float lf = log10f(RADAR_BEEP_FAR_M), ln = log10f(RADAR_BEEP_NEAR_M);
  float t = clamp01((lf - log10f(max(d, 1e-3f))) / (lf - ln));
  return (uint32_t)lroundf(RADAR_BEEP_MAX_INTERVAL_MS +
                           ((float)RADAR_BEEP_MIN_INTERVAL_MS - (float)RADAR_BEEP_MAX_INTERVAL_MS) * powf(t, RADAR_BEEP_CURVE_POW));
}

// The beep loop (power on): interval from the nearest ball, close/found effects with re-arm, and the
// blink on every beep. Muted or without balls the dots pulse every RADAR_BLINK_PERIOD_MS.
static void beepStep(uint32_t now) {
  if (!ballCount || muted) {
    if (!blinkEver || (now - blinkAt) >= RADAR_BLINK_PERIOD_MS) {
      blinkAt = now;
      blinkEver = true;
    }
    if (ballCount) {  // keep the arms in step with the distance even while muted
      float d = nearestM();
      if (d > RADAR_BEEP_FOUND_M * RADAR_BEEP_REARM_FACTOR) foundArmed = true;
      if (d > RADAR_BEEP_CLOSE_M * RADAR_BEEP_REARM_FACTOR) closeArmed = true;
    }
    return;
  }
  if (pressGuard) {  // a press sound has priority: the beep waits for it
    if ((int32_t)(now - pressGuardUntil) < 0) return;
    pressGuard = false;
  }
  const float d = nearestM();
  const uint32_t iv = beepIntervalMs(d);
  const char *sfx = nullptr;
  if (foundArmed && d <= RADAR_BEEP_FOUND_M) {
    foundArmed = closeArmed = false;
    sfx = RADAR_BEEP_FOUND_SFX;
  } else if (closeArmed && d <= RADAR_BEEP_CLOSE_M) {
    closeArmed = false;
    sfx = RADAR_BEEP_CLOSE_SFX;
  }
  if (d > RADAR_BEEP_FOUND_M * RADAR_BEEP_REARM_FACTOR) foundArmed = true;
  if (d > RADAR_BEEP_CLOSE_M * RADAR_BEEP_REARM_FACTOR) closeArmed = true;
  if (sfx || !beepEver || (now - lastBeep) >= iv) {
    playSfx(sfx ? sfx : RADAR_BEEP_SFX);
    if (!sfx) beepCount++;
    lastBeep = blinkAt = now;
    beepEver = blinkEver = true;
  }
}

// ====================================================================== persistence

static void saveSettings() {
  Preferences p;
  p.begin(APP_PREFS_NS, false);
  p.putUChar("zoom", zoom);
  p.putUChar("theme", theme);
  p.putUChar("sweep", sweep);
  p.putBool("muted", muted);
  p.putUChar("vol", soundConfig().volume);
  p.end();
  savedVolume = soundConfig().volume;
}

static void saveBalls() {
  Preferences p;
  p.begin(APP_PREFS_NS, false);
  p.putBool("man", manual);
  p.putBytes("balls", balls, sizeof(Ball) * ballCount);
  p.end();
}

static void loadDemo() {
  ballCount = min(MAX_BALLS, (int)(sizeof(RADAR_DEMO_BALLS) / sizeof(RADAR_DEMO_BALLS[0])));
  for (int i = 0; i < ballCount; i++)
    balls[i] = {RADAR_DEMO_BALLS[i].stars, RADAR_DEMO_BALLS[i].bearingDeg, RADAR_DEMO_BALLS[i].distanceM};
  manual = false;
}

static bool validBall(const Ball &b) {
  return b.stars >= 1 && b.stars <= 7 && isfinite(b.bearingDeg) && b.bearingDeg >= 0 && b.bearingDeg < 360 &&
         isfinite(b.distanceM) && b.distanceM >= 0.1f && b.distanceM <= 100000;
}

static void loadSettings() {
  Preferences p;
  p.begin(APP_PREFS_NS, true);
  zoom = min<uint8_t>(RADAR_ZOOM_COUNT - 1, p.getUChar("zoom", RADAR_ZOOM_DEFAULT));
  theme = p.getUChar("theme", RADAR_THEME_DEFAULT);
  if (theme >= RADAR_THEME_COUNT) theme = RADAR_THEME_DEFAULT;
  sweep = p.getUChar("sweep", RADAR_SWEEP_DEFAULT);
  if (sweep >= RADAR_SWEEP_COUNT) sweep = RADAR_SWEEP_DEFAULT;
  muted = p.getBool("muted", false);
  int vol = p.isKey("vol") ? p.getUChar("vol", 60) : -1;
  manual = p.getBool("man", false);
  if (manual) {
    size_t n = p.getBytesLength("balls") / sizeof(Ball);
    ballCount = min<int>(MAX_BALLS, n);
    if (ballCount) p.getBytes("balls", balls, sizeof(Ball) * ballCount);
    for (int i = 0; i < ballCount; i++)
      if (!validBall(balls[i])) ballCount = 0;  // corrupt: start empty
  }
  p.end();
  if (!manual) loadDemo();
  if (vol >= 0 && vol <= 100 && vol != soundConfig().volume) {
    SoundConfig sc = soundConfig();
    sc.volume = vol;
    String err;
    if (soundConfigure(sc, err)) devSaveOutputs();
  }
  savedVolume = soundConfig().volume;
}

// ====================================================================== behaviour

static void zoomTo(int level, const char *sfx) {
  level = constrain(level, 0, RADAR_ZOOM_COUNT - 1);
  if (level == zoom) return;
  uint32_t now = millis();
  if (power == P_ON) {
    zoomFrom = zoom;
    zoomStart = now;
    labelAt = now;
    labelShown = true;
    if (sfx) playPressAction(sfx, true);
  }
  zoom = level;
  saveSettings();
}

void radarZoomOut() {
  if (power != P_ON) return;
  if (zoom < RADAR_ZOOM_COUNT - 1) zoomTo(zoom + 1, "zoom_out");
  else if (RADAR_ZOOM_WRAP) zoomTo(0, "zoom_in");
}

void radarZoomIn() {
  if (power != P_ON) return;
  if (zoom > 0) zoomTo(zoom - 1, "zoom_in");
  else if (RADAR_ZOOM_WRAP) zoomTo(RADAR_ZOOM_COUNT - 1, "zoom_out");
}

static void setTheme(int t) {
  t = ((t % RADAR_THEME_COUNT) + RADAR_THEME_COUNT) % RADAR_THEME_COUNT;
  if (t == theme) return;
  theme = t;
  fullPending = true;
  saveSettings();
}

void radarStepTheme(int dir) { setTheme(theme + (dir >= 0 ? 1 : -1)); }

static void bootNow() {
  power = P_BOOT;
  bootStart = millis();
  zoomFrom = -1;
  labelShown = false;
  closeArmed = foundArmed = true;
  beepEver = false;
  fullPending = true;
  playPressAction(RADAR_BOOT_SFX, false);
}

static void powerOff() {
  if (power == P_OFF || power == P_POWERING_OFF) return;
  power = P_POWERING_OFF;
  offStart = millis();
  playPressAction(RADAR_POWER_OFF_SFX, false);
}

void radarPowerToggle() {
  if (power == P_OFF) bootNow();
  else powerOff();
}

bool radarIsOn() { return power == P_ON || power == P_BOOT; }
bool radarReady() { return power == P_ON; }

void radarResetHeading() { motionResetYaw(); }

void radarPauseBeeps(bool on) {
  if (beepsPaused && !on) beepEver = false;  // resume with a beep straight away, as after the boot
  beepsPaused = on;
}

void radarSuspend(bool on) {
  if (suspended == on) return;
  suspended = on;
  if (on) {
    soundStop();
  } else {
    beepEver = false;  // a beep straight away, as after the boot
    radarInvalidate();
  }
}

bool radarSuspended() { return suspended; }

void radarCardBackground(uint16_t *dst, float keep) {
  if (!selectTheme()) {
    memset(dst, 0, W * H * 2);
    return;
  }
  const int k = (int)(clamp01(keep) * 256);
  for (int i = 0; i < W * H; i++) {
    const uint16_t c = bgPlain[i];
    dst[i] = pack((FROM5[c >> 11] * k) >> 8, (FROM6[(c >> 5) & 63] * k) >> 8, (FROM5[c & 31] * k) >> 8);
  }
  fb = dst;
  fbStride = W;
  clipR = {0, 0, W, H};
  finishRim();
  fb = nullptr;
}

void radarCardColors(uint32_t &text, uint32_t &accent, uint32_t &dim) {
  text = pal().text888;
  accent = pal().digits888;
  const Rgb t = rgbOf(pal().text888), g = rgbOf(pal().screenEdge888);
  const Rgb d = mixRgb(t, g, 0.45f);
  dim = ((uint32_t)d.r << 16) | ((uint32_t)d.g << 8) | (uint32_t)d.b;
}

void radarInvalidate() {
  fullPending = true;
  offDrawn = false;
}

bool radarPlaySfx(const char *name) {
  if (power == P_OFF || power == P_POWERING_OFF || sfxIdOf(name) < 0) return false;
  return playSfx(name);
}

// Panel brightness: the user's level, faded with the boot and power-off timelines (the preview's
// black overlay, done with the AMOLED's own brightness so the flickers keep their timing).
static void applyBrightness(uint32_t now) {
  float fade = 1;
  if (power == P_BOOT) {
    float t = (float)(now - bootStart);
    fade = rampf(t, RADAR_BOOT_SCREEN_FROM_MS, RADAR_BOOT_SCREEN_TO_MS);
    for (int i = 0; i < RADAR_BOOT_FLICKER_COUNT; i++)
      if (t >= RADAR_BOOT_FLICKER_AT_MS[i] && t < RADAR_BOOT_FLICKER_AT_MS[i] + RADAR_BOOT_FLICKER_MS) fade *= RADAR_BOOT_FLICKER_LEVEL;
  } else if (power == P_POWERING_OFF) {
    fade = 1 - rampf((float)(now - offStart), RADAR_POWER_OFF_BALLS_OUT_MS, RADAR_POWER_OFF_DURATION_MS);
  } else if (power == P_OFF) {
    fade = 0;
  }
  uint8_t level = (uint8_t)lroundf(devBacklightLevel(netIsNight()) * fade);
  if (level != lastBrightness) {
    lastBrightness = level;
    lcdSetBacklight(level);
  }
}

void radarBegin() {
  initTables();
  loadSettings();
  buildRimMap();
  buildAngleMap();
  pushBegin();
  selectTheme();  // before the boot timeline starts: it takes ~0.6 s
  statWindowStart = millis();
  bootNow();
  Serial.printf("[radar] %s, zoom %s, theme %s, %d balls (%s)\n", POWER_NAMES[power], RADAR_ZOOM[zoom].label, pal().name,
                ballCount, manual ? "manual" : "demo");
}

static void renderTimed(uint32_t now) {
  const uint32_t t = micros();
  if (statLastEndUs) statGapSumUs += t - statLastEndUs;
  render(now);
  statLastEndUs = micros();
  statRenderSumUs += statLastEndUs - t;
  statLoopN++;
}

void radarLoop(uint32_t now) {
  startReportStep();
  if (power == P_BOOT && (now - bootStart) >= RADAR_BOOT_DURATION_MS) {
    power = P_ON;
    sweepEpoch = now;  // the hand starts at north when the radar is ready
  }
  if (power == P_POWERING_OFF && (now - offStart) >= RADAR_POWER_OFF_DURATION_MS) power = P_OFF;
  if (power == P_ON && !beepsPaused && !suspended) beepStep(now);
  applyBrightness(now);
  if (soundConfig().volume != savedVolume) saveSettings();  // changed elsewhere (core 'vol')
  if (suspended) return;
  if ((now - lastFrameMs) < FRAME_MIN_MS) {
    if (power == P_ON && zoomFrom < 0) buildIdle();  // the other themes, a few rows at a time
    return;
  }
  lastFrameMs = now;
  renderTimed(now);
}

// ====================================================================== API

static void ballJson(JsonObject o, const Ball &b) {
  o["stars"] = b.stars;
  o["bearingDeg"] = roundf(b.bearingDeg * 10) / 10;
  o["distanceM"] = roundf(b.distanceM * 10) / 10;
}

void radarStateJson(JsonObject out) {
  out["power"] = POWER_NAMES[power];
  out["zoom"] = zoom;
  JsonArray levels = out["zoomLevels"].to<JsonArray>();
  for (int i = 0; i < RADAR_ZOOM_COUNT; i++) {
    JsonObject l = levels.add<JsonObject>();
    l["cellM"] = RADAR_ZOOM[i].cellM;
    l["label"] = RADAR_ZOOM[i].label;
  }
  out["heading"] = roundf(headingDeg() * 10) / 10;
  out["theme"] = pal().name;
  out["sweep"] = RADAR_SWEEP_NAMES[sweep];
  out["volume"] = soundConfig().volume;
  out["muted"] = muted;
  out["mode"] = manual ? "manual" : "demo";
  JsonArray arr = out["balls"].to<JsonArray>();
  for (int i = 0; i < ballCount; i++) ballJson(arr.add<JsonObject>(), balls[i]);
  if (ballCount) out["nearestM"] = roundf(nearestM() * 10) / 10;
  else out["nearestM"] = nullptr;
  if (ballCount && (power == P_ON || power == P_BOOT)) out["beepIntervalMs"] = beepIntervalMs(nearestM());
  else out["beepIntervalMs"] = nullptr;
}

static int sweepIndex(const char *name) {
  for (int i = 0; i < RADAR_SWEEP_COUNT; i++)
    if (name && !strcmp(RADAR_SWEEP_NAMES[i], name)) return i;
  return -1;
}

static void setSweep(int m) {
  if (m < 0 || m >= RADAR_SWEEP_COUNT || m == sweep) return;
  sweep = m;
  saveSettings();
}

static int themeIndex(const char *name) {
  for (int i = 0; i < RADAR_THEME_COUNT; i++)
    if (name && !strcmp(RADAR_THEMES[i].name, name)) return i;
  return -1;
}

static const char *setVolume(int v) {
  if (v < 0 || v > 100) return "volume must be 0-100";
  SoundConfig sc = soundConfig();
  sc.volume = v;
  String err;
  if (!soundConfigure(sc, err)) return "could not set the volume";
  devSaveOutputs();
  saveSettings();
  return nullptr;
}

const char *radarApply(JsonObjectConst in) {
  // Validate everything first, then apply, so a bad field changes nothing.
  JsonVariantConst z = in["zoom"], t = in["theme"], v = in["volume"], m = in["muted"], pw = in["power"], rh = in["resetHeading"];
  JsonVariantConst sw = in["sweep"];
  if (!z.isNull() && (!z.is<int>() || z.as<int>() < 0 || z.as<int>() >= RADAR_ZOOM_COUNT)) return "zoom must be 0-3";
  if (!t.isNull() && themeIndex(t.as<const char *>()) < 0) return "unknown theme";
  if (!sw.isNull() && sweepIndex(sw.as<const char *>()) < 0) return "sweep must be line, ring, both or none";
  if (!v.isNull() && (!v.is<int>() || v.as<int>() < 0 || v.as<int>() > 100)) return "volume must be 0-100";
  if (!m.isNull() && !m.is<bool>()) return "muted must be true or false";
  const char *ps = pw.isNull() ? nullptr : pw.as<const char *>();
  if (!pw.isNull() && (!ps || (strcmp(ps, "on") && strcmp(ps, "off")))) return "power must be on or off";
  if (!rh.isNull() && !rh.is<bool>()) return "resetHeading must be true or false";

  if (ps && !strcmp(ps, "off")) powerOff();
  if (!t.isNull()) setTheme(themeIndex(t.as<const char *>()));
  if (!sw.isNull()) setSweep(sweepIndex(sw.as<const char *>()));
  if (!z.isNull()) {
    int target = z.as<int>();
    zoomTo(target, target > zoom ? "zoom_out" : "zoom_in");
  }
  if (!m.isNull() && m.as<bool>() != muted) {
    muted = m.as<bool>();
    if (muted) soundStop();
    saveSettings();
  }
  if (!v.isNull()) setVolume(v.as<int>());
  if (rh.as<bool>()) radarResetHeading();
  if (ps && !strcmp(ps, "on") && power == P_OFF) bootNow();
  return nullptr;
}

const char *radarSetBalls(JsonObjectConst in) {
  if (in["demo"].as<bool>()) {
    loadDemo();
    saveBalls();
    return nullptr;
  }
  JsonArrayConst arr = in["balls"].as<JsonArrayConst>();
  if (in["balls"].isNull() || !in["balls"].is<JsonArrayConst>()) return "expected balls or demo";
  if ((int)arr.size() > MAX_BALLS) return "at most 7 balls";
  Ball tmp[MAX_BALLS];
  int n = 0;
  for (JsonVariantConst b : arr) {
    Ball x = {(uint8_t)(b["stars"] | 0), b["bearingDeg"] | -1.0f, b["distanceM"] | -1.0f};
    if (!b["stars"].is<int>() || !validBall(x)) return "each ball needs stars 1-7, bearingDeg 0-359.9, distanceM 0.1-100000";
    tmp[n++] = x;
  }
  memcpy(balls, tmp, sizeof(Ball) * n);
  ballCount = n;
  manual = true;
  saveBalls();
  return nullptr;
}

// ====================================================================== CLI

void radarCliHelp(Print &out) {
  out.println(
      "radar:    radar | zoom <0-3> | theme <z|classic|ptp> | sweep <line|ring|both|none> | mute [on|off]\n"
      "          power [on|off] | yaw0\n"
      "          ball <stars> <bearingDeg> <distanceM> | balls clear|demo | sfx <name> | vol <0-100>\n"
      "          api radar|balls <json> | blink <0-1>|live | heading <deg>|gyro\n"
      "          still boot <ms>|zoom <from> <to> <p>|sweep <deg>|ring <p>|off\n"
      "          (test aids for make shot)");
}

static void printRadar() {
  JsonDocument doc;
  radarStateJson(doc.to<JsonObject>());
  serializeJson(doc, Serial);
  Serial.println();
  Serial.printf("beeps: %u since boot, uptime %u ms, loop stack free %u bytes\n", (unsigned)beepCount,
                (unsigned)millis(), (unsigned)uxTaskGetStackHighWaterMark(nullptr));
  Serial.printf("render: %.1f fps, last frame compose %.1f ms flush %.1f ms (%u px), %u frames (%u full)\n", statFps,
                statComposeUs / 1000.0f, statFlushUs / 1000.0f, (unsigned)statPixels, (unsigned)statFrames,
                (unsigned)statFull);
  Serial.printf("render: partial frames avg %.1f ms max %.1f ms (%u since the last report); last full frame compose %.1f ms "
                "flush %.1f ms\n",
                statPartN ? statPartSumUs / 1000.0f / statPartN : 0.0f, statPartMaxUs / 1000.0f, (unsigned)statPartN,
                statFullComposeUs / 1000.0f, statFullFlushUs / 1000.0f);
  Serial.printf("render: full frames take %.1f ms wall time (worst %.1f ms since the last report); last frame: "
                "describe + dirty pass %.1f ms, %u rectangles\n",
                statFullFrameUs / 1000.0f, statFullMaxUs / 1000.0f, statPrepUs / 1000.0f, (unsigned)statRects);
  if (statLoopN)
    Serial.printf("render: per call %.1f ms in render(), %.1f ms in the rest of the loop between calls (%u calls)\n",
                  statRenderSumUs / 1000.0f / statLoopN, statGapSumUs / 1000.0f / statLoopN, (unsigned)statLoopN);
  statRenderSumUs = statGapSumUs = statLoopN = 0;
  statPartSumUs = statPartN = statPartMaxUs = statFullMaxUs = 0;
  for (int k = 0; k < 2; k++) {
    Serial.print(k ? "render: last full frame by stage (us):" : "render: last frame by stage (us):");
    for (int i = 0; i < 8; i++) Serial.printf(" %s=%u", PROF_NAMES[i], (unsigned)(k ? profFullUs : profUs)[i]);
    Serial.println();
  }
}

bool radarCli(const String &cmd, const String &args) {
  String a = args;
  a.toLowerCase();
  if (cmd == "radar") {
    printRadar();
  } else if (cmd == "zoom") {
    if (a.isEmpty()) Serial.printf("zoom=%u (%s)\n", zoom, RADAR_ZOOM[zoom].label);
    else if (a == "out") radarZoomOut();
    else if (a == "in") radarZoomIn();
    else if (a.length() != 1 || a[0] < '0' || a[0] >= '0' + RADAR_ZOOM_COUNT) {
      Serial.println("usage: zoom <0-3>|in|out");
      return true;
    } else {
      int z = a.toInt();
      zoomTo(z, z > zoom ? "zoom_out" : "zoom_in");
    }
    Serial.printf("zoom=%u (%s)\n", zoom, RADAR_ZOOM[zoom].label);
  } else if (cmd == "theme") {
    if (a == "next" || a == "prev") radarStepTheme(a == "next" ? 1 : -1);
    else if (a.length() && themeIndex(a.c_str()) < 0) {
      Serial.println("usage: theme z|classic|ptp|next|prev");
      return true;
    } else if (a.length()) setTheme(themeIndex(a.c_str()));
    Serial.printf("theme=%s\n", pal().name);
  } else if (cmd == "sweep") {
    if (a.length() && sweepIndex(a.c_str()) < 0) {
      Serial.println("usage: sweep line|ring|both|none");
      return true;
    }
    if (a.length()) setSweep(sweepIndex(a.c_str()));
    Serial.printf("sweep=%s\n", RADAR_SWEEP_NAMES[sweep]);
  } else if (cmd == "mute") {
    bool m = a == "on" ? true : a == "off" ? false : !muted;
    if (a.length() && a != "on" && a != "off") {
      Serial.println("usage: mute [on|off]");
      return true;
    }
    if (m != muted) {
      muted = m;
      if (muted) soundStop();
      saveSettings();
    }
    Serial.printf("muted=%d\n", muted);
  } else if (cmd == "power") {
    if (a == "on" && power == P_OFF) bootNow();
    else if (a == "off") powerOff();
    else if (a.isEmpty()) radarPowerToggle();
    else if (a != "on") {
      Serial.println("usage: power [on|off]");
      return true;
    }
    Serial.printf("power=%s\n", POWER_NAMES[power]);
  } else if (cmd == "yaw0") {
    radarResetHeading();
    Serial.println("ok heading reset");
  } else if (cmd == "ball") {
    int stars = 0;
    float bearing = -1, dist = -1;
    Ball b;
    if (sscanf(a.c_str(), "%d %f %f", &stars, &bearing, &dist) != 3 ||
        !validBall(b = {(uint8_t)stars, bearing, dist})) {
      Serial.println("usage: ball <stars 1-7> <bearingDeg 0-359.9> <distanceM 0.1-100000>");
      return true;
    }
    if (!manual) ballCount = 0;  // the first manual ball replaces the demo
    int i = 0;
    while (i < ballCount && balls[i].stars != b.stars) i++;
    if (i == ballCount && ballCount == MAX_BALLS) {
      Serial.println("error: 7 balls already");
      return true;
    }
    balls[i] = b;
    if (i == ballCount) ballCount++;
    manual = true;
    saveBalls();
    Serial.printf("ok %d balls (manual), nearest %.1f m\n", ballCount, nearestM());
  } else if (cmd == "balls") {
    if (a == "clear") {
      ballCount = 0;
      manual = true;
    } else if (a == "demo") {
      loadDemo();
    } else {
      Serial.println("usage: balls clear|demo");
      return true;
    }
    saveBalls();
    Serial.printf("ok %d balls (%s)\n", ballCount, manual ? "manual" : "demo");
  } else if (cmd == "sfx") {
    if (sfxIdOf(a.c_str()) < 0) return false;  // the common 'sfx' (list, on/off) takes the rest
    Serial.println(radarPlaySfx(a.c_str()) ? "ok" : "not played (radar off or muted)");
  } else if (cmd == "blink") {
    if (a == "live" || a.isEmpty()) blinkFreeze = -1;
    else blinkFreeze = clamp01(a.toFloat());
    Serial.printf("blink=%s\n", blinkFreeze < 0 ? "live" : String(blinkFreeze, 2).c_str());
  } else if (cmd == "shot") {
    // The radar sends its frames to the panel without the canvas: fill it, then the common 'shot'
    // dumps it. While another screen has the panel, the canvas already holds that screen.
    if (suspended) {
      return false;
    } else if (power == P_OFF && stillKind == 0) {
      if (lcdCanvas()) memset(lcdCanvas()->getFramebuffer(), 0, W * H * 2);
    } else {
      composeToCanvas();
    }
    return false;
  } else if (cmd == "api") {
    // api radar|balls <json>: the functions behind POST /api/radar and /api/balls, for tests
    // without WiFi (the HTTP routes add their own validation in radar_api.cpp first).
    int sp = args.indexOf(' ');
    String which = sp < 0 ? args : args.substring(0, sp);
    JsonDocument doc;
    if (sp < 0 || deserializeJson(doc, args.substring(sp + 1)) || !doc.is<JsonObject>()) {
      Serial.println("usage: api radar|balls {json}");
      return true;
    }
    const char *err = which == "radar" ? radarApply(doc.as<JsonObjectConst>())
                    : which == "balls" ? radarSetBalls(doc.as<JsonObjectConst>())
                                       : "use api radar or api balls";
    if (err) Serial.printf("error: %s\n", err);
    else printRadar();
  } else if (cmd == "still") {
    int a1 = 0, a2 = 0;
    float a3 = 0;
    if (a.startsWith("boot") && sscanf(a.c_str() + 4, "%d", &a1) == 1) {
      stillKind = 1;
      stillBootT = constrain(a1, 0, (int)RADAR_BOOT_DURATION_MS);
    } else if (a.startsWith("zoom") && sscanf(a.c_str() + 4, "%d %d %f", &a1, &a2, &a3) == 3 && a1 >= 0 &&
               a1 < RADAR_ZOOM_COUNT && a2 >= 0 && a2 < RADAR_ZOOM_COUNT) {
      stillKind = 2;
      stillFrom = a1;
      zoom = a2;
      stillP = clamp01(a3);
    } else if (a.startsWith("sweep") && sscanf(a.c_str() + 5, "%f", &a3) == 1) {
      stillSweepDeg = wrap360(a3);  // the line frozen at this angle (live boot/zoom unaffected)
    } else if (a.startsWith("ring") && sscanf(a.c_str() + 4, "%f", &a3) == 1) {
      stillRingP = clamp01(a3);     // the ring frozen at this progress
    } else if (a == "off" || a.isEmpty()) {
      stillKind = 0;
      stillSweepDeg = stillRingP = -1;
    } else {
      Serial.println("usage: still boot <ms> | still zoom <from> <to> <0-1> | still sweep <deg> | still ring <0-1> | still off");
      return true;
    }
    radarInvalidate();
    Serial.printf("still=%s sweepDeg=%.1f ringP=%.2f\n", stillKind == 1 ? "boot" : stillKind == 2 ? "zoom" : "off",
                  stillSweepDeg, stillRingP);
  } else if (cmd == "heading") {
    if (a == "gyro" || a.isEmpty()) headingFreeze = -1;
    else headingFreeze = wrap360(a.toFloat());
    Serial.printf("heading=%.1f (%s)\n", headingDeg(), headingFreeze < 0 ? "gyro" : "fixed");
  } else {
    return false;
  }
  return true;
}
