#!/usr/bin/env node
// Keeps the common core identical across every app on this base. core.lock.json (same in every
// repo) lists the shared files and their sha256:
//   sketchFiles  live in firmware/<sketch>/ (the only folder there with <name>/<name>.ino)
//   repoFiles    paths from the repo root
//   blockFiles   repo files with an app block: the lines between the '>>> app' and '<<< app'
//                markers ('# >>> app' or '<!-- >>> app -->') are dropped before hashing
//
// Usage: node tools/check_core.mjs check | lock | diff <other-repo> | sync <other-repo>
//   check  recompute the hashes and compare them with core.lock.json (make check runs it)
//   lock   rewrite core.lock.json from the current files
//   diff   compare the core files with another repo's (app blocks ignored)
//   sync   copy the core files from another repo (keeping this repo's app blocks) and relock
import { readFileSync, writeFileSync, existsSync, readdirSync, mkdirSync } from 'node:fs';
import { createHash } from 'node:crypto';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const LOCK = 'core.lock.json';
const root = join(dirname(fileURLToPath(import.meta.url)), '..');

function sketchDir(repo) {
  const sketches = readdirSync(join(repo, 'firmware'), { withFileTypes: true })
    .filter((d) => d.isDirectory() && existsSync(join(repo, 'firmware', d.name, `${d.name}.ino`)))
    .map((d) => d.name);
  if (sketches.length !== 1) throw new Error(`${repo}: expected one sketch under firmware/, found ${sketches.length}`);
  return `firmware/${sketches[0]}`;
}

function readLock(repo) {
  const path = join(repo, LOCK);
  if (!existsSync(path)) throw new Error(`${path} not found`);
  const lock = JSON.parse(readFileSync(path, 'utf8'));
  if (lock.version !== 1) throw new Error(`${path}: unsupported version ${lock.version}`);
  for (const k of ['sketchFiles', 'repoFiles', 'blockFiles']) if (!Array.isArray(lock[k])) lock[k] = [];
  return lock;
}

// [logical name, path from the repo root, has an app block]
function entries(repo, lock) {
  const sketch = lock.sketchFiles.length ? sketchDir(repo) : null;
  const list = [
    ...lock.sketchFiles.map((f) => [f, `${sketch}/${f}`, false]),
    ...lock.repoFiles.map((f) => [f, f, false]),
    ...lock.blockFiles.map((f) => [f, f, true]),
  ];
  const seen = new Set();
  for (const [name] of list) {
    if (name === LOCK) throw new Error(`${LOCK} cannot list itself`);
    if (seen.has(name)) throw new Error(`${name} is listed twice in ${LOCK}`);
    seen.add(name);
  }
  return list;
}

// Splits a block file into [before + start marker, app lines, end marker + after].
function splitBlock(text, file) {
  const lines = text.split('\n');
  const start = lines.findIndex((l) => l.includes('>>> app'));
  const end = lines.findIndex((l) => l.includes('<<< app'));
  if (start < 0 || end <= start) throw new Error(`${file}: needs one '>>> app' ... '<<< app' block`);
  if (lines.slice(end + 1).some((l) => l.includes('>>> app'))) throw new Error(`${file}: more than one app block`);
  return [lines.slice(0, start + 1), lines.slice(start + 1, end), lines.slice(end)];
}

function content(repo, path, block) {
  const full = join(repo, path);
  if (!existsSync(full)) return null;
  const buf = readFileSync(full);
  if (!block) return buf;
  const [head, , tail] = splitBlock(buf.toString('utf8'), path);
  return Buffer.from([...head, ...tail].join('\n'));
}

const sha256 = (buf) => createHash('sha256').update(buf).digest('hex');

function writeLock(repo, lock) {
  const hashes = {};
  for (const [name, path, block] of entries(repo, lock)) {
    const c = content(repo, path, block);
    if (!c) throw new Error(`${path} is missing`);
    hashes[name] = sha256(c);
  }
  const out = { version: 1, sketchFiles: lock.sketchFiles, repoFiles: lock.repoFiles,
    blockFiles: lock.blockFiles, sha256: hashes };
  writeFileSync(join(repo, LOCK), JSON.stringify(out, null, 2) + '\n');
  console.log(`${LOCK}: ${Object.keys(hashes).length} core files locked`);
}

function check() {
  const lock = readLock(root);
  const bad = [];
  for (const [name, path, block] of entries(root, lock)) {
    const c = content(root, path, block);
    if (!c) bad.push(`${path}: missing`);
    else if (!lock.sha256?.[name]) bad.push(`${path}: not in the lock`);
    else if (sha256(c) !== lock.sha256[name]) bad.push(`${path}: changed`);
  }
  const extra = Object.keys(lock.sha256 || {}).filter((n) => !entries(root, lock).some(([name]) => name === n));
  for (const n of extra) bad.push(`${n}: hash without a file entry`);
  if (bad.length) {
    console.error('core files drifted from core.lock.json:');
    for (const b of bad) console.error(`  ${b}`);
    console.error('A core file must change the same way in every repo: edit it in one, run');
    console.error('`make core-sync OTHER=<that repo>` in the others, then `make core-lock` (see AGENTS.md).');
    process.exit(1);
  }
  console.log(`core ok: ${Object.keys(lock.sha256).length} files match ${LOCK}`);
}

function otherRepo(arg) {
  if (!arg) throw new Error('usage: check_core.mjs diff|sync <other-repo>');
  const other = resolve(arg);
  if (other === resolve(root)) throw new Error('the other repo is this repo');
  if (!existsSync(join(other, LOCK))) throw new Error(`${other} has no ${LOCK}`);
  return other;
}

function diff(arg) {
  const other = otherRepo(arg);
  const mine = readLock(root), theirs = readLock(other);
  const differences = [];
  for (const k of ['sketchFiles', 'repoFiles', 'blockFiles']) {
    if (JSON.stringify(mine[k]) !== JSON.stringify(theirs[k])) differences.push(`${LOCK}: ${k} lists differ`);
  }
  const theirPaths = new Map(entries(other, theirs).map(([name, path]) => [name, path]));
  for (const [name, path, block] of entries(root, mine)) {
    const a = content(root, path, block);
    const otherPath = theirPaths.get(name);
    const b = otherPath ? content(other, otherPath, block) : null;
    if (!a) differences.push(`${path}: missing here`);
    else if (!b) differences.push(`${path}: missing in ${arg}`);
    else if (!a.equals(b)) differences.push(`${path}: differs${block ? ' outside the app block' : ''}`);
  }
  if (differences.length) {
    console.error(`core differs from ${arg}:`);
    for (const d of differences) console.error(`  ${d}`);
    process.exit(1);
  }
  console.log(`core identical to ${arg}: ${entries(root, mine).length} files`);
}

function sync(arg) {
  const other = otherRepo(arg);
  // The other repo's lock decides which files are core, so a new core file propagates too.
  const theirs = readLock(other);
  const mineSketch = theirs.sketchFiles.length ? sketchDir(root) : null;
  let changed = 0;
  for (const [name, otherPath, block] of entries(other, theirs)) {
    const path = theirs.sketchFiles.includes(name) ? `${mineSketch}/${name}` : name;
    const src = readFileSync(join(other, otherPath));
    let out = src;
    if (block) {
      const [head, , tail] = splitBlock(src.toString('utf8'), otherPath);
      const mine = existsSync(join(root, path)) ? splitBlock(readFileSync(join(root, path), 'utf8'), path)[1] : [];
      out = Buffer.from([...head, ...mine, ...tail].join('\n'));
    }
    const dest = join(root, path);
    if (existsSync(dest) && readFileSync(dest).equals(out)) continue;
    mkdirSync(dirname(dest), { recursive: true });
    writeFileSync(dest, out);
    console.log(`synced ${path}`);
    changed++;
  }
  console.log(`${changed} core file(s) updated from ${arg}`);
  writeLock(root, theirs);
}

const [cmd, arg] = process.argv.slice(2);
try {
  if (cmd === 'check') check();
  else if (cmd === 'lock') writeLock(root, readLock(root));
  else if (cmd === 'diff') diff(arg);
  else if (cmd === 'sync') sync(arg);
  else throw new Error('usage: node tools/check_core.mjs check | lock | diff <other-repo> | sync <other-repo>');
} catch (e) {
  console.error(`check_core: ${e.message}`);
  process.exit(1);
}
