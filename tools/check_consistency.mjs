#!/usr/bin/env node
// Fails when the radar's names drift between the contract (docs/api.md), the shared assets
// (shared/radar.json, shared/sounds.json), the generated headers (radar_assets.h, sounds.h), the
// HTTP routes (radar_api.cpp, and radar.cpp once it exists), the web panel, the MCP and the landing
// (docs/index.html: its theme, sweep, zoom and sound lists, their labels and the long press). Checked:
// themes, sweep modes, digit modes, zoom levels, sound names, the API field names (state object,
// POST bodies, ball) and
// their ranges, and that the pages embed the current renderer and shared JSON. No dependencies.
import { existsSync, readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { PAGES, buildPage } from './preview/build.mjs';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
const read = (p) => readFileSync(join(root, p), 'utf8');
const FW = 'firmware/dragonradar';
const errors = [];
const fail = (msg) => errors.push(msg);

// ------------------------------------------------------------------ extractors
function jsArray(file, name) {
  const m = read(file).match(new RegExp(`const ${name} = \\[([^\\]]*)\\]`));
  if (!m) throw new Error(`${name} not found in ${file}`);
  return [...m[1].matchAll(/'([^']*)'/g)].map((x) => x[1]);
}
function jsNumber(file, name) {
  const m = read(file).match(new RegExp(`\\b${name} = (-?[\\d.]+)`));
  if (!m) throw new Error(`${name} not found in ${file}`);
  return +m[1];
}
function cArray(file, name) {
  const m = read(file).match(new RegExp(`\\b${name}\\[\\] = \\{([^}]*)\\}`));
  if (!m) throw new Error(`${name} not found in ${file}`);
  return [...m[1].matchAll(/"([^"]*)"/g)].map((x) => x[1]);
}
function cNumber(file, name) {
  const m = read(file).match(new RegExp(`\\b${name} = (-?[\\d.]+)f?;`)) || read(file).match(new RegExp(`#define ${name} (-?[\\d.]+)`));
  if (!m) throw new Error(`${name} not found in ${file}`);
  return +m[1];
}
function cEnum(file, name, prefix) {
  const m = read(file).match(new RegExp(`enum ${name}[^{]*\\{([^}]*)\\}`));
  if (!m) throw new Error(`enum ${name} not found in ${file}`);
  return [...m[1].matchAll(new RegExp(`\\b${prefix}([A-Z0-9_]+)\\b`, 'g'))].map((x) => x[1].toLowerCase()).filter((n) => n !== 'count');
}

// docs/api.md: the jsonc block that follows a heading, and lines of the Types block.
const API = read('docs/api.md');
function apiBlock(heading) {
  const i = API.indexOf(heading);
  if (i < 0) throw new Error(`docs/api.md: section "${heading}" not found`);
  const m = API.slice(i).match(/```jsonc\n([\s\S]*?)```/);
  if (!m) throw new Error(`docs/api.md: no jsonc block under "${heading}"`);
  return m[1];
}
const keysOf = (text) => [...new Set([...text.matchAll(/"(\w+)":/g)].map((x) => x[1]))];
const types = apiBlock('## Tipos');
const typeLine = (name) => {
  const m = types.match(new RegExp(`^${name}\\s+(.*)$`, 'm'));
  if (!m) throw new Error(`docs/api.md: type ${name} not found`);
  return m[1].replace(/\/\/.*$/, '');
};
const api = {
  themes: [...typeLine('Theme').matchAll(/"(\w+)"/g)].map((x) => x[1]),
  sweeps: [...typeLine('Sweep').matchAll(/"(\w+)"/g)].map((x) => x[1]),
  power: [...typeLine('Power').matchAll(/"(\w+)"/g)].map((x) => x[1]),
  ballKeys: keysOf(typeLine('Ball')),
  ballRanges: Object.fromEntries([...typeLine('Ball').matchAll(/"(\w+)": ([\d.]+)-([\d.]+)/g)].map((x) => [x[1], [+x[2], +x[3]]])),
  // One field per line of the "radar" object; the first key on the line (zoomLevels nests others).
  stateKeys: apiBlock('## `GET /api/state`').match(/"radar": \{([\s\S]*)\}/)[1].split('\n')
    .flatMap((l) => [...l.matchAll(/^\s+"(\w+)":|, "(\w+)":/g)].filter((m) => !/\[\{/.test(l.slice(0, m.index))).map((m) => m[1] || m[2])),
  radarBody: apiBlock('## `POST /api/radar`'),
  ballsBody: apiBlock('## `POST /api/balls`'),
  sfxBody: apiBlock('## `POST /api/sfx`'),
};
api.radarKeys = keysOf(api.radarBody);
api.ballsKeys = keysOf(api.ballsBody);
api.sfxKeys = keysOf(api.sfxBody);
const range = (text, key) => {
  const m = text.match(new RegExp(`"${key}": (\\d+)-(\\d+)`));
  if (!m) throw new Error(`docs/api.md: no range for ${key}`);
  return [+m[1], +m[2]];
};
api.zoomRange = range(api.radarBody, 'zoom');
api.volumeRange = range(api.radarBody, 'volume');
api.radarPower = [...api.radarBody.matchAll(/"power": ([^,}]*)/g)].flatMap((m) => [...m[1].matchAll(/"(\w+)"/g)].map((x) => x[1]));
api.maxBalls = +API.match(/0-(\d+) esferas/)[1];
api.cliThemes = API.match(/`theme <([^>]+)>`/)[1].split('|');
api.cliZoom = API.match(/`zoom <0-(\d+)>`/)[1];
api.cliSweeps = API.match(/`sweep <([^>]+)>`/)[1].split('|');

// ------------------------------------------------------------------ comparisons
function same(what, lists) {
  const entries = Object.entries(lists);
  const [refName, ref] = entries[0];
  for (const [file, names] of entries.slice(1))
    if (names.join(',') !== ref.join(',')) fail(`${what} in ${file} differ from ${refName}:\n    ${names.join(', ')}\n    ${ref.join(', ')}`);
}
function eq(what, lists) {
  const entries = Object.entries(lists);
  const [refName, ref] = entries[0];
  for (const [file, v] of entries.slice(1)) if (v !== ref) fail(`${what} in ${file} is ${v}, ${refName} says ${ref}`);
}

const radarJson = JSON.parse(read('shared/radar.json'));
const soundsJson = JSON.parse(read('shared/sounds.json'));
const levels = radarJson.zoom.levels;

same('themes', {
  'docs/api.md': api.themes,
  'shared/radar.json': Object.keys(radarJson.themes),
  [`${FW}/radar_assets.h (RadarThemeId)`]: cEnum(`${FW}/radar_assets.h`, 'RadarThemeId', 'RADAR_THEME_'),
  [`${FW}/radar_assets.h (RADAR_THEMES)`]: [...read(`${FW}/radar_assets.h`).matchAll(/^\s+\{"(\w+)", "/gm)].map((x) => x[1]),
  'docs/api.md (CLI)': api.cliThemes,
  'mcp/tools.mjs': jsArray('mcp/tools.mjs', 'THEMES'),
});

const sweepLists = {
  'docs/api.md': api.sweeps,
  'shared/radar.json (sweep.modes)': radarJson.sweep.modes,
  'docs/api.md (CLI)': api.cliSweeps,
  [`${FW}/radar_api.cpp`]: cArray(`${FW}/radar_api.cpp`, 'SWEEP_MODES'),
  'mcp/tools.mjs': jsArray('mcp/tools.mjs', 'SWEEP_MODES'),
};
// The generated header lists the modes too once gen_radar.mjs emits them (RadarSweepMode).
if (/enum RadarSweepMode\b/.test(read(`${FW}/radar_assets.h`)))
  sweepLists[`${FW}/radar_assets.h (RadarSweepMode)`] = cEnum(`${FW}/radar_assets.h`, 'RadarSweepMode', 'RADAR_SWEEP_');
same('sweep modes', sweepLists);
if (!radarJson.sweep.modes.includes(radarJson.sweep.mode)) fail(`shared/radar.json: sweep.mode "${radarJson.sweep.mode}" is not in sweep.modes`);

// Digit modes: every theme's counts must be one the renderer draws.
const countModes = jsArray('tools/preview/renderer.js', 'COUNT_MODES');
for (const [t, p] of Object.entries(radarJson.themes))
  if (!countModes.includes(p.counts)) fail(`shared/radar.json: themes.${t}.counts "${p.counts}" is not one of ${countModes.join(', ')} (renderer.js)`);

eq('zoom level count', {
  'shared/radar.json': levels.length,
  [`${FW}/radar_assets.h`]: cNumber(`${FW}/radar_assets.h`, 'RADAR_ZOOM_COUNT'),
  'docs/api.md (POST /api/radar zoom max + 1)': api.zoomRange[1] + 1,
  'docs/api.md (CLI zoom max + 1)': +api.cliZoom + 1,
  'mcp/tools.mjs (ZOOM_LABELS)': jsArray('mcp/tools.mjs', 'ZOOM_LABELS').length,
});
same('zoom labels', {
  'shared/radar.json': levels.map((l) => l.label),
  [`${FW}/radar_assets.h`]: [...read(`${FW}/radar_assets.h`).matchAll(/^\s+\{[\d.]+f, [\d.]+f, [\d.]+f, "([^"]+)"\},/gm)].map((x) => x[1]),
  'mcp/tools.mjs': jsArray('mcp/tools.mjs', 'ZOOM_LABELS'),
});

same('sound effects', {
  'shared/sounds.json': soundsJson.sfx.map((s) => s.name),
  [`${FW}/sounds.h`]: cEnum(`${FW}/sounds.h`, 'SfxId', 'SFX_'),
  'mcp/tools.mjs': jsArray('mcp/tools.mjs', 'SFX'),
});

// The landing (docs/index.html): its name lists must match, and every name needs its labels.
const LANDING = 'docs/index.html';
same('landing themes', { 'shared/radar.json': Object.keys(radarJson.themes), [LANDING]: jsArray(LANDING, 'THEMES') });
same('landing sweep modes', { 'shared/radar.json (sweep.modes)': radarJson.sweep.modes, [LANDING]: jsArray(LANDING, 'SWEEP_MODES') });
same('landing zoom labels', { 'shared/radar.json': levels.map((l) => l.label), [LANDING]: jsArray(LANDING, 'ZOOM_LABELS') });
same('landing sound effects', { 'shared/sounds.json': soundsJson.sfx.map((s) => s.name), [LANDING]: jsArray(LANDING, 'SFX') });
function jsKeys(file, name) {
  const m = read(file).match(new RegExp(`const ${name} = \\{([\\s\\S]*?)\\};`));
  if (!m) throw new Error(`${name} not found in ${file}`);
  return [...m[1].matchAll(/(\w+): \[/g)].map((x) => x[1]);
}
same('landing sweep labels (SWEEP_TEXT)', { 'shared/radar.json (sweep.modes)': radarJson.sweep.modes, [LANDING]: jsKeys(LANDING, 'SWEEP_TEXT') });
same('landing sound labels (SFX_TEXT)', { 'shared/sounds.json': soundsJson.sfx.map((s) => s.name), [LANDING]: jsKeys(LANDING, 'SFX_TEXT') });
// The demo's long press must be the firmware's.
eq('long press ms', { [`${FW}/config.h`]: cNumber(`${FW}/config.h`, 'BUTTON_LONG_PRESS_MS'), [LANDING]: jsNumber(LANDING, 'RADAR_LONG_MS') });

const cpp = `${FW}/radar_api.cpp`;
same('POST /api/radar fields', { 'docs/api.md': api.radarKeys, [cpp]: cArray(cpp, 'RADAR_KEYS'), 'web/index.html': jsArray('web/index.html', 'RADAR_KEYS'), 'mcp/tools.mjs': jsArray('mcp/tools.mjs', 'RADAR_KEYS') });
same('POST /api/radar power values', { 'docs/api.md': api.radarPower, [cpp]: cArray(cpp, 'POWER_VALUES') });
same('POST /api/balls fields', { 'docs/api.md': api.ballsKeys, [cpp]: cArray(cpp, 'BALLS_KEYS') });
same('POST /api/sfx fields', { 'docs/api.md': api.sfxKeys, [cpp]: cArray(cpp, 'SFX_KEYS') });
same('ball fields', { 'docs/api.md': api.ballKeys, [cpp]: cArray(cpp, 'BALL_KEYS'), 'web/index.html': jsArray('web/index.html', 'BALL_KEYS'), 'mcp/tools.mjs': jsArray('mcp/tools.mjs', 'BALL_KEYS') });
same('radar state fields', { 'docs/api.md': api.stateKeys, 'web/index.html': jsArray('web/index.html', 'STATE_FIELDS'), 'mcp/tools.mjs': jsArray('mcp/tools.mjs', 'STATE_FIELDS') });

eq('max balls', { 'docs/api.md': api.maxBalls, [cpp]: cNumber(cpp, 'MAX_BALLS'), 'web/index.html': jsNumber('web/index.html', 'MAX_BALLS'), 'mcp/tools.mjs': jsNumber('mcp/tools.mjs', 'MAX_BALLS'), 'shared/radar.json (demo)': radarJson.demo.balls.length });
eq('ball stars max', { 'docs/api.md': api.ballRanges.stars[1], [cpp]: cNumber(cpp, 'BALL_STARS_MAX') });
eq('ball distance min', { 'docs/api.md': api.ballRanges.distanceM[0], [cpp]: cNumber(cpp, 'BALL_DISTANCE_MIN_M'), 'web/index.html': jsNumber('web/index.html', 'DISTANCE_MIN_M'), 'mcp/tools.mjs': jsNumber('mcp/tools.mjs', 'DISTANCE_MIN_M') });
eq('ball distance max', { 'docs/api.md': api.ballRanges.distanceM[1], [cpp]: cNumber(cpp, 'BALL_DISTANCE_MAX_M'), 'web/index.html': jsNumber('web/index.html', 'DISTANCE_MAX_M'), 'mcp/tools.mjs': jsNumber('mcp/tools.mjs', 'DISTANCE_MAX_M') });
eq('volume max', { 'docs/api.md': api.volumeRange[1], [cpp]: cNumber(cpp, 'VOLUME_MAX') });

// The power states of GET /api/state must include what POST accepts.
for (const p of api.radarPower) if (!api.power.includes(p)) fail(`docs/api.md: POST power "${p}" is not a Power state`);

// radar.cpp (the radar itself) must emit every state field once it exists.
if (existsSync(join(root, `${FW}/radar.cpp`))) {
  const src = read(`${FW}/radar.cpp`);
  const missing = api.stateKeys.filter((k) => !src.includes(`"${k}"`));
  if (missing.length) fail(`${FW}/radar.cpp does not emit radar state fields: ${missing.join(', ')}`);
}

// The web panel posts only to the radar routes and embeds the current renderer and JSON.
const webPosts = [...read('web/index.html').matchAll(/post\('(\/api\/\w+)'/g)].map((x) => x[1]);
for (const p of webPosts) if (!['/api/radar', '/api/balls', '/api/sfx'].includes(p) && !API.includes(`\`POST ${p}\``)) fail(`web/index.html posts to ${p}, which docs/api.md does not define`);
for (const page of PAGES) {
  const { rel, current, expected } = buildPage(page);
  if (current !== expected) fail(`${rel} embeds a stale renderer or shared JSON: run node tools/preview/build.mjs`);
}

if (errors.length) {
  console.error(errors.map((e) => `consistency: ${e}`).join('\n'));
  process.exit(1);
}
console.log(`consistency ok: ${api.themes.length} themes, ${api.sweeps.length} sweep modes, ${levels.length} zoom levels, ${soundsJson.sfx.length} sounds, ` +
  `${api.stateKeys.length} state + ${api.radarKeys.length} radar + ${api.ballKeys.length} ball fields across docs, shared, firmware, web, MCP and landing`);
