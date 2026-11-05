#!/usr/bin/env node
// Renders PNG stills of the preview into build/preview/ with headless Chrome (one foreground,
// one-shot process per still, file:// URL, no server): every zoom level, the boot keyframes, a
// zoom transition, a rotated heading, the other themes and one still per sweep mode, plus a
// contact sheet of the sweep modes (sweep_sheet.png).
// Usage: node tools/preview/render.mjs   (CHROME=/path/to/chrome to override the browser)
import { execFileSync } from 'node:child_process';
import { existsSync, mkdirSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
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

// The sweep hand of the default mode sits a little clockwise of ball 1 (bearing 40° at heading 0),
// so the stills show the trail and the flash it leaves on the dot.
const L = radar.sweep.line;
const st = Math.round(((40 + 18) / 360) * L.periodMs);
const shots = [];
radar.zoom.levels.forEach((l, i) => shots.push([`zoom${i}`, { zoom: i, b: 1, st }]));
shots.push(['zoom1_dim', { zoom: 1, b: radar.blink.dimLevel, sweep: 'none' }]);
shots.push(['zoom1_label', { zoom: 1, b: 1, label: 200, st }]);
shots.push(['zoom_transition_1to2', { zfrom: 1, zoom: 2, zp: 0.35, b: 1, st }]);
shots.push(['heading90', { zoom: 2, heading: 90, b: 1, st }]);
for (const t of Object.keys(radar.themes)) if (t !== radar.theme) shots.push([`theme_${t}`, { theme: t, zoom: 2, b: 1, st }]);
const B = radar.boot;
const keys = [B.flickerAtMs[0] + 10, Math.round((B.screenFromMs + B.screenToMs) / 2), B.screenToMs,
  Math.round((B.gridFromMs + B.gridToMs) / 2), B.gridToMs, B.markerFromMs + 10,
  Math.round((B.pingFromMs + B.pingToMs) / 2), Math.round((B.ballsFromMs + B.ballsToMs) / 2), B.durationMs];
for (const ms of keys) shots.push([`boot_${String(ms).padStart(4, '0')}ms`, { boot: ms, zoom: radar.zoom.default, b: 1, st }]);
// One still per sweep mode at 100 m: dots at their dim level, so the hand's flash on ball 1 shows;
// the ring 40 % of the way out after a beep.
const ring = Math.round(radar.sweep.ring.durationMs * 0.4);
const sweepShots = radar.sweep.modes.map((m) => [`sweep_${m}`, { sweep: m, zoom: 1, b: radar.blink.dimLevel, st, ring }]);
shots.push(...sweepShots);

function screenshot(url, file, w, h) {
  rmSync(file, { force: true });
  execFileSync(chrome, ['--headless=new', '--disable-gpu', '--hide-scrollbars', '--no-first-run',
    '--force-device-scale-factor=1', `--user-data-dir=${profile}`, `--window-size=${w},${h}`,
    '--virtual-time-budget=1000', `--screenshot=${file}`, url], { stdio: 'ignore', timeout: 30000 });
  if (!existsSync(file)) throw new Error(`no screenshot ${file}`);
}

const page = pathToFileURL(join(here, 'index.html')).href;
for (const [name, params] of shots) {
  screenshot(`${page}?shot=1&${new URLSearchParams(params)}`, join(out, `${name}.png`), size, size);
  console.log(`build/preview/${name}.png`);
}

// Contact sheet of the sweep modes: a throwaway page next to the stills, screenshotted and removed.
const LABELS = { line: 'line (aguja)', ring: 'ring (onda)', both: 'both (las dos)', none: 'none (ninguno)' };
const cell = 300, gap = 12, cols = sweepShots.length, sheetW = cols * cell + (cols + 1) * gap, sheetH = cell + 40 + 2 * gap;
const html = join(out, '_sweep_sheet.html');
writeFileSync(html, `<!doctype html><meta charset="utf-8"><style>body{margin:0;background:#111;color:#ddd;font:15px system-ui,sans-serif}
.g{display:flex;gap:${gap}px;padding:${gap}px}figure{margin:0;width:${cell}px;text-align:center}img{width:${cell}px;height:${cell}px;display:block}
figcaption{padding-top:8px}</style><div class="g">${sweepShots.map(([n, p]) =>
  `<figure><img src="${n}.png"><figcaption>${LABELS[p.sweep] || p.sweep}</figcaption></figure>`).join('')}</div>`);
screenshot(pathToFileURL(html).href, join(out, 'sweep_sheet.png'), sheetW, sheetH);
rmSync(html, { force: true });
console.log('build/preview/sweep_sheet.png');
rmSync(profile, { recursive: true, force: true });
