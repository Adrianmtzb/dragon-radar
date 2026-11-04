#!/usr/bin/env node
// Renders every sound of shared/sounds.json to build/sounds/<name>.wav for listening, plus three
// radar-beep loops (far, mid, near) that show how the interval follows the distance. The synth
// mirrors the ES8311 path of sound.cpp: naive square or sine, phase-continuous, amplitude ramped
// over synth.rampMs, level = synth.levelAt100 x (volume / 100)^2, volume = gain at user volume 100.
// Usage: node tools/preview/render_wav.mjs
import { readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = join(dirname(fileURLToPath(import.meta.url)), '..', '..');
const sounds = JSON.parse(readFileSync(join(root, 'shared/sounds.json'), 'utf8'));
const { sampleRate: RATE, rampMs, levelAt100 } = sounds.synth;
const outDir = join(root, 'build/sounds');
mkdirSync(outDir, { recursive: true });

// Raw-tone subset of the melody format: ~square | ~sine, HZ:MS, R:MS.
export function parseNotes(notes) {
  const out = [];
  let wave = 'square';
  for (const tok of notes.trim().split(/[\s,|]+/)) {
    if (tok.startsWith('~')) {
      wave = tok.slice(1);
      if (wave !== 'square' && wave !== 'sine') throw new Error(`bad waveform ${tok}`);
      continue;
    }
    const m = /^(R|r|\d+):(\d+)$/.exec(tok);
    if (!m) throw new Error(`unsupported token "${tok}" (use HZ:MS, R:MS, ~square, ~sine)`);
    out.push({ hz: /r/i.test(m[1]) ? 0 : +m[1], ms: +m[2], wave });
  }
  return out;
}

export function beepIntervalMs(d) {
  const b = sounds.beep;
  const lf = Math.log10(b.farM), ln = Math.log10(b.nearM);
  const t = Math.min(1, Math.max(0, (lf - Math.log10(Math.max(d, 1e-3))) / (lf - ln)));
  return Math.round(b.maxIntervalMs + (b.minIntervalMs - b.maxIntervalMs) * Math.pow(t, b.curvePow));
}

// events: [{atMs, tones, volume}] -> Float32Array
function render(events, totalMs) {
  const n = Math.ceil((totalMs * RATE) / 1000);
  const buf = new Float32Array(n);
  // Flatten into a per-sample target schedule: later events replace earlier ones (as the player does).
  const segs = [];
  for (const ev of events) {
    let t = ev.atMs;
    for (const tone of ev.tones) {
      segs.push({ from: t, to: t + tone.ms, hz: tone.hz, wave: tone.wave, level: tone.hz ? levelAt100 * (ev.volume / 100) ** 2 : 0 });
      t += tone.ms;
    }
  }
  segs.sort((a, b) => a.from - b.from);
  let phase = 0, amp = 0, si = 0, cur = null, hz = 0, wave = 'square';
  const step = levelAt100 / ((rampMs * RATE) / 1000);  // full scale in rampMs
  for (let i = 0; i < n; i++) {
    const ms = (i * 1000) / RATE;
    while (si < segs.length && segs[si].from <= ms) cur = segs[si++];
    const active = cur && ms < cur.to ? cur : null;
    const target = active ? active.level : 0;
    if (active && active.hz) { hz = active.hz; wave = active.wave; }  // a rest keeps the last pitch while it fades
    amp = amp < target ? Math.min(target, amp + step) : Math.max(target, amp - step);
    phase = (phase + hz / RATE) % 1;
    buf[i] = (wave === 'sine' ? Math.sin(2 * Math.PI * phase) : phase < 0.5 ? 1 : -1) * amp;
  }
  return buf;
}

function writeWav(path, samples) {
  const data = Buffer.alloc(samples.length * 2);
  samples.forEach((v, i) => data.writeInt16LE(Math.max(-32767, Math.min(32767, Math.round(v * 32767))), i * 2));
  const h = Buffer.alloc(44);
  h.write('RIFF', 0); h.writeUInt32LE(36 + data.length, 4); h.write('WAVE', 8);
  h.write('fmt ', 12); h.writeUInt32LE(16, 16); h.writeUInt16LE(1, 20); h.writeUInt16LE(1, 22);
  h.writeUInt32LE(RATE, 24); h.writeUInt32LE(RATE * 2, 28); h.writeUInt16LE(2, 32); h.writeUInt16LE(16, 34);
  h.write('data', 36); h.writeUInt32LE(data.length, 40);
  writeFileSync(path, Buffer.concat([h, data]));
}

const TAIL_MS = 60;
const byName = Object.fromEntries(sounds.sfx.map((s) => [s.name, s]));
const lines = [];
for (const s of sounds.sfx) {
  const tones = parseNotes(s.notes);
  const ms = tones.reduce((a, t) => a + t.ms, 0);
  writeWav(join(outDir, `${s.name}.wav`), render([{ atMs: 0, tones, volume: s.gain }], ms + TAIL_MS));
  lines.push(`${s.name}.wav  ${ms + TAIL_MS} ms`);
}
// Beep loops: 4 s of radar beeps at the interval for a ball at each distance.
const beep = byName[sounds.beep.sfx];
for (const [label, d] of [['far', 5000], ['mid', 200], ['near', 8]]) {
  const iv = beepIntervalMs(d), evs = [];
  for (let t = 0; t < 4000; t += iv) evs.push({ atMs: t, tones: parseNotes(beep.notes), volume: beep.gain });
  writeWav(join(outDir, `radar_beep_${label}.wav`), render(evs, 4000));
  lines.push(`radar_beep_${label}.wav  4000 ms (nearest ball ${d} m, one beep every ${iv} ms)`);
}
console.log(`build/sounds/ (${RATE} Hz mono):\n  ` + lines.join('\n  '));
