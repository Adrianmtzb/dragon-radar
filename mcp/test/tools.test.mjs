import { test, after } from 'node:test';
import assert from 'node:assert/strict';
import { startFakeBoard } from './fake-board.mjs';
import { describeRadar, getState, parseBearing, parseDistance, parseZoom, placeBall, playSfx, setBalls, setRadar } from '../tools.mjs';

const boards = [];
after(() => Promise.all(boards.map((b) => b.close())));
async function board(opts) {
  const b = await startFakeBoard(opts);
  boards.push(b);
  return b;
}
const posts = (b) => b.log.filter((e) => e.method === 'POST');

test('placement helper: bearings in degrees or compass points, distances with units', () => {
  assert.equal(parseBearing(45), 45);
  assert.equal(parseBearing('NE'), 45);
  assert.equal(parseBearing('wsw'), 247.5);
  assert.equal(parseBearing('NO'), 315);  // Spanish noroeste
  assert.equal(parseBearing('90°'), 90);
  assert.equal(parseBearing(-90), 270);
  assert.equal(parseBearing(720.04), 0);
  assert.throws(() => parseBearing('up'), /compass point/);
  assert.equal(parseDistance(300), 300);
  assert.equal(parseDistance('1.5 km'), 1500);
  assert.equal(parseDistance('25m'), 25);
  assert.throws(() => parseDistance(0), /outside/);
  assert.throws(() => parseDistance('200 km'), /outside/);
  assert.throws(() => parseDistance('far'), /kilometres/);
  assert.deepEqual(placeBall(4, 'S', '2 km'), { stars: 4, bearingDeg: 180, distanceM: 2000 });
  assert.throws(() => placeBall(8, 0, 1), /stars must be 1-7/);
});

test('zoom accepts an index or a cell size', () => {
  assert.equal(parseZoom(2), 2);
  assert.equal(parseZoom('1 km'), 2);
  assert.equal(parseZoom('100m'), 1);
  assert.throws(() => parseZoom(4), /0-3/);
  assert.throws(() => parseZoom('5 km'), /10 km/);
});

test('get_state describes the radar', async () => {
  const b = await board();
  const text = await getState(b.host);
  assert.match(text, /battery 81%/);
  assert.match(text, /power on, zoom 1 \(100 m per cell\), theme z, sweep line, volume 60, heading 37\.5°/);
  assert.match(text, /demo balls \(7\): 1★ 40° \(NE\) 25 m/);
  assert.match(text, /nearest 25 m, beep every \d+ ms/);
});

test('describeRadar flags fields an older firmware does not send', () => {
  assert.match(describeRadar({ power: 'on', zoom: 0, balls: [] }), /did not send: zoomLevels, heading, theme, sweep/);
  assert.match(describeRadar(undefined), /no radar object/);
});

test('set_radar sends only the given fields and translates zoom labels', async () => {
  const b = await board();
  const text = await setRadar(b.host, { zoom: '10 km', theme: 'classic', sweep: 'ring', muted: true, device: 'ignored' });
  assert.deepEqual(posts(b).at(-1), { method: 'POST', url: '/api/radar', body: { zoom: 3, theme: 'classic', sweep: 'ring', muted: true } });
  assert.match(text, /zoom 3 \(10 km per cell\), theme classic, sweep ring, volume 60 \(muted\)/);
  assert.match(await setRadar(b.host, { sweep: 'none' }), /sweep none/);
  await assert.rejects(setRadar(b.host, { sweep: 'spin' }), /sweep must be one of line, ring, both, none/);
  await setRadar(b.host, { power: 'off' });
  assert.match(await setRadar(b.host, { power: 'on' }), /power boot/);
  await assert.rejects(setRadar(b.host, {}), /nothing to change/);
  await assert.rejects(setRadar(b.host, { theme: 'gt' }), /theme must be one of z, classic, ptp/);
});

test('board validation errors reach the caller', async () => {
  const b = await board();
  await assert.rejects(setRadar(b.host, { volume: 150 }), /volume must be an integer 0-100/);
  await assert.rejects(setRadar(b.host, { zoom: 1.5 }), /zoom must be 0-3/);  // caught before the request
  const errBoard = await board({ fail: (e) => (e.url === '/api/sfx' ? { status: 400, error: 'busy' } : null) });
  await assert.rejects(playSfx(errBoard.host, 'zoom_in'), /busy/);
});

test('set_balls places balls by bearing and distance', async () => {
  const b = await board();
  const text = await setBalls(b.host, { balls: [{ stars: 3, bearing: 'E', distance: '1.2 km' }, { stars: 1, bearing: 10, distance: 50 }] });
  assert.deepEqual(posts(b).at(-1).body, { balls: [{ stars: 1, bearingDeg: 10, distanceM: 50 }, { stars: 3, bearingDeg: 90, distanceM: 1200 }] });
  assert.match(text, /manual balls \(2\): 1★ 10° \(N\) 50 m, 3★ 90° \(E\) 1\.2 km/);
  assert.equal(b.state.mode, 'manual');
});

test('set_balls keep moves one ball and leaves the others', async () => {
  const b = await board();
  await setBalls(b.host, { balls: [{ stars: 1, bearing: 0, distance: 5 }], keep: true });
  const sent = posts(b).at(-1).body.balls;
  assert.equal(sent.length, 7);
  assert.deepEqual(sent[0], { stars: 1, bearingDeg: 0, distanceM: 5 });
  assert.deepEqual(sent[6], { stars: 7, bearingDeg: 75, distanceM: 28000 });
  await assert.rejects(setBalls(b.host, { balls: [{ stars: 2, bearing: 0, distance: 5 }, { stars: 2, bearing: 9, distance: 5 }] }), /listed twice/);
});

test('set_balls demo and clear', async () => {
  const b = await board({ radar: { mode: 'manual', balls: [] } });
  assert.match(await setBalls(b.host, { demo: true }), /demo balls \(7\)/);
  assert.match(await setBalls(b.host, { clear: true }), /manual balls \(0\): none\nnearest none, beep off/);
  assert.deepEqual(posts(b).map((e) => e.body), [{ demo: true }, { balls: [] }]);
  await assert.rejects(setBalls(b.host, { demo: true, clear: true }), /exactly one/);
  await assert.rejects(setBalls(b.host, {}), /exactly one/);
});

test('play_sfx plays known sounds and reports when the radar is silent', async () => {
  const b = await board();
  assert.equal(await playSfx(b.host, 'ball_found'), 'played ball_found');
  assert.equal(await playSfx(b.host, 'button'), 'played button');
  await setRadar(b.host, { muted: true });
  assert.match(await playSfx(b.host, 'zoom_out'), /not played/);
  await assert.rejects(playSfx(b.host, 'kamehameha'), /unknown sound/);
  assert.deepEqual(b.sfxPlayed, ['ball_found', 'button', 'zoom_out']);
});

test('the fake board enforces the same contract as the firmware', async () => {
  const b = await board();
  const post = async (url, body) => {
    const r = await fetch(`http://${b.host}${url}`, { method: 'POST', body: JSON.stringify(body) });
    return [r.status, (await r.json()).error];
  };
  assert.deepEqual(await post('/api/balls', { balls: Array.from({ length: 8 }, (_, i) => ({ stars: (i % 7) + 1, bearingDeg: 0, distanceM: 1 })) }), [400, 'at most 7 balls, got 8']);
  assert.deepEqual(await post('/api/balls', { balls: [{ stars: 1, bearingDeg: 360, distanceM: 1 }] }), [400, 'balls[0].bearingDeg must be a number 0-359.9']);
  assert.equal((await post('/api/radar', { zom: 1 }))[0], 400);
  assert.deepEqual(await post('/api/radar', { sweep: 'spin' }), [400, 'sweep must be one of line, ring, both, none']);
  assert.deepEqual(await post('/api/sfx', { name: 'boom' }), [400, 'name must be one of power_on, radar_beep, zoom_out, button, zoom_in, ball_close, ball_found, power_off']);
});
