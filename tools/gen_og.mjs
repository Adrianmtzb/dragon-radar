#!/usr/bin/env node
// Renders the landing's social card (1200x630 PNG) in plain Node: pixels in a Uint8Array, PNG
// written with zlib. The radar comes from shared/radar.json (default theme colours, shading, grid,
// the demo balls at the default zoom, marker, edge arrows and the stroke font of the star digits),
// the version from docs/manifest.json. The letters of the title use a stroke font on the same
// 4 x 6 box as radar.json's digits.
// Usage: node tools/gen_og.mjs [out.png]   (default _site/og.png)
import { readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { deflateSync } from 'node:zlib';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
const out = process.argv[2] || join(root, '_site/og.png');
const manifest = JSON.parse(readFileSync(join(root, 'docs/manifest.json'), 'utf8'));
const RADAR = JSON.parse(readFileSync(join(root, 'shared/radar.json'), 'utf8'));
const P = RADAR.themes[RADAR.theme];

const W = 1200, H = 630;
const px = new Uint8Array(W * H * 3);
const hex = (h) => { const n = parseInt(h.slice(1), 16); return [(n >> 16) & 255, (n >> 8) & 255, n & 255]; };
const mix = (a, b, t) => a.map((v, i) => v + (b[i] - v) * t);
const clamp01 = (v) => Math.max(0, Math.min(1, v));

function blend(x, y, c, a) {
  if (x < 0 || y < 0 || x >= W || y >= H || a <= 0) return;
  const i = (y * W + x) * 3, k = Math.min(1, a);
  for (let j = 0; j < 3; j++) px[i + j] = Math.round(px[i + j] + (c[j] - px[i + j]) * k);
}
function fillDisc(cx, cy, r, colorAt) {
  for (let y = Math.floor(cy - r - 1); y <= Math.ceil(cy + r + 1); y++)
    for (let x = Math.floor(cx - r - 1); x <= Math.ceil(cx + r + 1); x++) {
      const d = Math.hypot(x + 0.5 - cx, y + 0.5 - cy);
      const a = clamp01(r - d + 0.5);
      if (a > 0) { const [c, ca] = colorAt(x + 0.5, y + 0.5, d); blend(x, y, c, a * ca); }
    }
}
// Antialiased thick polylines (round caps): distance from each pixel to each segment.
function strokes(lines, width, c, alpha = 1) {
  const hw = width / 2;
  for (const pts of lines)
    for (let i = 0; i + 1 < pts.length; i++) {
      const [x0, y0] = pts[i], [x1, y1] = pts[i + 1], dx = x1 - x0, dy = y1 - y0, L2 = dx * dx + dy * dy || 1;
      for (let y = Math.floor(Math.min(y0, y1) - hw - 1); y <= Math.ceil(Math.max(y0, y1) + hw + 1); y++)
        for (let x = Math.floor(Math.min(x0, x1) - hw - 1); x <= Math.ceil(Math.max(x0, x1) + hw + 1); x++) {
          const t = clamp01(((x + 0.5 - x0) * dx + (y + 0.5 - y0) * dy) / L2);
          const d = Math.hypot(x + 0.5 - (x0 + t * dx), y + 0.5 - (y0 + t * dy));
          const a = clamp01(hw - d + 0.5);
          if (a > 0) blend(x, y, c, a * alpha);
        }
    }
}
// Triangle with 4x4 supersampling.
function triangle(p, c, alpha = 1) {
  const xs = p.map((q) => q[0]), ys = p.map((q) => q[1]);
  const side = (a, b, x, y) => (b[0] - a[0]) * (y - a[1]) - (b[1] - a[1]) * (x - a[0]);
  for (let y = Math.floor(Math.min(...ys)); y <= Math.ceil(Math.max(...ys)); y++)
    for (let x = Math.floor(Math.min(...xs)); x <= Math.ceil(Math.max(...xs)); x++) {
      let n = 0;
      for (let sy = 0; sy < 4; sy++) for (let sx = 0; sx < 4; sx++) {
        const X = x + (sx + 0.5) / 4, Y = y + (sy + 0.5) / 4;
        const s1 = side(p[0], p[1], X, Y), s2 = side(p[1], p[2], X, Y), s3 = side(p[2], p[0], X, Y);
        if ((s1 >= 0 && s2 >= 0 && s3 >= 0) || (s1 <= 0 && s2 <= 0 && s3 <= 0)) n++;
      }
      if (n) blend(x, y, c, (n / 16) * alpha);
    }
}

// ---- stroke font: radar.json's digits plus the letters this card needs, on the same 4 x 6 box
const F = RADAR.font;
const LETTERS = {
  A: [[[0, 6], [0, 1.5], [2, 0], [4, 1.5], [4, 6]], [[0, 3.4], [4, 3.4]]],
  B: [[[0, 0], [0, 6], [3.2, 6], [4, 5.2], [4, 3.8], [3.2, 3], [0, 3]], [[0, 0], [3, 0], [3.8, 0.8], [3.8, 2.2], [3, 3]]],
  C: [[[4, 0], [0, 0], [0, 6], [4, 6]]],
  D: [[[0, 0], [0, 6], [2.6, 6], [4, 4.6], [4, 1.4], [2.6, 0], [0, 0]]],
  E: [[[4, 0], [0, 0], [0, 6], [4, 6]], [[0, 3], [3, 3]]],
  G: [[[4, 0], [0, 0], [0, 6], [4, 6], [4, 3.2], [2.2, 3.2]]],
  I: [[[2, 0], [2, 6]], [[0.8, 0], [3.2, 0]], [[0.8, 6], [3.2, 6]]],
  L: [[[0, 0], [0, 6], [4, 6]]],
  M: [[[0, 6], [0, 0], [2, 3], [4, 0], [4, 6]]],
  N: [[[0, 6], [0, 0], [4, 6], [4, 0]]],
  O: [[[0, 0], [4, 0], [4, 6], [0, 6], [0, 0]]],
  P: [[[0, 6], [0, 0], [4, 0], [4, 3], [0, 3]]],
  R: [[[0, 6], [0, 0], [4, 0], [4, 3], [0, 3]], [[1.6, 3], [4, 6]]],
  S: [[[4, 0.9], [3.1, 0], [0.9, 0], [0, 0.9], [0, 2.1], [0.9, 3], [3.1, 3], [4, 3.9], [4, 5.1], [3.1, 6], [0.9, 6], [0, 5.1]]],  // rounded, unlike the 5
  V: [[[0, 0], [2, 6], [4, 0]]],
  W: [[[0, 0], [0.8, 6], [2, 2.6], [3.2, 6], [4, 0]]],
  X: [[[0, 0], [4, 6]], [[4, 0], [0, 6]]],
  '-': [[[0.8, 3], [3.2, 3]]],
  '.': [[[1.8, 5.6], [2.2, 5.6]]],
  '·': [[[1.8, 3], [2.2, 3]]],
};
const glyph = (ch) => LETTERS[ch] || F.glyphs[ch] || [];
const textWidth = (str, h) => ((str.length - 1) * F.advance + F.boxW) * (h / F.boxH);
function text(str, x, y, h, width, c, alpha = 1) {
  const s = h / F.boxH, lines = [];
  [...str].forEach((ch, i) => {
    for (const st of glyph(ch)) lines.push(st.map(([gx, gy]) => [x + (i * F.advance + gx) * s, y + gy * s]));
  });
  strokes(lines, width, c, alpha);
}

// ---- background: AMOLED black, a faint grid and a green glow behind the radar
const CX = 858, CY = 330, R = 228, CASE = 26;
const bg = [5, 8, 5], gridC = hex(P.grid);
for (let y = 0; y < H; y++)
  for (let x = 0; x < W; x++) {
    const i = (y * W + x) * 3;
    const g = clamp01(1 - Math.hypot(x - CX, y - CY) / 560) ** 2;
    let c = mix(bg, hex(P.screenEdge), 0.22 * g);
    if (x % 52 === 0 || y % 52 === 0) c = mix(c, gridC, 0.35);
    px[i] = c[0]; px[i + 1] = c[1]; px[i + 2] = c[2];
  }

// ---- casing: the white disc and the knurled knob on top
for (let k = 0; k < 9; k++) {  // knob first, the disc covers its base
  const x0 = CX - 40 + k * 9;
  for (let y = CY - R - CASE - 36; y < CY - R - 6; y++)
    for (let x = x0; x < x0 + 9; x++) blend(x, y, (x - x0) < 4 ? [141, 144, 136] : [201, 203, 195], 1);
}
const caseLight = [251, 251, 247], caseMid = [218, 220, 212], caseDark = [166, 169, 159];
fillDisc(CX, CY, R + CASE, (x, y) => {
  const t = clamp01(Math.hypot(x - (CX - 0.3 * R), y - (CY - 0.44 * R)) / (1.6 * (R + CASE)));
  return [t < 0.58 ? mix(caseLight, caseMid, t / 0.58) : mix(caseMid, caseDark, (t - 0.58) / 0.42), 1];
});
fillDisc(CX, CY, R + 3, () => [[184, 186, 178], 1]);

// ---- screen: radial gradient, grid, sheen (radar.json shading), then balls, marker and rim
const sh = RADAR.shading, cC = hex(P.screenCenter), cS = hex(P.screen), cE = hex(P.screenEdge);
const L = RADAR.zoom.levels[RADAR.zoom.default], pitch = L.pitchR * R, lw = Math.max(1, L.lineWidthR * R);
const off = RADAR.grid.offsetCells * pitch;
const S = sh.sheen, sa = (S.angleDeg * Math.PI) / 180;
fillDisc(CX, CY, R, (x, y, d) => {
  const t = d / R;
  let c = t < sh.midStop ? mix(cC, cS, t / sh.midStop) : mix(cS, cE, (t - sh.midStop) / (1 - sh.midStop));
  const gx = Math.abs((((x - CX - off) % pitch) + pitch) % pitch - pitch / 2), gy = Math.abs((((y - CY - off) % pitch) + pitch) % pitch - pitch / 2);
  const line = Math.max(clamp01(lw / 2 - (pitch / 2 - gx) + 0.5), clamp01(lw / 2 - (pitch / 2 - gy) + 0.5));
  c = mix(c, gridC, line);
  if (S.enabled) {  // the faint glass reflection, upper left
    const ex = x - (CX + S.cxR * R), ey = y - (CY + S.cyR * R);
    const u = (ex * Math.cos(sa) + ey * Math.sin(sa)) / (S.rxR * R), v = (-ex * Math.sin(sa) + ey * Math.cos(sa)) / (S.ryR * R);
    c = mix(c, hex(P.sheen), S.alpha * clamp01(1 - Math.hypot(u, v)));
  }
  return [c, 1];
});

// Same cluster layout as tools/preview/renderer.js: 2 side by side, 3-6 on a ring, 7 a ring of six
// around one, clusterSpacingR between neighbouring centres.
function clusterOffsets(n) {
  const s = RADAR.ball.clusterSpacingR;
  if (n <= 1) return [[0, 0]];
  const out = n === 7 ? [[0, 0]] : [], ring = n === 7 ? 6 : n;
  const rr = n === 7 ? s : s / (2 * Math.sin(Math.PI / n));
  const start = -Math.PI / 2 + (ring % 2 ? 0 : Math.PI / ring);
  for (let k = 0; k < ring; k++) {
    const a = start + (2 * Math.PI * k) / ring;
    out.push([Math.cos(a) * rr, Math.sin(a) * rr]);
  }
  return out;
}

// Demo balls at the default zoom, laid out like tools/preview/renderer.js (nearest first, merge,
// edge arrows), heading 0.
const B = RADAR.ball, A = RADAR.arrow, scale = L.pitchR / L.cellM;
const dots = [], arrows = [];
const adiff = (a, b) => Math.abs(((a - b + 3 * Math.PI) % (2 * Math.PI)) - Math.PI);
for (const b of [...RADAR.demo.balls].sort((p, q) => p.distanceM - q.distanceM)) {
  const rel = (b.bearingDeg * Math.PI) / 180, r = b.distanceM * scale;
  if (r > B.visibleR) { if (!arrows.some((a) => adiff(a, rel) < (A.mergeDeg * Math.PI) / 180)) arrows.push(rel); continue; }
  const x = Math.sin(rel) * r, y = -Math.cos(rel) * r;
  const near = dots.find((c) => Math.hypot(c.x - x, c.y - y) < B.mergeR);
  if (near) { near.x = (near.x * near.n + x) / (near.n + 1); near.y = (near.y * near.n + y) / (near.n + 1); near.n++; }
  else dots.push({ x, y, n: 1, stars: b.stars });
}
for (const rel of arrows) {
  const rot = ([u, v]) => [CX + u * Math.cos(rel) - v * Math.sin(rel), CY + u * Math.sin(rel) + v * Math.cos(rel)];
  const r0 = (A.radiusR - A.lengthR / 2) * R, r1 = (A.radiusR + A.lengthR / 2) * R, w = (A.widthR / 2) * R;
  triangle([rot([0, -r1]), rot([w, -r0]), rot([-w, -r0])], hex(P.arrow));
}
for (const d of dots) {
  const x = CX + d.x * R, y = CY + d.y * R;
  // A group of n balls is a little cluster of n spheres; the rightmost one carries the digit.
  const offs = clusterOffsets(d.n);
  let lead = 0;
  offs.forEach(([ox, oy], k) => {
    const [lx, ly] = offs[lead];
    if (ox > lx + 1e-4 || (Math.abs(ox - lx) <= 1e-4 && oy < ly)) lead = k;
  });
  offs.forEach(([ox, oy], k) => {
    const sx = x + ox * R, sy = y + oy * R;
    fillDisc(sx, sy, B.glowR * R, (_x, _y, dd) => [hex(P.ballGlow), B.glowAlpha * Math.pow(clamp01(1 - dd / (B.glowR * R)), B.glowFalloff)]);
    fillDisc(sx, sy, B.coreR * R, () => [hex(P.ballCore), 1]);
    if (k !== lead) return;
    const h = B.digitHeightR * R;
    text(String(d.stars), sx + B.digitDxR * R, sy + B.digitDyR * R - h / 2, h, Math.max(1, B.digitStrokeR * R), hex(P.digits));
  });
}
const M = RADAR.marker, mh = M.heightR * R, mw = (M.baseR / 2) * R;
const tri = [[CX, CY - (2 / 3) * mh], [CX + mw, CY + mh / 3], [CX - mw, CY + mh / 3]];
triangle(tri, hex(P.marker));
strokes([[...tri, tri[0]]], Math.max(1, M.edgeR * R), hex(P.markerEdge));
// Rim: the glass darkens towards the casing.
const rimW = sh.rimWidthR * 2 * R;
fillDisc(CX, CY, R, (_x, _y, d) => [hex(P.rim), d < R - rimW ? 0 : Math.pow((d - (R - rimW)) / rimW, 1.5)]);

// ---- title and captions
const title = [217, 248, 192].map((v, i) => v + (hex(P.ballCore)[i] - v) * 0.5);
const glow = hex(P.ballGlow), accent = hex(P.ping), muted = [140, 160, 136];
const X0 = 76;
text('DRAGON', X0, 118, 92, 13, title);
text('RADAR', X0, 236, 92, 13, glow);
text('ESP32-S3 · AMOLED 466X466', X0, 386, 20, 3.2, accent);
text('WEB · API · MCP', X0, 428, 20, 3.2, muted);
const ver = `V${manifest.version}`;
text(ver, X0, 508, 30, 4.4, hex(P.marker));
text('ADRIANMB.DEV', X0 + textWidth(ver, 30) + 40, 514, 22, 3.2, muted);

// ---- PNG
const CRC = new Int32Array(256).map((_, n) => { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; return c; });
function crc32(b) { let c = -1; for (const x of b) c = CRC[(c ^ x) & 255] ^ (c >>> 8); return (c ^ -1) >>> 0; }
function chunk(type, data) {
  const len = Buffer.alloc(4); len.writeUInt32BE(data.length);
  const td = Buffer.concat([Buffer.from(type), data]);
  const crc = Buffer.alloc(4); crc.writeUInt32BE(crc32(td));
  return Buffer.concat([len, td, crc]);
}
const ihdr = Buffer.alloc(13);
ihdr.writeUInt32BE(W, 0); ihdr.writeUInt32BE(H, 4); ihdr[8] = 8; ihdr[9] = 2;  // 8-bit RGB
const raw = Buffer.alloc((W * 3 + 1) * H);
for (let y = 0; y < H; y++) Buffer.from(px.buffer, y * W * 3, W * 3).copy(raw, y * (W * 3 + 1) + 1);
const png = Buffer.concat([
  Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
  chunk('IHDR', ihdr), chunk('IDAT', deflateSync(raw, { level: 9 })), chunk('IEND', Buffer.alloc(0)),
]);
mkdirSync(dirname(out), { recursive: true });
writeFileSync(out, png);
console.log(`og.png: ${W}x${H}, ${png.length} bytes, v${manifest.version}, ${dots.length} dots + ${arrows.length} arrows -> ${out}`);
