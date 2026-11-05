import { test, after } from 'node:test';
import assert from 'node:assert/strict';
import http from 'node:http';
import { api, describeFetchError } from '../client.mjs';

const servers = [];
after(() => servers.forEach((s) => s.close()));

function listen(handler) {
  const s = http.createServer(handler);
  servers.push(s);
  return new Promise((r) => s.listen(0, '127.0.0.1', () => r(`127.0.0.1:${s.address().port}`)));
}

const ok = (res, body) => {
  res.writeHead(200, { 'Content-Type': 'application/json', Connection: 'close' });
  res.end(JSON.stringify({ ok: true, ...body }));
};

// Board that drops the connection as soon as a large body starts arriving.
const dropper = await listen((req, res) => {
  if (req.method === 'POST') {
    let n = 0;
    req.on('data', (c) => {
      n += c.length;
      if (n > 4096) req.socket.destroy();
    });
    return;
  }
  ok(res, { radar: { power: 'off' } });
});
const healthy = await listen((req, res) => ok(res, { radar: { power: 'on' } }));

test('a dropped 40 KB upload reports the real cause', async () => {
  const frames = ['ab'.repeat(20 * 1024)];
  await assert.rejects(api(dropper, '/api/balls', { frames }), (e) => {
    assert.doesNotMatch(e.message, /^fetch failed$/);
    assert.match(e.message, /KB to \/api\/balls/);
    return true;
  });
});

test('other hosts and later requests keep working after a failure', async () => {
  const frames = ['ab'.repeat(20 * 1024)];
  await assert.rejects(api(dropper, '/api/balls', { frames }));
  assert.equal((await api(healthy, '/api/state')).radar.power, 'on');
  assert.equal((await api(dropper, '/api/state')).radar.power, 'off');
});

test('HTTP errors from the board are surfaced', async () => {
  const tooBig = await listen((req, res) => {
    req.resume();
    req.on('end', () => {
      res.writeHead(413, { 'Content-Type': 'application/json', Connection: 'close' });
      res.end(JSON.stringify({ ok: false, error: 'invalid JSON: NoMemory' }));
    });
  });
  await assert.rejects(api(tooBig, '/api/balls', { x: 'y'.repeat(2048) }), /NoMemory.*too large/);
});

test('describeFetchError unwraps the cause', () => {
  const e = new TypeError('fetch failed', { cause: Object.assign(new Error('read ECONNRESET'), { code: 'ECONNRESET' }) });
  assert.equal(describeFetchError(e), 'connection reset by the device (ECONNRESET)');
});
