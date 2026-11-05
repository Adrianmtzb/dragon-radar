// Tool logic of the Dragon Radar MCP server, kept free of the MCP SDK so `make check` can test it
// with node --test alone. server.mjs wraps these functions as MCP tools. Every function takes the
// board host and returns a short readable text; board errors surface as thrown Errors.
import { api } from './client.mjs';

// API names and enums of docs/api.md; tools/check_consistency.mjs compares them with the contract,
// shared/*.json, the firmware and the web panel.
export const THEMES = ['z', 'classic', 'ptp'];
export const SWEEP_MODES = ['line', 'ring', 'both', 'none'];
export const SFX = ['power_on', 'radar_beep', 'zoom_out', 'button', 'zoom_in', 'ball_close', 'ball_found', 'power_off'];
export const ZOOM_LABELS = ['10 m', '100 m', '1 km', '10 km'];
export const STATE_FIELDS = ['power', 'zoom', 'zoomLevels', 'heading', 'theme', 'sweep', 'volume', 'muted', 'mode', 'balls', 'nearestM', 'beepIntervalMs'];
export const RADAR_KEYS = ['zoom', 'theme', 'sweep', 'volume', 'muted', 'power', 'resetHeading'];
export const BALL_KEYS = ['stars', 'bearingDeg', 'distanceM'];
export const MAX_BALLS = 7;
export const DISTANCE_MIN_M = 0.1;
export const DISTANCE_MAX_M = 100000;

// 16-point compass rose, clockwise from north.
export const COMPASS = ['N', 'NNE', 'NE', 'ENE', 'E', 'ESE', 'SE', 'SSE', 'S', 'SSW', 'SW', 'WSW', 'W', 'WNW', 'NW', 'NNW'];
const COMPASS_ES = { O: 'W', NO: 'NW', SO: 'SW', ONO: 'WNW', OSO: 'WSW', NNO: 'NNW', SSO: 'SSW' };

// ------------------------------------------------------------------ helpers

const round1 = (v) => Math.round(v * 10) / 10;

export function fmtDistance(m) {
  if (m === null || m === undefined) return 'none';
  if (m >= 1000) return `${+(m / 1000).toFixed(m >= 10000 ? 1 : 2)} km`;
  return `${+m.toFixed(m < 10 ? 1 : 0)} m`;
}

// Degrees (number or "45", "45°", "45 deg") or a compass point ("NE", Spanish "NO" too) -> 0-359.9.
export function parseBearing(v) {
  let deg;
  if (typeof v === 'number') deg = v;
  else if (typeof v === 'string') {
    const t = v.trim().toUpperCase();
    const p = COMPASS.indexOf(COMPASS_ES[t] || t);
    if (p >= 0) deg = p * 22.5;
    else {
      const m = /^(-?\d+(?:\.\d+)?)\s*(°|DEG|DEGREES|º)?$/.exec(t);
      if (!m) throw new Error(`bearing "${v}": use degrees clockwise from north (0-359.9) or a compass point (${COMPASS.join(', ')})`);
      deg = +m[1];
    }
  } else throw new Error('bearing must be a number of degrees or a compass point');
  if (!Number.isFinite(deg)) throw new Error(`bearing ${v} is not a number`);
  deg = round1(((deg % 360) + 360) % 360);
  return deg >= 360 ? 0 : deg;
}

// Metres (number or "300", "300 m", "1.5 km", "2km") -> metres within the API range.
export function parseDistance(v) {
  let m;
  if (typeof v === 'number') m = v;
  else if (typeof v === 'string') {
    const x = /^(\d+(?:\.\d+)?)\s*(m|km|metres|meters|metros)?$/i.exec(v.trim());
    if (!x) throw new Error(`distance "${v}": use metres (300, "300 m") or kilometres ("1.5 km")`);
    m = +x[1] * (x[2] && /^km$/i.test(x[2]) ? 1000 : 1);
  } else throw new Error('distance must be a number of metres or a string like "1.5 km"');
  if (!(m >= DISTANCE_MIN_M && m <= DISTANCE_MAX_M)) throw new Error(`distance ${fmtDistance(m)} is outside ${DISTANCE_MIN_M} m - ${DISTANCE_MAX_M / 1000} km`);
  return m;
}

// The placement helper: a ball from its star count, bearing and distance in any accepted form.
export function placeBall(stars, bearing, distance) {
  if (!Number.isInteger(stars) || stars < 1 || stars > 7) throw new Error(`stars must be 1-7, got ${stars}`);
  return { stars, bearingDeg: parseBearing(bearing), distanceM: parseDistance(distance) };
}

const compassOf = (deg) => COMPASS[Math.round(deg / 22.5) % 16];

export function describeBall(b) {
  return `${b.stars}★ ${round1(b.bearingDeg)}° (${compassOf(b.bearingDeg)}) ${fmtDistance(b.distanceM)}`;
}

export function describeRadar(r) {
  if (!r) return 'the board sent no radar object (older firmware?)';
  const missing = STATE_FIELDS.filter((k) => !(k in r));
  const levels = r.zoomLevels || ZOOM_LABELS.map((label) => ({ label }));
  const lvl = levels[r.zoom];
  const lines = [
    `power ${r.power}, zoom ${r.zoom} (${lvl ? lvl.label : '?'} per cell), theme ${r.theme}, sweep ${r.sweep ?? '?'}, ` +
      `volume ${r.volume}${r.muted ? ' (muted)' : ''}, heading ${round1(r.heading ?? 0)}°`,
    `${r.mode === 'demo' ? 'demo' : 'manual'} balls (${(r.balls || []).length}): ${(r.balls || []).map(describeBall).join(', ') || 'none'}`,
    `nearest ${fmtDistance(r.nearestM)}, beep ${r.beepIntervalMs ? `every ${r.beepIntervalMs} ms` : 'off'}`,
  ];
  if (missing.length) lines.push(`(the board did not send: ${missing.join(', ')})`);
  return lines.join('\n');
}

// ------------------------------------------------------------------ tools

export async function getState(host) {
  const s = await api(host, '/api/state');
  const bat = s.power && s.power.batteryPct >= 0 ? `, battery ${s.power.batteryPct}%${s.power.charging ? ' charging' : ''}` : '';
  return `${s.name || 'board'} ${s.firmware || ''}${bat}\n${describeRadar(s.radar)}`;
}

// Zoom as an index (0-3) or a label ("1 km", "100m").
export function parseZoom(z) {
  if (typeof z === 'number') {
    if (!Number.isInteger(z) || z < 0 || z >= ZOOM_LABELS.length) throw new Error(`zoom must be 0-${ZOOM_LABELS.length - 1}`);
    return z;
  }
  const norm = (s) => String(s).toLowerCase().replace(/\s+/g, '');
  const i = ZOOM_LABELS.findIndex((l) => norm(l) === norm(z));
  if (i < 0) throw new Error(`zoom "${z}": use 0-${ZOOM_LABELS.length - 1} or one of ${ZOOM_LABELS.join(', ')}`);
  return i;
}

export async function setRadar(host, args = {}) {
  const body = {};
  for (const k of RADAR_KEYS) if (args[k] !== undefined) body[k] = args[k];
  if (body.zoom !== undefined) body.zoom = parseZoom(body.zoom);
  if (body.theme !== undefined && !THEMES.includes(body.theme)) throw new Error(`theme must be one of ${THEMES.join(', ')}`);
  if (body.sweep !== undefined && !SWEEP_MODES.includes(body.sweep)) throw new Error(`sweep must be one of ${SWEEP_MODES.join(', ')}`);
  if (!Object.keys(body).length) throw new Error(`nothing to change: give any of ${RADAR_KEYS.join(', ')}`);
  const r = await api(host, '/api/radar', body);
  return describeRadar(r.radar);
}

// balls: [{stars, bearing, distance}] placed with placeBall(); keep: merge with the balls on the
// radar now (a ball with the same stars is moved); demo: back to the 7 demo balls; clear: none.
export async function setBalls(host, { balls, keep, demo, clear } = {}) {
  let body;
  if ([balls !== undefined, !!demo, !!clear].filter(Boolean).length !== 1) throw new Error('give exactly one of balls, demo or clear');
  if (demo) body = { demo: true };
  else if (clear) body = { balls: [] };
  else {
    const placed = balls.map((b) => placeBall(b.stars, b.bearing ?? b.bearingDeg, b.distance ?? b.distanceM));
    const seen = new Set();
    for (const b of placed) {
      if (seen.has(b.stars)) throw new Error(`the ${b.stars}-star ball is listed twice`);
      seen.add(b.stars);
    }
    let list = placed;
    if (keep) {
      const s = await api(host, '/api/state');
      const current = (s.radar && s.radar.balls) || [];
      list = [...current.filter((b) => !seen.has(b.stars)).map((b) => Object.fromEntries(BALL_KEYS.map((k) => [k, b[k]]))), ...placed];
    }
    if (list.length > MAX_BALLS) throw new Error(`at most ${MAX_BALLS} balls, that would be ${list.length}`);
    list.sort((a, b) => a.stars - b.stars);
    body = { balls: list };
  }
  const r = await api(host, '/api/balls', body);
  return describeRadar(r.radar);
}

export async function playSfx(host, name) {
  if (!SFX.includes(name)) throw new Error(`unknown sound "${name}": use one of ${SFX.join(', ')}`);
  const r = await api(host, '/api/sfx', { name });
  return r.played === false ? `${name} not played (the radar is off or muted)` : `played ${name}`;
}
