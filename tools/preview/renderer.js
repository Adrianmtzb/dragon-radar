// Radar renderer shared by the preview (tools/preview/index.html) and the board's web panel
// (web/index.html). tools/preview/build.mjs copies this file verbatim between the renderer markers
// of both pages; edit it here, then run the build. Plain browser script, no modules, so both pages
// keep working from file://. It draws one frame from a view and keeps no state between frames.
//
// createRadarRenderer(canvas, RADAR) -> { draw(view), scaleOf(zoom), blinkLevel(msSinceBeep),
//                                         pointToBall(px, py, view), viewRadiusM(zoom) }
// view = {
//   theme, zoom, heading,                    // palette name, level index, degrees (map is heading-up)
//   balls: [{ stars, bearingDeg, distanceM }],
//   blink,                                   // dot brightness dimLevel..1 (blinkLevel())
//   zoomFrom, zoomP,                         // transition from level zoomFrom at progress 0..1 (or null)
//   bootT, offT,                             // ms since power-on / power-off, or null
//   labelAge,                                // ms since the last zoom change (scale label), or null
//   sweep, sweepT, ringAge,                  // scan mode (radar.json sweep.modes), ms clock of the hand,
//                                            //   ms since the last beep (the ring); mode defaults to none
//   off,                                     // the screen is dark
// }
function createRadarRenderer(canvas, RADAR) {
  'use strict';
  const ctx = canvas.getContext('2d');
  const W = canvas.width, CX = W / 2, CY = W / 2, R = W / 2;
  const LEVELS = RADAR.zoom.levels;
  const clamp01 = (v) => Math.max(0, Math.min(1, v));
  const ramp = (t, a, b) => (b > a ? clamp01((t - a) / (b - a)) : t >= b ? 1 : 0);
  const easeOutCubic = (p) => 1 - Math.pow(1 - p, 3);
  const rad = (d) => (d * Math.PI) / 180;
  // Digit modes of theme.counts: stars = the star number of the dot's (nearest) ball; none = no digits.
  const COUNT_MODES = ['stars', 'none'];
  let P = RADAR.themes[RADAR.theme];

  function hexRgb(h) { const n = parseInt(h.slice(1), 16); return [(n >> 16) & 255, (n >> 8) & 255, n & 255]; }
  function rgba(h, a = 1) { const [r, g, b] = hexRgb(h); return `rgba(${r},${g},${b},${a})`; }
  function mix(h1, h2, t) {
    const a = hexRgb(h1), b = hexRgb(h2);
    return `rgb(${a.map((v, i) => Math.round(v + (b[i] - v) * t)).join(',')})`;
  }

  const scaleOf = (i) => LEVELS[i].pitchR / LEVELS[i].cellM;  // R units per metre
  const viewRadiusM = (i) => LEVELS[i].cellM / LEVELS[i].pitchR;

  // Dot brightness e ms after a beep: rise to 1, then exponential decay to dimLevel.
  function blinkLevel(e) {
    const k = RADAR.blink;
    if (e < 0) return k.dimLevel;
    if (e < k.riseMs) return k.dimLevel + (1 - k.dimLevel) * (e / k.riseMs);
    return k.dimLevel + (1 - k.dimLevel) * Math.exp(-(e - k.riseMs) / k.decayMs);
  }

  // Brightness of a dot e ms after the sweep hand crossed it: same rise and decay, no floor.
  function flashLevel(e) {
    const k = RADAR.blink;
    if (e < 0) return 0;
    return e < k.riseMs ? e / k.riseMs : Math.exp(-(e - k.riseMs) / k.decayMs);
  }

  // Screen positions (R units) at a scale; dots closer than mergeR merge, near arrows are drawn once.
  // Balls go nearest first, so a merged dot keeps the stars of its nearest ball.
  function layout(balls, heading, scale) {
    const B = RADAR.ball, dots = [], arrows = [];
    const diff = (a, b) => Math.abs(((a - b + 3 * Math.PI) % (2 * Math.PI)) - Math.PI);
    for (const b of [...balls].sort((a, c) => a.distanceM - c.distanceM)) {
      const rel = rad(b.bearingDeg - heading), r = b.distanceM * scale;
      if (r > B.visibleR) {
        if (!arrows.some((a) => diff(a, rel) < rad(RADAR.arrow.mergeDeg))) arrows.push(rel);
        continue;
      }
      const x = Math.sin(rel) * r, y = -Math.cos(rel) * r;
      const near = dots.find((c) => Math.hypot(c.x - x, c.y - y) < B.mergeR);
      if (near) { near.x = (near.x * near.n + x) / (near.n + 1); near.y = (near.y * near.n + y) / (near.n + 1); near.n++; }
      else dots.push({ x, y, n: 1, stars: b.stars });
    }
    return { dots, arrows };
  }

  // Centres (R units, around the group centre) of the n spheres of a cluster: 1 alone, 2 side by
  // side, 3-6 on a ring (point up when odd, flat top when even), 7 a ring of six around one.
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

  // Canvas pixel -> the ball that would sit there at this zoom and heading.
  function pointToBall(px, py, view) {
    const x = (px - CX) / R, y = (py - CY) / R;
    const rel = Math.atan2(x, -y) * 180 / Math.PI;
    return {
      bearingDeg: Math.round((((rel + view.heading) % 360) + 360) % 360 * 10) / 10,
      distanceM: Math.hypot(x, y) / scaleOf(view.zoom),
    };
  }

  function drawScreen() {
    const sh = RADAR.shading, g = ctx.createRadialGradient(CX, CY, 0, CX, CY, R);
    g.addColorStop(0, P.screenCenter); g.addColorStop(sh.midStop, P.screen); g.addColorStop(1, P.screenEdge);
    ctx.fillStyle = g;
    ctx.fillRect(0, 0, W, W);
  }

  function drawRim() {
    const w = RADAR.shading.rimWidthR, g = ctx.createRadialGradient(CX, CY, R * (1 - w * 2), CX, CY, R);
    g.addColorStop(0, rgba(P.rim, 0)); g.addColorStop(0.5, rgba(P.rim, 0.55)); g.addColorStop(1, rgba(P.rim, 1));
    ctx.fillStyle = g;
    ctx.beginPath(); ctx.arc(CX, CY, R, 0, 2 * Math.PI); ctx.fill();
  }

  function drawSheen() {
    const s = RADAR.shading.sheen;
    if (!s.enabled) return;
    ctx.save();
    ctx.translate(CX + s.cxR * R, CY + s.cyR * R);
    ctx.rotate(rad(s.angleDeg));
    ctx.scale(1, s.ryR / s.rxR);
    const g = ctx.createRadialGradient(0, 0, 0, 0, 0, s.rxR * R);
    g.addColorStop(0, rgba(P.sheen, s.alpha)); g.addColorStop(1, rgba(P.sheen, 0));
    ctx.fillStyle = g;
    ctx.beginPath(); ctx.arc(0, 0, s.rxR * R, 0, 2 * Math.PI); ctx.fill();
    ctx.restore();
  }

  // Square grid of the given pitch (R units), centre in the middle of a cell.
  function drawGrid(pitchR, lineWidthR, alpha) {
    if (alpha <= 0.01 || pitchR < 0.015) return;
    const pitch = pitchR * R, off = RADAR.grid.offsetCells * pitch;
    ctx.save();
    ctx.strokeStyle = rgba(P.grid, alpha);
    ctx.lineWidth = Math.max(1, lineWidthR * R);
    ctx.beginPath();
    const n = Math.ceil(R / pitch) + 1;
    for (let k = -n; k <= n; k++) {
      const v = k * pitch + off;
      if (Math.abs(v) > R) continue;
      ctx.moveTo(CX + v, 0); ctx.lineTo(CX + v, W);
      ctx.moveTo(0, CY + v); ctx.lineTo(W, CY + v);
    }
    ctx.stroke();
    ctx.restore();
  }

  function drawText(str, x, y, heightR, strokeR, color) {
    const F = RADAR.font, s = (heightR * R) / F.boxH;
    ctx.save();
    ctx.strokeStyle = color; ctx.lineWidth = Math.max(1, strokeR * R); ctx.lineCap = 'round'; ctx.lineJoin = 'round';
    ctx.beginPath();
    [...str].forEach((ch, i) => {
      for (const stroke of F.glyphs[ch] || []) {
        stroke.forEach(([gx, gy], j) => {
          const px = x + (i * F.advance + gx) * s, py = y + gy * s;
          j ? ctx.lineTo(px, py) : ctx.moveTo(px, py);
        });
      }
    });
    ctx.stroke();
    ctx.restore();
  }
  const textWidthR = (str, heightR) => ((str.length - 1) * RADAR.font.advance + RADAR.font.boxW) * (heightR / RADAR.font.boxH);

  function drawDot(x, y, stars, b, alpha) {
    const B = RADAR.ball, px = CX + x * R, py = CY + y * R;
    const g = ctx.createRadialGradient(px, py, 0, px, py, B.glowR * R);
    for (let i = 0; i <= 8; i++) {
      const t = i / 8;
      g.addColorStop(t, rgba(P.ballGlow, alpha * B.glowAlpha * b * Math.pow(1 - t, B.glowFalloff)));
    }
    ctx.fillStyle = g;
    ctx.beginPath(); ctx.arc(px, py, B.glowR * R, 0, 2 * Math.PI); ctx.fill();
    ctx.globalAlpha = alpha;
    ctx.fillStyle = mix(P.ballDim, P.ballCore, b);
    ctx.beginPath(); ctx.arc(px, py, B.coreR * R, 0, 2 * Math.PI); ctx.fill();
    ctx.globalAlpha = 1;
    if (P.counts === 'stars' && stars) {
      const h = B.digitHeightR;
      drawText(String(stars), px + B.digitDxR * R, py + (B.digitDyR - h / 2) * R, h, B.digitStrokeR, rgba(P.digits, alpha * (0.55 + 0.45 * b)));
    }
  }

  function drawArrow(rel, b, alpha) {
    const A = RADAR.arrow;
    ctx.save();
    ctx.translate(CX, CY); ctx.rotate(rel);
    const r0 = (A.radiusR - A.lengthR / 2) * R, r1 = (A.radiusR + A.lengthR / 2) * R, w = (A.widthR / 2) * R;
    ctx.fillStyle = rgba(P.arrow, alpha * (0.45 + 0.55 * b));
    ctx.beginPath(); ctx.moveTo(0, -r1); ctx.lineTo(w, -r0); ctx.lineTo(-w, -r0); ctx.closePath(); ctx.fill();
    ctx.restore();
  }

  // ---- scan effect (radar.json sweep); screen-fixed like the grid, clockwise from screen-up.
  const sweepMode = (v) => (RADAR.sweep.enabled && RADAR.sweep.modes.includes(v.sweep) ? v.sweep : 'none');
  const handDeg = (t) => ((t % RADAR.sweep.line.periodMs) + RADAR.sweep.line.periodMs) % RADAR.sweep.line.periodMs / RADAR.sweep.line.periodMs * 360;
  // ms since the hand last crossed the screen angle deg (0 = up, clockwise).
  function sinceHand(deg, t) {
    const L = RADAR.sweep.line;
    return ((((handDeg(t) - deg) % 360) + 360) % 360) / 360 * L.periodMs;
  }

  function drawHand(t) {
    const L = RADAR.sweep.line, a = rad(handDeg(t) - 90), trail = rad(L.trailDeg), n = 36;
    ctx.save();
    // Trail: a wedge of trailDeg behind the hand, trailAlpha at the hand fading linearly to 0.
    if (ctx.createConicGradient) {
      const g = ctx.createConicGradient(a - trail, CX, CY), f = trail / (2 * Math.PI);
      g.addColorStop(0, rgba(P.sweep, 0)); g.addColorStop(f, rgba(P.sweep, L.trailAlpha)); g.addColorStop(Math.min(1, f + 1e-4), rgba(P.sweep, 0));
      ctx.fillStyle = g;
      ctx.beginPath(); ctx.moveTo(CX, CY); ctx.arc(CX, CY, R, a - trail, a); ctx.closePath(); ctx.fill();
    } else {
      for (let i = 0; i < n; i++) {  // no conic gradients: n wedges of falling alpha
        ctx.fillStyle = rgba(P.sweep, L.trailAlpha * (1 - i / n));
        ctx.beginPath(); ctx.moveTo(CX, CY);
        ctx.arc(CX, CY, R, a - (trail * (i + 1)) / n, a - (trail * i) / n + 0.002);
        ctx.closePath(); ctx.fill();
      }
    }
    ctx.strokeStyle = rgba(P.sweep, L.alpha);
    ctx.lineWidth = Math.max(1, L.lineWidthR * R); ctx.lineCap = 'round';
    ctx.beginPath(); ctx.moveTo(CX, CY); ctx.lineTo(CX + Math.cos(a) * R, CY + Math.sin(a) * R); ctx.stroke();
    ctx.restore();
  }

  function drawRing(age) {
    const G = RADAR.sweep.ring;
    if (age === null || age === undefined || age < 0 || age >= G.durationMs) return;
    const p = age / G.durationMs;
    ctx.strokeStyle = rgba(P.sweep, G.alpha * (1 - p));
    ctx.lineWidth = Math.max(1, G.widthR * R);
    ctx.beginPath(); ctx.arc(CX, CY, Math.max(1, p * G.maxR * R), 0, 2 * Math.PI); ctx.stroke();
  }

  function drawMarker(alpha) {
    if (alpha <= 0) return;
    const M = RADAR.marker, h = M.heightR * R, w = (M.baseR / 2) * R;
    ctx.save();
    ctx.globalAlpha = alpha;
    ctx.beginPath();
    ctx.moveTo(CX, CY - (2 / 3) * h); ctx.lineTo(CX + w, CY + h / 3); ctx.lineTo(CX - w, CY + h / 3); ctx.closePath();
    ctx.fillStyle = P.marker; ctx.fill();
    ctx.lineWidth = Math.max(1, M.edgeR * R); ctx.lineJoin = 'round'; ctx.strokeStyle = P.markerEdge; ctx.stroke();
    ctx.restore();
  }

  function drawScaleLabel(zoom, e) {
    const L = RADAR.scaleLabel;
    if (!L.enabled || e === null || e === undefined || e < 0 || e > L.showMs + L.fadeMs) return;
    const a = e < L.showMs ? 1 : 1 - (e - L.showMs) / L.fadeMs;
    const str = LEVELS[zoom].label, wR = textWidthR(str, L.heightR);
    drawText(str, CX - (wR / 2) * R, CY + (L.yR - L.heightR / 2) * R, L.heightR, L.strokeR, rgba(P.text, a));
  }

  function blackout(a) {
    if (a <= 0) return;
    ctx.fillStyle = `rgba(0,0,0,${Math.min(1, a)})`;
    ctx.fillRect(0, 0, W, W);
  }

  function draw(v) {
    const BT = RADAR.boot, bootT = v.bootT ?? null, offT = v.offT ?? null;
    P = RADAR.themes[v.theme] || RADAR.themes[RADAR.theme];
    ctx.setTransform(1, 0, 0, 1, 0, 0);
    ctx.fillStyle = P.background; ctx.fillRect(0, 0, W, W);
    if (v.off || (bootT !== null && bootT < BT.screenFromMs)) return;
    ctx.save();
    ctx.beginPath(); ctx.arc(CX, CY, R, 0, 2 * Math.PI); ctx.clip();

    // Zoom transition: log-interpolated scale, old grid out and new grid in, both following the scale.
    let scale = scaleOf(v.zoom), grids = [[LEVELS[v.zoom], 1, 1]];
    if (v.zoomFrom !== null && v.zoomFrom !== undefined && v.zoomP < 1) {
      const p = easeOutCubic(clamp01(v.zoomP)), sF = scaleOf(v.zoomFrom), sT = scaleOf(v.zoom);
      scale = Math.exp(Math.log(sF) + (Math.log(sT) - Math.log(sF)) * p);
      grids = [[LEVELS[v.zoomFrom], scale / sF, 1 - p], [LEVELS[v.zoom], scale / sT, p]];
    }

    drawScreen();
    const gridR = bootT === null ? 1.5 : ramp(bootT, BT.gridFromMs, BT.gridToMs) * 1.02;
    if (gridR > 0) {
      ctx.save();
      if (gridR < 1.5) { ctx.beginPath(); ctx.arc(CX, CY, gridR * R, 0, 2 * Math.PI); ctx.clip(); }
      for (const [lvl, k, a] of grids) drawGrid(lvl.pitchR * k, lvl.lineWidthR, a);
      ctx.restore();
    }
    drawSheen();

    // Scan effect: above the grid and the sheen, below the dots and the marker; not while booting
    // or powering off.
    const mode = bootT === null && offT === null ? sweepMode(v) : 'none';
    const hand = mode === 'line' || mode === 'both', t = v.sweepT || 0;
    if (hand) drawHand(t);
    if (mode === 'ring' || mode === 'both') drawRing(v.ringAge);

    let ballsA = 1;
    if (bootT !== null) ballsA = ramp(bootT, BT.ballsFromMs, BT.ballsToMs);
    if (offT !== null) ballsA = 1 - ramp(offT, 0, RADAR.transitions.powerOff.ballsOutMs);
    if (ballsA > 0) {
      const b = v.blink ?? 1, { dots, arrows } = layout(v.balls || [], v.heading || 0, scale);
      // With dotFlash the hand lights each dot as it crosses it, on top of the beep blink.
      const flash = hand && RADAR.sweep.line.dotFlash
        ? (deg) => Math.max(b, flashLevel(sinceHand(deg, t))) : () => b;
      const deg = (x, y) => (Math.atan2(x, -y) * 180) / Math.PI;
      for (const rel of arrows) drawArrow(rel, flash((rel * 180) / Math.PI), ballsA);
      for (const d of dots) {
        // A group of n balls is a little cluster of n spheres; the rightmost one carries the digit.
        const offs = clusterOffsets(d.n), fl = flash(deg(d.x, d.y));
        let lead = 0;
        offs.forEach(([ox, oy], k) => {
          const [lx, ly] = offs[lead];
          if (ox > lx + 1e-4 || (Math.abs(ox - lx) <= 1e-4 && oy < ly)) lead = k;
        });
        offs.forEach(([ox, oy], k) => drawDot(d.x + ox, d.y + oy, k === lead ? d.stars : 0, fl, ballsA));
      }
    }

    let markerA = 1;
    if (bootT !== null) {
      if (bootT < BT.markerFromMs) markerA = 0;
      else if (bootT < BT.markerToMs) {
        const slot = (BT.markerToMs - BT.markerFromMs) / (2 * BT.markerBlinks);
        markerA = Math.floor((bootT - BT.markerFromMs) / slot) % 2 === 0 ? 1 : 0;
      }
    }
    drawMarker(markerA);

    if (bootT !== null && bootT >= BT.pingFromMs && bootT < BT.pingToMs) {
      const p = ramp(bootT, BT.pingFromMs, BT.pingToMs);
      ctx.strokeStyle = rgba(P.ping, 0.85 * (1 - p));
      ctx.lineWidth = BT.pingWidthR * R;
      ctx.beginPath(); ctx.arc(CX, CY, Math.max(1, p * R), 0, 2 * Math.PI); ctx.stroke();
    }
    if (bootT === null && offT === null) drawScaleLabel(v.zoom, v.labelAge);
    drawRim();

    if (bootT !== null) {
      let a = ramp(bootT, BT.screenFromMs, BT.screenToMs);
      for (const f of BT.flickerAtMs) if (bootT >= f && bootT < f + BT.flickerMs) a *= BT.flickerLevel;
      blackout(1 - a);
    }
    if (offT !== null) {
      const O = RADAR.transitions.powerOff;
      blackout(ramp(offT, O.ballsOutMs, O.durationMs));
    }
    ctx.restore();
  }

  return { draw, scaleOf, blinkLevel, pointToBall, viewRadiusM, COUNT_MODES };
}
