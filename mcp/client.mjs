// HTTP client for the Dragon Radar boards. Every request is independent: no shared agent state,
// timers or abort controllers survive a call, and a failure on one host never affects another.
import { lookup } from 'node:dns/promises';
import { isIP } from 'node:net';

export const TIMEOUT_MS = Number(process.env.DRAGONRADAR_TIMEOUT_MS || 6000);
// Extra time allowed per KB of request body (the boards parse JSON bodies synchronously).
const MS_PER_KB = 40;
const ADDR_TTL_MS = 60_000;

// mDNS names are resolved IPv4-only: the boards never answer AAAA queries, so a default lookup
// waits ~5 s for the IPv6 answer on macOS, and that pending lookup also delays every other host.
const addrCache = new Map();

function splitHost(host) {
  const m = /^\[?([^\]]+?)\]?(?::(\d+))?$/.exec(host);
  if (m && isIP(m[1]) === 6) return { name: m[1], port: m[2] };
  const i = host.lastIndexOf(':');
  return i > 0 && isIP(host.slice(0, i)) !== 6 ? { name: host.slice(0, i), port: host.slice(i + 1) } : { name: host };
}

async function resolve(host) {
  const { name, port } = splitHost(host);
  const fmt = (addr) => (isIP(addr) === 6 ? `[${addr}]` : addr) + (port ? `:${port}` : '');
  if (isIP(name)) return fmt(name);
  const hit = addrCache.get(name);
  if (hit && Date.now() - hit.at < ADDR_TTL_MS) return fmt(hit.addr);
  let addr;
  try {
    ({ address: addr } = await lookup(name, { family: 4 }));
  } catch (e) {
    if (e.code === 'ENOTFOUND' || e.code === 'EAI_NONAME') throw new Error(`${host} not found (is it on the same network?)`);
    throw new Error(`${host}: name lookup failed (${e.code || e.message})`);
  }
  addrCache.set(name, { addr, at: Date.now() });
  return fmt(addr);
}

const CAUSES = {
  ECONNRESET: 'connection reset by the device',
  EPIPE: 'the device closed the connection while the request was being sent',
  ECONNREFUSED: 'connection refused',
  EHOSTUNREACH: 'host unreachable (on macOS, check the Local Network permission of the app running this server)',
  ENETUNREACH: 'network unreachable',
  ETIMEDOUT: 'connection timed out',
  UND_ERR_SOCKET: 'the device closed the connection',
  UND_ERR_CONNECT_TIMEOUT: 'connection timed out',
  UND_ERR_HEADERS_TIMEOUT: 'the device did not send a response',
};

// Turns undici's generic "fetch failed" into the underlying reason.
export function describeFetchError(e) {
  let c = e?.cause;
  while (c?.cause && !c.code) c = c.cause;
  if (!c) return e?.message || String(e);
  const code = c.code || c.name;
  const text = CAUSES[code] || c.message || 'request failed';
  return code && !text.includes(code) ? `${text} (${code})` : text;
}

// Connection-level failures where the request may not have reached the board.
const RETRYABLE = new Set(['ECONNRESET', 'EPIPE', 'ECONNREFUSED', 'UND_ERR_SOCKET', 'ETIMEDOUT', 'UND_ERR_CONNECT_TIMEOUT']);

async function once(host, path, body, method) {
  const payload = body === undefined ? undefined : JSON.stringify(body);
  const kb = payload ? Buffer.byteLength(payload) / 1024 : 0;
  const timeoutMs = Math.round(TIMEOUT_MS + kb * MS_PER_KB);
  const addr = await resolve(host);
  const signal = AbortSignal.timeout(timeoutMs);
  let res;
  try {
    res = await fetch(`http://${addr}${path}`, {
      method,
      headers: payload === undefined ? {} : { 'Content-Type': 'application/json' },
      body: payload,
      signal,
    });
  } catch (e) {
    addrCache.delete(splitHost(host).name);  // the board may have a new address after a reconnect
    if (e.name === 'TimeoutError' || e.name === 'AbortError') {
      throw Object.assign(new Error(`${host} did not answer within ${timeoutMs} ms`), { retryable: false });
    }
    const code = e.cause?.code;
    const size = kb >= 1 ? ` while sending ${Math.round(kb)} KB to ${path}` : '';
    throw Object.assign(new Error(`${host}: ${describeFetchError(e)}${size}`), { retryable: RETRYABLE.has(code) });
  }
  let json = {};
  try {
    json = await res.json();
  } catch {}
  if (!res.ok || json.ok === false) {
    const why = json.error || `HTTP ${res.status}`;
    throw new Error(res.status === 413 ? `${why} (request of ${Math.round(kb)} KB is too large for the board)` : why);
  }
  return json;
}

// GET, or POST when there is a body; `method` overrides it (PUT, DELETE). Connection drops on reads
// are retried once; writes are not, since they may already have been applied.
export async function api(host, path, body, method) {
  method = method || (body === undefined ? 'GET' : 'POST');
  try {
    return await once(host, path, body, method);
  } catch (e) {
    if (method !== 'GET' || !e.retryable) throw e;
    await new Promise((r) => setTimeout(r, 250));
    return once(host, path, body, method);
  }
}
