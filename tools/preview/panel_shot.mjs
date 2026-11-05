#!/usr/bin/env node
// Drives the board's web panel (web/index.html) in headless Chrome against the in-process fake
// board of mcp/test/fake-board.mjs, saves build/web/panel.png and checks that the controls send
// the right requests: a click on the radar places the chosen ball, and the zoom, theme, sweep, demo
// and sound controls post to the API. The page opens from file:// with ?host=127.0.0.1:<port>.
// Chrome runs in the foreground of this script (driven over the DevTools protocol) and is
// killed before it returns, as is the fake board; nothing keeps running.
// Usage: node tools/preview/panel_shot.mjs   (CHROME=/path/to/chrome to override the browser)
import { spawn } from 'node:child_process';
import { existsSync, mkdirSync, rmSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { startFakeBoard } from '../../mcp/test/fake-board.mjs';

const root = join(dirname(fileURLToPath(import.meta.url)), '..', '..');
const out = join(root, 'build/web/panel.png');
const chrome = [process.env.CHROME, '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome',
  '/Applications/Chromium.app/Contents/MacOS/Chromium', '/usr/bin/google-chrome', '/usr/bin/chromium']
  .find((p) => p && existsSync(p));
if (!chrome) throw new Error('no Chrome/Chromium found; set CHROME=/path/to/chrome');
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// A manual set of balls at the 1 km level, so the shot shows dots, a merged pair and an edge arrow.
const board = await startFakeBoard({
  radar: {
    zoom: 2, heading: 20, mode: 'manual', volume: 55,
    balls: [
      { stars: 1, bearingDeg: 30, distanceM: 1500 },
      { stars: 2, bearingDeg: 120, distanceM: 2600 },
      { stars: 3, bearingDeg: 200, distanceM: 900 },
      { stars: 4, bearingDeg: 205, distanceM: 950 },
      { stars: 5, bearingDeg: 300, distanceM: 3100 },
      { stars: 6, bearingDeg: 75, distanceM: 30000 },
    ],
  },
});
const profile = join(root, 'build/.chrome-panel');
mkdirSync(dirname(out), { recursive: true });
const url = `${pathToFileURL(join(root, 'web/index.html')).href}?host=${board.host}`;
const proc = spawn(chrome, ['--headless=new', '--disable-gpu', '--hide-scrollbars', '--no-first-run',
  '--force-device-scale-factor=1', `--user-data-dir=${profile}`, '--window-size=1100,960',
  '--remote-debugging-port=0', 'about:blank'], { stdio: ['ignore', 'ignore', 'pipe'] });
const killer = setTimeout(() => proc.kill(), 60000);

try {
  // DevTools endpoint from Chrome's stderr, then the page target.
  const wsBrowser = await new Promise((resolve, reject) => {
    let err = '';
    proc.stderr.on('data', (c) => {
      err += c;
      const m = /DevTools listening on (ws:\/\/\S+)/.exec(err);
      if (m) resolve(m[1]);
    });
    proc.on('exit', (c) => reject(new Error(`Chrome exited with ${c}`)));
  });
  const port = new URL(wsBrowser).port;
  const targets = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
  const ws = new WebSocket(targets.find((t) => t.type === 'page').webSocketDebuggerUrl);
  await new Promise((r, j) => { ws.onopen = r; ws.onerror = j; });
  let id = 0;
  const pending = new Map();
  ws.onmessage = (e) => {
    const m = JSON.parse(e.data);
    if (m.id && pending.has(m.id)) { pending.get(m.id)(m); pending.delete(m.id); }
  };
  const cdp = (method, params = {}) => new Promise((resolve, reject) => {
    const n = ++id;
    pending.set(n, (m) => (m.error ? reject(new Error(`${method}: ${m.error.message}`)) : resolve(m.result)));
    ws.send(JSON.stringify({ id: n, method, params }));
  });
  const evaluate = async (expr) => (await cdp('Runtime.evaluate', { expression: expr, returnByValue: true, awaitPromise: true })).result.value;
  const click = async (x, y) => {
    for (const type of ['mousePressed', 'mouseReleased']) await cdp('Input.dispatchMouseEvent', { type, x, y, button: 'left', clickCount: 1 });
  };
  const posts = () => board.log.filter((e) => e.method === 'POST').map((e) => `${e.url} ${JSON.stringify(e.body)}`);

  await cdp('Page.enable');
  await cdp('Runtime.enable');
  await cdp('Emulation.setDeviceMetricsOverride', { width: 1100, height: 960, deviceScaleFactor: 1, mobile: false });
  await cdp('Page.navigate', { url });
  await sleep(1500);
  const polls = board.log.filter((e) => e.url === '/api/state').length;
  if (!polls) throw new Error('the panel never polled /api/state');
  const shot = await cdp('Page.captureScreenshot', { format: 'png' });
  writeFileSync(out, Buffer.from(shot.data, 'base64'));

  // Interactions: pick row 7, click 80 px right of the radar centre; then zoom out, theme, demo, a sound.
  const box = await evaluate(`(() => { const r = document.getElementById('radar').getBoundingClientRect(); return { x: r.x, y: r.y, w: r.width }; })()`);
  const cx = box.x + box.w / 2, cy = box.y + box.w / 2;
  await evaluate(`document.querySelectorAll('#slots input[type=radio]')[6].click()`);
  await click(cx + 80, cy);
  await sleep(400);
  const placed = board.state.balls.find((b) => b.stars === 7);
  const expectBearing = 90 + 20;  // right of centre = 90° relative, plus the heading
  const expectM = (80 / (box.w / 2)) / (0.222 / 1000);  // 1 km level: pitch 0.222 R per 1000 m
  if (!placed || Math.abs(placed.bearingDeg - expectBearing) > 1 || Math.abs(placed.distanceM / expectM - 1) > 0.02)
    throw new Error(`canvas click placed ${JSON.stringify(placed)}, expected 7 stars at ${expectBearing}° ~${Math.round(expectM)} m`);
  await evaluate(`document.getElementById('zOut').click()`);
  await evaluate(`(() => { const s = document.getElementById('theme'); s.value = 'classic'; s.dispatchEvent(new Event('change')); })()`);
  await evaluate(`(() => { const s = document.getElementById('sweep'); s.value = 'both'; s.dispatchEvent(new Event('change')); })()`);
  await evaluate(`document.getElementById('demo').click()`);
  await evaluate(`[...document.querySelectorAll('#sfx button')].find((b) => b.textContent === 'encontrada').click()`);
  await evaluate(`[...document.querySelectorAll('#sfx button')].find((b) => b.textContent === 'botón').click()`);
  await sleep(600);
  const want = ['/api/balls', '/api/radar {"zoom":3}', '/api/radar {"theme":"classic"}', '/api/radar {"sweep":"both"}', '/api/balls {"demo":true}', '/api/sfx {"name":"ball_found"}', '/api/sfx {"name":"button"}'];
  const got = posts();
  for (const w of want) if (!got.some((g) => g.startsWith(w))) throw new Error(`the panel did not send ${w}; sent:\n  ${got.join('\n  ')}`);
  console.log(`build/web/panel.png (${polls} polls of /api/state before the shot)`);
  console.log(`controls ok: canvas click placed 7 stars at ${placed.bearingDeg}° ${Math.round(placed.distanceM)} m; requests:\n  ${got.join('\n  ')}`);
  ws.close();
} finally {
  clearTimeout(killer);
  proc.kill();
  await new Promise((r) => (proc.exitCode !== null ? r() : proc.on('exit', r)));
  await board.close();
  rmSync(profile, { recursive: true, force: true });
}
