#!/usr/bin/env node
// Renders PNG stills of the preview into build/preview/ with headless Chrome (one foreground,
// one-shot process per still, file:// URL, no server): every zoom level, the boot keyframes, a
// zoom transition, a rotated heading and the other themes.
// Usage: node tools/preview/render.mjs   (CHROME=/path/to/chrome to override the browser)
import { execFileSync } from 'node:child_process';
import { existsSync, mkdirSync, readFileSync, rmSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const root = join(here, '..', '..');
const out = join(root, 'build/preview');
const radar = JSON.parse(readFileSync(join(root, 'shared/radar.json'), 'utf8'));
const size = radar.refSizePx;

const chrome = [process.env.CHROME, '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome',
  '/Applications/Chromium.app/Contents/MacOS/Chromium', '/usr/bin/google-chrome', '/usr/bin/chromium']
  .find((p) => p && existsSync(p));
if (!chrome) throw new Error('no Chrome/Chromium found; set CHROME=/path/to/chrome');

const profile = join(root, 'build/.chrome-profile');
mkdirSync(out, { recursive: true });

const shots = [];
radar.zoom.levels.forEach((l, i) => shots.push([`zoom${i}`, { zoom: i, b: 1 }]));
shots.push(['zoom1_dim', { zoom: 1, b: radar.blink.dimLevel }]);
shots.push(['zoom1_label', { zoom: 1, b: 1, label: 200 }]);
shots.push(['zoom_transition_1to2', { zfrom: 1, zoom: 2, zp: 0.35, b: 1 }]);
shots.push(['heading90', { zoom: 2, heading: 90, b: 1 }]);
for (const t of Object.keys(radar.themes)) if (t !== radar.theme) shots.push([`theme_${t}`, { theme: t, zoom: 2, b: 1 }]);
const B = radar.boot;
const keys = [B.flickerAtMs[0] + 10, Math.round((B.screenFromMs + B.screenToMs) / 2), B.screenToMs,
  Math.round((B.gridFromMs + B.gridToMs) / 2), B.gridToMs, B.markerFromMs + 10,
  Math.round((B.pingFromMs + B.pingToMs) / 2), Math.round((B.ballsFromMs + B.ballsToMs) / 2), B.durationMs];
for (const ms of keys) shots.push([`boot_${String(ms).padStart(4, '0')}ms`, { boot: ms, zoom: radar.zoom.default, b: 1 }]);

const page = pathToFileURL(join(here, 'index.html')).href;
for (const [name, params] of shots) {
  const file = join(out, `${name}.png`);
  const url = `${page}?shot=1&${new URLSearchParams(params)}`;
  execFileSync(chrome, ['--headless=new', '--disable-gpu', '--hide-scrollbars', '--no-first-run',
    '--force-device-scale-factor=1', `--user-data-dir=${profile}`, `--window-size=${size},${size}`,
    '--virtual-time-budget=1000', `--screenshot=${file}`, url], { stdio: 'ignore', timeout: 30000 });
  if (!existsSync(file)) throw new Error(`no screenshot for ${name}`);
  console.log(`build/preview/${name}.png`);
}
rmSync(profile, { recursive: true, force: true });
