// In-process fake Dragon Radar board for the MCP tests and the panel screenshot
// (tools/preview/panel_shot.mjs). It speaks the API of docs/api.md with the same validation rules
// as firmware/dragonradar/radar_api.cpp, keeps the radar state in memory and logs every request.
// It listens on 127.0.0.1 with a random port and lives only as long as the calling process.
import http from 'node:http';
import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = join(dirname(fileURLToPath(import.meta.url)), '..', '..');
const RADAR = JSON.parse(readFileSync(join(root, 'shared/radar.json'), 'utf8'));
const SOUNDS = JSON.parse(readFileSync(join(root, 'shared/sounds.json'), 'utf8'));
const THEMES = Object.keys(RADAR.themes);
const SWEEPS = RADAR.sweep.modes;
const SFX = SOUNDS.sfx.map((s) => s.name);
const LEVELS = RADAR.zoom.levels;

export function beepIntervalMs(d) {
  const b = SOUNDS.beep, lf = Math.log10(b.farM), ln = Math.log10(b.nearM);
  const t = Math.min(1, Math.max(0, (lf - Math.log10(Math.max(d, 1e-3))) / (lf - ln)));
  return Math.round(b.maxIntervalMs + (b.minIntervalMs - b.maxIntervalMs) * Math.pow(t, b.curvePow));
}

const isInt = (v) => Number.isInteger(v);
const isNum = (v) => typeof v === 'number' && Number.isFinite(v);
function unknown(obj, keys, what) {
  const k = Object.keys(obj).find((x) => !keys.includes(x));
  return k ? `unknown field "${k}" in ${what} (expected ${keys.join(', ')})` : '';
}

function checkRadar(b) {
  const u = unknown(b, ['zoom', 'theme', 'sweep', 'volume', 'muted', 'power', 'resetHeading'], '/api/radar');
  if (u) return u;
  if ('zoom' in b && (!isInt(b.zoom) || b.zoom < 0 || b.zoom >= LEVELS.length)) return `zoom must be an integer 0-${LEVELS.length - 1}`;
  if ('theme' in b && !THEMES.includes(b.theme)) return `theme must be one of ${THEMES.join(', ')}`;
  if ('sweep' in b && !SWEEPS.includes(b.sweep)) return `sweep must be one of ${SWEEPS.join(', ')}`;
  if ('volume' in b && (!isInt(b.volume) || b.volume < 0 || b.volume > 100)) return 'volume must be an integer 0-100';
  if ('muted' in b && typeof b.muted !== 'boolean') return 'muted must be true or false';
  if ('power' in b && b.power !== 'on' && b.power !== 'off') return 'power must be "on" or "off"';
  if ('resetHeading' in b && typeof b.resetHeading !== 'boolean') return 'resetHeading must be true or false';
  return '';
}

function checkBalls(b) {
  const u = unknown(b, ['balls', 'demo'], '/api/balls');
  if (u) return u;
  if ('balls' in b && 'demo' in b) return 'send either balls or demo, not both';
  if ('demo' in b) return b.demo === true ? '' : 'demo must be true';
  if (!('balls' in b)) return 'expected {"balls": [...]} or {"demo": true}';
  if (!Array.isArray(b.balls)) return 'balls must be an array';
  if (b.balls.length > 7) return `at most 7 balls, got ${b.balls.length}`;
  for (const [i, x] of b.balls.entries()) {
    const at = `balls[${i}]`;
    if (!x || typeof x !== 'object' || Array.isArray(x)) return `${at} must be an object {stars, bearingDeg, distanceM}`;
    const u2 = unknown(x, ['stars', 'bearingDeg', 'distanceM'], at);
    if (u2) return u2;
    if (!isInt(x.stars) || x.stars < 1 || x.stars > 7) return `${at}.stars must be an integer 1-7`;
    if (!isNum(x.bearingDeg) || x.bearingDeg < 0 || x.bearingDeg >= 360) return `${at}.bearingDeg must be a number 0-359.9`;
    if (!isNum(x.distanceM) || x.distanceM < 0.1 || x.distanceM > 100000) return `${at}.distanceM must be a number 0.1-100000`;
  }
  return '';
}

export function initialRadar() {
  return {
    power: 'on', zoom: RADAR.zoom.default, heading: 37.5, theme: RADAR.theme, sweep: RADAR.sweep.mode, volume: 60, muted: false,
    mode: 'demo', balls: RADAR.demo.balls.map((b) => ({ ...b })),
  };
}

// Options: radar (initial state overrides), delayMs, fail(entry) -> {status, error} to inject errors.
export async function startFakeBoard({ radar = {}, delayMs = 0, fail } = {}) {
  const st = { ...initialRadar(), ...radar };
  const log = [];
  const sfxPlayed = [];
  const radarJson = () => {
    const nearest = st.balls.length ? Math.min(...st.balls.map((b) => b.distanceM)) : null;
    return {
      power: st.power, zoom: st.zoom, zoomLevels: LEVELS.map((l) => ({ cellM: l.cellM, label: l.label })),
      heading: st.heading, theme: st.theme, sweep: st.sweep, volume: st.volume, muted: st.muted, mode: st.mode,
      balls: st.balls, nearestM: nearest,
      beepIntervalMs: nearest === null || st.power === 'off' ? null : beepIntervalMs(nearest),
    };
  };
  const route = (method, url, body) => {
    if (method === 'GET' && url === '/api/state') {
      return { status: 200, json: { ok: true, name: 'Dragon Radar', firmware: '0.1.0', board: 's3_amoled175', uptimeSec: 42, wifi: 'connected',
        power: { pmic: true, batteryMv: 3950, batteryPct: 81, charging: false, lowBattery: false }, radar: radarJson() } };
    }
    if (method !== 'POST') return { status: 404, json: { ok: false, error: 'not found' } };
    if (body === undefined) return { status: 400, json: { ok: false, error: 'expected a JSON body (Content-Type: application/json)' } };
    if (body === null || typeof body !== 'object' || Array.isArray(body)) return { status: 400, json: { ok: false, error: 'expected a JSON object' } };
    let err;
    switch (url) {
      case '/api/radar':
        if ((err = checkRadar(body))) break;
        for (const k of ['zoom', 'theme', 'sweep', 'volume', 'muted']) if (k in body) st[k] = body[k];
        if (body.power === 'on' && st.power === 'off') st.power = 'boot';
        if (body.power === 'off') st.power = 'off';
        if (body.resetHeading) st.heading = 0;
        return { status: 200, json: { ok: true, radar: radarJson() } };
      case '/api/balls':
        if ((err = checkBalls(body))) break;
        if (body.demo) { st.mode = 'demo'; st.balls = RADAR.demo.balls.map((b) => ({ ...b })); }
        else { st.mode = 'manual'; st.balls = body.balls.map((b) => ({ ...b })); }
        return { status: 200, json: { ok: true, radar: radarJson() } };
      case '/api/sfx':
        if (unknown(body, ['name'], '/api/sfx') || !SFX.includes(body.name)) { err = `name must be one of ${SFX.join(', ')}`; break; }
        sfxPlayed.push(body.name);
        return { status: 200, json: { ok: true, name: body.name, played: st.power !== 'off' && !st.muted } };
      default:
        return { status: 404, json: { ok: false, error: 'not found' } };
    }
    return { status: 400, json: { ok: false, error: err } };
  };

  const server = http.createServer((req, res) => {
    let raw = '';
    req.on('data', (c) => (raw += c));
    req.on('end', () => {
      const headers = { 'Content-Type': 'application/json', 'Access-Control-Allow-Origin': '*', Connection: 'close' };
      if (req.method === 'OPTIONS') {
        res.writeHead(204, { ...headers, 'Access-Control-Allow-Methods': 'GET, POST, OPTIONS', 'Access-Control-Allow-Headers': 'Content-Type' });
        return res.end();
      }
      let body;
      try { body = raw ? JSON.parse(raw) : undefined; } catch { body = Symbol('bad'); }
      const entry = { method: req.method, url: req.url, body };
      log.push(entry);
      let out = typeof body === 'symbol' ? { status: 400, json: { ok: false, error: 'invalid JSON' } } : null;
      const injected = fail && fail(entry);
      if (!out && injected) out = { status: injected.status, json: { ok: false, error: injected.error } };
      if (!out) out = route(req.method, req.url.split('?')[0], body);
      setTimeout(() => { res.writeHead(out.status, headers); res.end(JSON.stringify(out.json)); }, delayMs);
    });
  });
  await new Promise((r) => server.listen(0, '127.0.0.1', r));
  return {
    host: `127.0.0.1:${server.address().port}`,
    log, sfxPlayed, state: st,
    close: () => new Promise((r) => server.close(r)),
  };
}
