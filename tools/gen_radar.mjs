#!/usr/bin/env node
// Generates firmware/dragonradar/radar_assets.h from shared/radar.json (look, geometry, timings,
// stroke font, demo balls) and the behaviour fields of shared/sounds.json that the common
// sounds.h does not carry (per-effect gain, beep interval mapping).
// Lengths stay relative to the screen radius R (multiply by the panel radius at run time); colours
// are emitted as RGB565 for the panel and RGB888 for blending.
// --check: fails when the committed header does not match the JSON.
import { readFileSync, writeFileSync, existsSync, mkdirSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
const rel = 'firmware/dragonradar/radar_assets.h';
const radar = JSON.parse(readFileSync(join(root, 'shared/radar.json'), 'utf8'));
const sounds = JSON.parse(readFileSync(join(root, 'shared/sounds.json'), 'utf8'));

// ------------------------------------------------------------------ validation
const fail = (msg) => { throw new Error(`radar.json/sounds.json: ${msg}`); };
const isHex = (c) => typeof c === 'string' && /^#[0-9a-fA-F]{6}$/.test(c);
const num = (v, name, lo = -Infinity, hi = Infinity) => {
  if (typeof v !== 'number' || !Number.isFinite(v) || v < lo || v > hi) fail(`${name} must be a number in ${lo}..${hi}, got ${v}`);
  return v;
};

const COLOR_KEYS = ['background', 'screen', 'screenCenter', 'screenEdge', 'rim', 'grid', 'ballCore', 'ballDim',
  'ballGlow', 'marker', 'markerEdge', 'digits', 'arrow', 'text', 'ping', 'sheen', 'sweep'];
const COUNTS = ['none', 'clusters', 'all', 'stars'];  // RadarCounts order
const SWEEP_MODES = ['line', 'ring', 'both', 'none'];  // the firmware knows these four
const themeNames = Object.keys(radar.themes || {});
if (!themeNames.length) fail('no themes');
if (!radar.themes[radar.theme]) fail(`default theme "${radar.theme}" is not defined`);
for (const t of themeNames) {
  if (!/^[a-z][a-z0-9_]*$/.test(t)) fail(`theme name "${t}" must be lowercase`);
  for (const k of COLOR_KEYS) if (!isHex(radar.themes[t][k])) fail(`themes.${t}.${k} must be #RRGGBB`);
  if (!COUNTS.includes(radar.themes[t].counts)) fail(`themes.${t}.counts must be one of ${COUNTS.join(', ')}`);
}

const levels = radar.zoom.levels;
if (!Array.isArray(levels) || levels.length < 2 || levels.length > 8) fail('zoom.levels needs 2-8 levels');
levels.forEach((l, i) => {
  num(l.cellM, `zoom.levels[${i}].cellM`, 0.01);
  num(l.pitchR, `zoom.levels[${i}].pitchR`, 0.02, 1);
  num(l.lineWidthR, `zoom.levels[${i}].lineWidthR`, 0.001, 0.1);
  for (const ch of String(l.label)) if (!(ch in radar.font.glyphs)) fail(`zoom.levels[${i}].label: "${ch}" is not in the stroke font`);
  if (i && levels[i - 1].cellM / levels[i - 1].pitchR >= l.cellM / l.pitchR) fail('zoom levels must widen the view one after another');
});
num(radar.zoom.default, 'zoom.default', 0, levels.length - 1);

const b = radar.boot;
const order = ['screenFromMs', 'screenToMs', 'gridFromMs', 'gridToMs', 'markerFromMs', 'markerToMs', 'pingFromMs',
  'pingToMs', 'ballsFromMs', 'ballsToMs'];
order.forEach((k, i) => {
  num(b[k], `boot.${k}`, 0, b.durationMs);
  if (i && b[k] < b[order[i - 1]]) fail(`boot.${k} must not come before boot.${order[i - 1]}`);
});
if (!Array.isArray(b.flickerAtMs) || b.flickerAtMs.length > 4) fail('boot.flickerAtMs: up to 4 times');

const sfxNames = new Set(sounds.sfx.map((s) => s.name));
for (const [where, name] of [['boot.sfx', b.sfx], ['transitions.powerOff.sfx', radar.transitions.powerOff.sfx],
  ['beep.sfx', sounds.beep.sfx], ['beep.closeSfx', sounds.beep.closeSfx], ['beep.foundSfx', sounds.beep.foundSfx]])
  if (!sfxNames.has(name)) fail(`${where} names an unknown sound "${name}"`);
for (const s of sounds.sfx) {
  num(s.gain, `sfx ${s.name}.gain`, 0, 100);
  // The preview and render_wav.mjs synthesize the raw-tone subset only; keep the catalogue in it.
  for (const tok of s.notes.trim().split(/\s+/))
    if (!/^(~square|~sine|~tri|R:\d+|\d+:\d+)$/.test(tok)) fail(`sfx ${s.name}: token "${tok}" is outside HZ:MS, R:MS, ~square, ~sine, ~tri`);
}
const sy = sounds.synth;
if (![24000, 32000, 44100, 48000].includes(sy.sampleRate)) fail('synth.sampleRate must be 24000, 32000, 44100 or 48000');
num(sy.rampMs, 'synth.rampMs', 0.1, 50);
num(sy.levelAt100, 'synth.levelAt100', 0.05, 1);
num(sy.rangeDb, 'synth.rangeDb', 6, 90);
num(sy.curveShape, 'synth.curveShape', 0.2, 5);
const bp = sounds.beep;
num(bp.nearM, 'beep.nearM', 0.01);
num(bp.farM, 'beep.farM', bp.nearM * 1.0001);
num(bp.minIntervalMs, 'beep.minIntervalMs', 30);
num(bp.maxIntervalMs, 'beep.maxIntervalMs', bp.minIntervalMs);

const sw = radar.sweep;
if (!Array.isArray(sw.modes) || sw.modes.length !== SWEEP_MODES.length || !SWEEP_MODES.every((m) => sw.modes.includes(m)))
  fail(`sweep.modes must list ${SWEEP_MODES.join(', ')}`);
if (!sw.modes.includes(sw.mode)) fail(`sweep.mode "${sw.mode}" is not in sweep.modes`);
num(sw.line.periodMs, 'sweep.line.periodMs', 200);
num(sw.line.trailDeg, 'sweep.line.trailDeg', 0, 180);  // the firmware draws the trail as one convex wedge
num(sw.line.alpha, 'sweep.line.alpha', 0, 1);
num(sw.line.trailAlpha, 'sweep.line.trailAlpha', 0, 1);
num(sw.line.lineWidthR, 'sweep.line.lineWidthR', 0.001, 0.1);
num(sw.ring.durationMs, 'sweep.ring.durationMs', 50);
num(sw.ring.widthR, 'sweep.ring.widthR', 0.001, 0.2);
num(sw.ring.alpha, 'sweep.ring.alpha', 0, 1);
num(sw.ring.maxR, 'sweep.ring.maxR', 0.05, 1.5);

const font = radar.font;
const glyphs = Object.entries(font.glyphs);
for (const [ch, strokes] of glyphs) {
  if (ch.length !== 1 || ch.charCodeAt(0) > 126) fail(`font glyph "${ch}" must be one ASCII character`);
  for (const s of strokes) {
    if (s.length < 2) fail(`font glyph "${ch}": a stroke needs 2+ points`);
    for (const [x, y] of s) { num(x, `glyph ${ch} x`, 0, font.boxW); num(y, `glyph ${ch} y`, 0, font.boxH); }
  }
}
const balls = radar.demo.balls;
if (balls.length !== 7) fail('demo.balls must list the 7 dragon balls');

// ------------------------------------------------------------------ emitters
const rgb = (hex) => parseInt(hex.slice(1), 16);
const rgb565 = (hex) => {
  const n = rgb(hex);
  return (((n >> 16) & 0xf8) << 8) | (((n >> 8) & 0xfc) << 3) | ((n & 0xff) >> 3);
};
const h16 = (v) => '0x' + v.toString(16).toUpperCase().padStart(4, '0');
const h24 = (v) => '0x' + v.toString(16).toUpperCase().padStart(6, '0');
const f = (v) => {  // float literal, shortest round-trip form
  const s = String(+v.toPrecision(7));
  return (/[.e]/.test(s) ? s : s + '.0') + 'f';
};
const C = (name) => name.replace(/([a-z0-9])([A-Z])/g, '$1_$2').toUpperCase();
const cstr = (v) => JSON.stringify(v);

let h = `// GENERATED by tools/gen_radar.mjs from shared/radar.json and shared/sounds.json. Do not edit;
// run \`make gen\`. Lengths are fractions of the screen radius R; times are milliseconds.
#pragma once
#include <stdint.h>

#define RADAR_REF_SIZE_PX ${radar.refSizePx}

// ---- palette (theme order below; RADAR_THEME_DEFAULT is the one used at boot)
struct RadarPalette {
  const char *name;
  const char *label;
${COLOR_KEYS.map((k) => `  uint16_t ${k};`).join('\n')}
${COLOR_KEYS.map((k) => `  uint32_t ${k}888;`).join('\n')}
  uint8_t counts;  // RadarCounts: which dots carry a ball count
};

// stars: each dot shows the star number of its (nearest) ball; all: the ball count of every dot;
// clusters: the count of merged dots only; none: no digits.
enum RadarCounts : uint8_t { RADAR_COUNTS_NONE, RADAR_COUNTS_CLUSTERS, RADAR_COUNTS_ALL, RADAR_COUNTS_STARS };

enum RadarThemeId : uint8_t {
${themeNames.map((t) => `  RADAR_THEME_${t.toUpperCase()},`).join('\n')}
  RADAR_THEME_COUNT
};
#define RADAR_THEME_DEFAULT RADAR_THEME_${radar.theme.toUpperCase()}

static const RadarPalette RADAR_THEMES[RADAR_THEME_COUNT] = {
${themeNames.map((t) => {
  const p = radar.themes[t];
  return `    {${cstr(t)}, ${cstr(p.label)},\n     ${COLOR_KEYS.map((k) => h16(rgb565(p[k]))).join(', ')},\n     ${COLOR_KEYS.map((k) => h24(rgb(p[k]))).join(', ')},
     RADAR_COUNTS_${p.counts.toUpperCase()}},`;
}).join('\n')}
};

// ---- screen shading
static constexpr float RADAR_SHADE_MID_STOP = ${f(radar.shading.midStop)};
static constexpr float RADAR_RIM_WIDTH_R = ${f(radar.shading.rimWidthR)};
#define RADAR_SHEEN_ENABLED ${radar.shading.sheen.enabled ? 1 : 0}
static constexpr float RADAR_SHEEN_CX_R = ${f(radar.shading.sheen.cxR)};
static constexpr float RADAR_SHEEN_CY_R = ${f(radar.shading.sheen.cyR)};
static constexpr float RADAR_SHEEN_RX_R = ${f(radar.shading.sheen.rxR)};
static constexpr float RADAR_SHEEN_RY_R = ${f(radar.shading.sheen.ryR)};
static constexpr float RADAR_SHEEN_ANGLE_DEG = ${f(radar.shading.sheen.angleDeg)};
static constexpr float RADAR_SHEEN_ALPHA = ${f(radar.shading.sheen.alpha)};

// ---- grid and zoom levels (the top button steps through them)
static constexpr float RADAR_GRID_OFFSET_CELLS = ${f(radar.grid.offsetCells)};
#define RADAR_GRID_ROTATES ${radar.grid.rotateWithHeading ? 1 : 0}

struct RadarZoomLevel {
  float cellM;       // real distance per grid cell, metres
  float pitchR;      // cell size on screen
  float lineWidthR;  // grid line width
  const char *label;
};

#define RADAR_ZOOM_COUNT ${levels.length}
#define RADAR_ZOOM_DEFAULT ${radar.zoom.default}
#define RADAR_ZOOM_WRAP ${radar.zoom.wrap ? 1 : 0}
static const RadarZoomLevel RADAR_ZOOM[RADAR_ZOOM_COUNT] = {
${levels.map((l) => `    {${f(l.cellM)}, ${f(l.pitchR)}, ${f(l.lineWidthR)}, ${cstr(l.label)}},`).join('\n')}
};

// ---- centre marker (the user), balls, edge arrows
`;
const block = (prefix, obj, skip = []) => Object.entries(obj)
  .filter(([k, v]) => k !== 'comment' && !skip.includes(k) && typeof v === 'number')
  .map(([k, v]) => scalar(`${prefix}_${C(k)}`, k, v)).join('\n') + '\n';
// Times (keys ending in Ms) and counts are integers; everything else is a float.
function scalar(name, key, v) {
  if (/(Ms|Blinks)$/.test(key)) {
    if (!Number.isInteger(v) || v < 0) fail(`${key} must be a whole number, got ${v}`);
    return `static constexpr uint32_t ${name} = ${v};`;
  }
  return `static constexpr float ${name} = ${f(v)};`;
}
h += block('RADAR_MARKER', radar.marker);
h += block('RADAR_BALL', radar.ball);
h += block('RADAR_ARROW', radar.arrow);
h += `
// ---- blink (all dots flash on each beep, then decay to the dim level)
#define RADAR_BLINK_SYNC_TO_BEEP ${radar.blink.syncToBeep ? 1 : 0}
${block('RADAR_BLINK', radar.blink, ['syncToBeep'])}
// ---- scale label after a zoom change (adaptation)
#define RADAR_SCALE_LABEL_ENABLED ${radar.scaleLabel.enabled ? 1 : 0}
${block('RADAR_SCALE_LABEL', radar.scaleLabel, ['enabled'])}
// ---- transitions
${block('RADAR_ZOOM_ANIM', radar.transitions.zoom)}#define RADAR_ZOOM_ANIM_EASING ${cstr(radar.transitions.zoom.easing)}
${block('RADAR_POWER_OFF', radar.transitions.powerOff)}#define RADAR_POWER_OFF_SFX ${cstr(radar.transitions.powerOff.sfx)}

// ---- boot timeline (ms from power-on)
${block('RADAR_BOOT', b).trimEnd()}
#define RADAR_BOOT_FLICKER_COUNT ${b.flickerAtMs.length}
static const uint16_t RADAR_BOOT_FLICKER_AT_MS[${Math.max(1, b.flickerAtMs.length)}] = {${b.flickerAtMs.join(', ') || '0'}};
#define RADAR_BOOT_SFX ${cstr(b.sfx)}

// ---- scan effect (sweep). line: a clockwise hand with a fading trail; ring: a ring that grows on
// every beep; both; none. RADAR_SWEEP_DEFAULT is the mode at first boot (none when disabled).
#define RADAR_SWEEP_ENABLED ${sw.enabled ? 1 : 0}
enum RadarSweepMode : uint8_t {
${sw.modes.map((m) => `  RADAR_SWEEP_${m.toUpperCase()},`).join('\n')}
  RADAR_SWEEP_COUNT
};
static const char *const RADAR_SWEEP_NAMES[RADAR_SWEEP_COUNT] = {${sw.modes.map(cstr).join(', ')}};
#define RADAR_SWEEP_DEFAULT RADAR_SWEEP_${(sw.enabled ? sw.mode : 'none').toUpperCase()}
${block('RADAR_SWEEP_LINE', sw.line, ['dotFlash'])}#define RADAR_SWEEP_LINE_DOT_FLASH ${sw.line.dotFlash ? 1 : 0}
${block('RADAR_SWEEP_RING', sw.ring, ['syncToBeep'])}#define RADAR_SWEEP_RING_SYNC_TO_BEEP ${sw.ring.syncToBeep ? 1 : 0}

// ---- radar beep (from shared/sounds.json): interval from the distance of the nearest ball
#define RADAR_BEEP_SFX ${cstr(bp.sfx)}
static constexpr float RADAR_BEEP_NEAR_M = ${f(bp.nearM)};
static constexpr float RADAR_BEEP_FAR_M = ${f(bp.farM)};
${scalar('RADAR_BEEP_MIN_INTERVAL_MS', 'minIntervalMs', bp.minIntervalMs)}
${scalar('RADAR_BEEP_MAX_INTERVAL_MS', 'maxIntervalMs', bp.maxIntervalMs)}
static constexpr float RADAR_BEEP_CURVE_POW = ${f(bp.curvePow)};
static constexpr float RADAR_BEEP_CLOSE_M = ${f(bp.closeM)};
#define RADAR_BEEP_CLOSE_SFX ${cstr(bp.closeSfx)}
static constexpr float RADAR_BEEP_FOUND_M = ${f(bp.foundM)};
#define RADAR_BEEP_FOUND_SFX ${cstr(bp.foundSfx)}
static constexpr float RADAR_BEEP_REARM_FACTOR = ${f(bp.rearmFactor)};

// Synth rule (shared/sounds.json synth), read by sound.cpp through config.h: level of full scale at
// volume v = LEVEL_AT_100 x 10^(-RANGE_DB x (1 - v/100)^CURVE_SHAPE / 20).
#define RADAR_SYNTH_SAMPLE_RATE ${sy.sampleRate}
#define RADAR_SYNTH_RAMP_MS ${f(sy.rampMs)}
#define RADAR_SYNTH_LEVEL_AT_100 ${f(sy.levelAt100)}
#define RADAR_SYNTH_RANGE_DB ${f(sy.rangeDb)}
#define RADAR_SYNTH_CURVE_SHAPE ${f(sy.curveShape)}

// Per-effect gain, percent of the user volume (same order as SfxId in sounds.h).
struct RadarSfxGain {
  const char *name;
  uint8_t gain;
};
#define RADAR_SFX_GAIN_COUNT ${sounds.sfx.length}
static const RadarSfxGain RADAR_SFX_GAIN[RADAR_SFX_GAIN_COUNT] = {
${sounds.sfx.map((s) => `    {${cstr(s.name)}, ${s.gain}},`).join('\n')}
};

// ---- stroke font: polylines on a ${font.boxW} x ${font.boxH} box; points are (x, y) pairs
`;
const pts = [], strokes = [], glyphRows = [];
for (const [ch, ss] of glyphs) {
  const first = strokes.length;
  for (const s of ss) {
    strokes.push([pts.length / 2, s.length]);
    for (const [x, y] of s) pts.push(x, y);
  }
  glyphRows.push([ch, first, ss.length]);
}
h += `static constexpr float RADAR_FONT_BOX_W = ${f(font.boxW)};
static constexpr float RADAR_FONT_BOX_H = ${f(font.boxH)};
static constexpr float RADAR_FONT_ADVANCE = ${f(font.advance)};
static const float RADAR_FONT_POINTS[${pts.length}] = {
${chunk(pts.map(f), 10).map((r) => '    ' + r.join(', ') + ',').join('\n')}
};
struct RadarStroke {
  uint16_t first;  // index of the first point (pairs in RADAR_FONT_POINTS)
  uint8_t count;
};
static const RadarStroke RADAR_FONT_STROKES[${strokes.length}] = {
${chunk(strokes.map(([a, n]) => `{${a}, ${n}}`), 8).map((r) => '    ' + r.join(', ') + ',').join('\n')}
};
struct RadarGlyph {
  char ch;
  uint8_t firstStroke;
  uint8_t strokeCount;
};
#define RADAR_FONT_GLYPH_COUNT ${glyphRows.length}
static const RadarGlyph RADAR_FONT_GLYPHS[RADAR_FONT_GLYPH_COUNT] = {
${glyphRows.map(([c, a, n]) => `    {'${c}', ${a}, ${n}},`).join('\n')}
};

// ---- demo: the seven balls of the preview (bearing clockwise from north, distance in metres)
struct RadarDemoBall {
  uint8_t stars;
  float bearingDeg;
  float distanceM;
};
static const RadarDemoBall RADAR_DEMO_BALLS[7] = {
${balls.map((x) => `    {${x.stars}, ${f(x.bearingDeg)}, ${f(x.distanceM)}},`).join('\n')}
};
`;

function chunk(arr, n) {
  const out = [];
  for (let i = 0; i < arr.length; i += n) out.push(arr.slice(i, i + n));
  return out;
}

const file = join(root, rel);
if (process.argv.includes('--check')) {
  if (!existsSync(file) || readFileSync(file, 'utf8') !== h) {
    console.error(`${rel} is stale: run make gen`);
    process.exit(1);
  }
  console.log(`${rel} up to date`);
} else {
  mkdirSync(dirname(file), { recursive: true });
  writeFileSync(file, h);
  console.log(`${rel}: ${themeNames.length} themes, ${levels.length} zoom levels, ${glyphRows.length} glyphs`);
}
