// End-to-end: the real MCP server (server.mjs) and an MCP client linked in memory, calling the
// tools against the in-process fake board. Needs the SDK (`npm install` in mcp/); without it the
// test is skipped, so `make check` still runs on a fresh clone.
import { test, after } from 'node:test';
import assert from 'node:assert/strict';
import { startFakeBoard } from './fake-board.mjs';

let sdk = null;
try {
  const [{ Client }, { InMemoryTransport }, { createServer }] = await Promise.all([
    import('@modelcontextprotocol/sdk/client/index.js'),
    import('@modelcontextprotocol/sdk/inMemory.js'),
    import('../server.mjs'),
  ]);
  sdk = { Client, InMemoryTransport, createServer };
} catch {}

const skip = sdk ? false : 'MCP SDK not installed (run npm install in mcp/)';
const closers = [];
after(() => Promise.all(closers.map((c) => c())));

async function connect(hosts) {
  const server = sdk.createServer(hosts);
  const client = new sdk.Client({ name: 'test', version: '0.0.0' });
  const [a, b] = sdk.InMemoryTransport.createLinkedPair();
  await Promise.all([server.connect(a), client.connect(b)]);
  closers.push(() => client.close(), () => server.close());
  return client;
}
const text = (r) => r.content.map((c) => c.text).join('\n');

test('the server lists the four radar tools', { skip }, async () => {
  const client = await connect(['127.0.0.1:1']);
  const names = (await client.listTools()).tools.map((t) => t.name).sort();
  assert.deepEqual(names, ['get_state', 'play_sfx', 'set_balls', 'set_radar']);
});

test('tools drive the board through the MCP protocol', { skip }, async () => {
  const board = await startFakeBoard();
  closers.push(() => board.close());
  const client = await connect([board.host]);
  assert.match(text(await client.callTool({ name: 'get_state', arguments: {} })), /power on, zoom 1 \(100 m per cell\)/);
  assert.match(text(await client.callTool({ name: 'set_radar', arguments: { zoom: '1 km', sweep: 'both', volume: 30 } })), /zoom 2 \(1 km per cell\).*sweep both, volume 30/);
  const badSweep = await client.callTool({ name: 'set_radar', arguments: { sweep: 'spin' } });
  assert.equal(badSweep.isError, true);
  const r = await client.callTool({ name: 'set_balls', arguments: { balls: [{ stars: 7, bearing: 'NW', distance: '3 km' }] } });
  assert.match(text(r), /7★ 315° \(NW\) 3 km/);
  assert.match(text(await client.callTool({ name: 'play_sfx', arguments: { name: 'ball_close' } })), /played ball_close/);
  assert.match(text(await client.callTool({ name: 'play_sfx', arguments: { name: 'button' } })), /played button/);
});

test('input outside the schema and board errors come back as tool errors', { skip }, async () => {
  const board = await startFakeBoard();
  closers.push(() => board.close());
  const client = await connect([board.host]);
  const bad = await client.callTool({ name: 'set_radar', arguments: { theme: 'gt' } });
  assert.equal(bad.isError, true);
  const gone = await startFakeBoard();  // a port nobody listens on any more
  await gone.close();
  const off = await client.callTool({ name: 'get_state', arguments: { device: gone.host } });
  assert.equal(off.isError, true);
  assert.match(text(off), /error - .*connection refused/);
});
