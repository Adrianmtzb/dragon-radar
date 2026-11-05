#!/usr/bin/env node
// Screenshots and smoke test of the landing (docs/index.html) in headless Chrome:
// build/shots/landing-{1280,390}-{es,en}.png, full page. It also checks that the version comes
// from manifest.json, the demo draws, the gestures work like the board's (a tap zooms out, KEY2
// held 600 ms powers off and on), no script throws, and no AudioContext is created without a
// click on a sound control.
// The page is served from an in-process HTTP server on 127.0.0.1 (fetch does not work from
// file://) and Chrome runs muted in the foreground of this script, driven over the DevTools
// protocol; both are stopped before it returns, nothing keeps running.
// The site is assembled like `make site` into build/landing-site/ (page, manifest, shared/*.json,
// renderer.js; no binaries), or pass --site _site to serve a real `make site` output.
// Usage: node tools/landing_shot.mjs [--site DIR]   (CHROME=/path/to/chrome to override the browser)
import { spawn } from 'node:child_process';
import { copyFileSync, existsSync, mkdirSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { createServer } from 'node:http';
import { dirname, extname, join, normalize, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
const shots = join(root, 'build/shots');
const chrome = [process.env.CHROME, '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome',
  '/Applications/Chromium.app/Contents/MacOS/Chromium', '/usr/bin/google-chrome', '/usr/bin/chromium']
  .find((p) => p && existsSync(p));
if (!chrome) throw new Error('no Chrome/Chromium found; set CHROME=/path/to/chrome');
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const manifest = JSON.parse(readFileSync(join(root, 'docs/manifest.json'), 'utf8'));

// ---- the site
let site = process.argv.includes('--site') ? resolve(process.argv[process.argv.indexOf('--site') + 1]) : null;
if (!site) {
  site = join(root, 'build/landing-site');
  rmSync(site, { recursive: true, force: true });
  mkdirSync(join(site, 'shared'), { recursive: true });
  const SITE_URL = 'https://adrianmtzb.github.io/dragon-radar/', REPO_URL = 'https://github.com/Adrianmtzb/dragon-radar';
  writeFileSync(join(site, 'index.html'), readFileSync(join(root, 'docs/index.html'), 'utf8')
    .replaceAll('__SITE_URL__', SITE_URL).replaceAll('__REPO_URL__', REPO_URL)
    .replaceAll('__REPO_NAME__', 'dragon-radar').replaceAll('__OWNER__', 'Adrianmtzb'));
  copyFileSync(join(root, 'docs/manifest.json'), join(site, 'manifest.json'));
  for (const f of ['radar.json', 'sounds.json']) copyFileSync(join(root, 'shared', f), join(site, 'shared', f));
  copyFileSync(join(root, 'tools/preview/renderer.js'), join(site, 'renderer.js'));
}
if (!existsSync(join(site, 'index.html'))) throw new Error(`${site}/index.html not found`);

const TYPES = { '.html': 'text/html; charset=utf-8', '.js': 'text/javascript', '.json': 'application/json', '.png': 'image/png', '.bin': 'application/octet-stream' };
const server = createServer((req, res) => {
  const path = normalize(decodeURIComponent(new URL(req.url, 'http://x').pathname)).replace(/^(\.\.[/\\])+/, '');
  const file = join(site, path.endsWith('/') ? join(path, 'index.html') : path);
  if (!file.startsWith(site) || !existsSync(file)) { res.writeHead(404); return res.end('not found'); }
  res.writeHead(200, { 'Content-Type': TYPES[extname(file)] || 'application/octet-stream' });
  res.end(readFileSync(file));
});
await new Promise((r) => server.listen(0, '127.0.0.1', r));
const base = `http://127.0.0.1:${server.address().port}/`;

// ---- Chrome over the DevTools protocol
const profile = join(root, 'build/.chrome-landing');
mkdirSync(shots, { recursive: true });
const proc = spawn(chrome, ['--headless=new', '--disable-gpu', '--hide-scrollbars', '--no-first-run', '--mute-audio',
  '--autoplay-policy=user-gesture-required', `--user-data-dir=${profile}`, '--window-size=1280,900',
  '--remote-debugging-port=0', 'about:blank'], { stdio: ['ignore', 'ignore', 'pipe'] });
const killer = setTimeout(() => proc.kill(), 120000);
const problems = [];

try {
  const wsBrowser = await new Promise((resolve_, reject) => {
    let err = '';
    proc.stderr.on('data', (c) => { err += c; const m = /DevTools listening on (ws:\/\/\S+)/.exec(err); if (m) resolve_(m[1]); });
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
    else if (m.method === 'Runtime.exceptionThrown') problems.push(`exception: ${m.params.exceptionDetails.exception?.description || m.params.exceptionDetails.text}`);
    else if (m.method === 'Runtime.consoleAPICalled' && m.params.type === 'error') problems.push(`console.error: ${m.params.args.map((a) => a.value ?? a.description).join(' ')}`);
  };
  const cdp = (method, params = {}) => new Promise((resolve_, reject) => {
    const n = ++id;
    pending.set(n, (m) => (m.error ? reject(new Error(`${method}: ${m.error.message}`)) : resolve_(m.result)));
    ws.send(JSON.stringify({ id: n, method, params }));
  });
  const evaluate = async (expr) => (await cdp('Runtime.evaluate', { expression: expr, returnByValue: true, awaitPromise: true })).result.value;
  const mouse = async (type, x, y) => cdp('Input.dispatchMouseEvent', { type, x, y, button: 'left', buttons: type === 'mousePressed' ? 1 : 0, clickCount: 1 });
  const centre = (sel) => evaluate(`(() => { const r = document.querySelector('${sel}').getBoundingClientRect(); return { x: r.x + r.width / 2, y: r.y + r.height / 2 }; })()`);
  const readout = () => evaluate(`document.getElementById('readout').textContent`);

  await cdp('Page.enable');
  await cdp('Runtime.enable');
  // Counts AudioContexts: the page may only create one after a click on a sound control.
  await cdp('Page.addScriptToEvaluateOnNewDocument', { source: `window.__acCount = 0; { const A = window.AudioContext;
    window.AudioContext = class extends A { constructor(...a) { super(...a); window.__acCount++; } }; }` });

  const results = [];
  for (const width of [1280, 390]) {
    for (const lang of ['es', 'en']) {
      const mobile = width < 600;
      await cdp('Emulation.setDeviceMetricsOverride', { width, height: mobile ? 844 : 900, deviceScaleFactor: 1, mobile });
      await cdp('Page.navigate', { url: `${base}?lang=${lang}` });
      await sleep(600);
      await evaluate('document.fonts.ready.then(() => true)');
      await sleep(1800);  // the boot animation (1.5 s) has finished
      const info = await evaluate(`(() => {
        const vis = (e) => e && e.offsetParent !== null;
        const h1 = [...document.querySelectorAll('h1 > span')].find(vis);
        const c = document.getElementById('radar'), g = c.getContext('2d'), d = g.getImageData(c.width / 2 - 60, c.height / 2 - 60, 120, 120).data;
        let green = 0; for (let i = 0; i < d.length; i += 4) if (d[i + 1] > d[i] + 30 && d[i + 1] > d[i + 2] + 30) green++;
        return { lang: document.documentElement.lang, h1: h1 && h1.textContent, ver: document.getElementById('ver').textContent,
          green, chips: document.querySelectorAll('#themeChips button, #sweepChips button').length,
          sfx: document.querySelectorAll('#sfxList li').length, beep: document.querySelectorAll('#beepTable .level').length,
          failed: document.getElementById('demo').classList.contains('failed'), qr: !!document.querySelector('a.tip svg.qr'),
          height: document.documentElement.scrollHeight };
      })()`);
      if (info.lang !== lang) problems.push(`${width}/${lang}: html lang is ${info.lang}`);
      if (info.ver !== `v${manifest.version}`) problems.push(`${width}/${lang}: version shows "${info.ver}", manifest says v${manifest.version}`);
      if (info.failed || info.green < 2000) problems.push(`${width}/${lang}: the demo did not draw (green px ${info.green})`);
      if (info.chips !== 7 || info.sfx !== 8 || info.beep !== 8 || !info.qr) problems.push(`${width}/${lang}: content missing ${JSON.stringify(info)}`);
      await cdp('Emulation.setDeviceMetricsOverride', { width, height: info.height, deviceScaleFactor: 1, mobile });
      await sleep(400);
      const shot = await cdp('Page.captureScreenshot', { format: 'png', captureBeyondViewport: true });
      const file = join(shots, `landing-${width}-${lang}.png`);
      writeFileSync(file, Buffer.from(shot.data, 'base64'));
      results.push(`build/shots/landing-${width}-${lang}.png  ${width}x${info.height}  h1="${info.h1}"  ${info.ver}  demo green px ${info.green}`);
    }
  }

  // Gestures, at 1280 px in Spanish: a tap on the screen zooms out (after the 350 ms double-tap
  // wait), KEY2 click zooms back in, KEY2 held 700 ms powers off, held again powers on.
  await cdp('Emulation.setDeviceMetricsOverride', { width: 1280, height: 900, deviceScaleFactor: 1, mobile: false });
  await cdp('Page.navigate', { url: `${base}?lang=es` });
  await sleep(2200);
  const log = [`start: ${await readout()}`];
  const cv = await centre('#radar');
  await mouse('mousePressed', cv.x, cv.y); await mouse('mouseReleased', cv.x, cv.y);
  await sleep(700);
  const afterTap = await readout(); log.push(`tap: ${afterTap}`);
  const k2 = await centre('#key2Btn');
  await mouse('mousePressed', k2.x, k2.y); await mouse('mouseReleased', k2.x, k2.y);
  await sleep(500);
  const afterKey2 = await readout(); log.push(`KEY2 click: ${afterKey2}`);
  await mouse('mousePressed', k2.x, k2.y); await sleep(700); await mouse('mouseReleased', k2.x, k2.y);
  await sleep(1000);
  const afterHold = await readout(); log.push(`KEY2 hold 700 ms: ${afterHold}`);
  await mouse('mousePressed', k2.x, k2.y); await sleep(700); await mouse('mouseReleased', k2.x, k2.y);
  await sleep(2200);
  const afterHold2 = await readout(); log.push(`KEY2 hold again: ${afterHold2}`);
  if (!afterTap.startsWith('1 km')) problems.push(`tap did not zoom out to 1 km: "${afterTap}"`);
  if (!afterKey2.startsWith('100 m')) problems.push(`KEY2 click did not zoom in to 100 m: "${afterKey2}"`);
  if (!afterHold.startsWith('apagado')) problems.push(`KEY2 hold did not power off: "${afterHold}"`);
  if (!afterHold2.startsWith('100 m')) problems.push(`KEY2 hold did not power back on: "${afterHold2}"`);
  const acs = await evaluate('window.__acCount');
  log.push(`AudioContexts created without a sound click: ${acs}`);
  if (acs !== 0) problems.push(`${acs} AudioContext(s) created without a click on a sound control`);
  // One click on Sound creates it (Chrome is muted: nothing reaches the speakers).
  const sb = await centre('#soundBtn');
  await mouse('mousePressed', sb.x, sb.y); await mouse('mouseReleased', sb.x, sb.y);
  await sleep(200);
  log.push(`after clicking Sound: ${await evaluate('window.__acCount')} AudioContext, button says ${await evaluate(`document.getElementById('soundState').textContent`)}`);
  await evaluate(`localStorage.clear()`);
  ws.close();

  console.log(results.join('\n'));
  console.log('gestures:\n  ' + log.join('\n  '));
  if (problems.length) { console.error('problems:\n  ' + problems.join('\n  ')); process.exitCode = 1; }
  else console.log('landing ok: no script errors, version, demo, gestures and audio gating as expected');
} finally {
  clearTimeout(killer);
  proc.kill();
  await new Promise((r) => (proc.exitCode !== null ? r() : proc.on('exit', r)));
  await new Promise((r) => server.close(r));
  rmSync(profile, { recursive: true, force: true });
}
