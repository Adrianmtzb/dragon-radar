#!/usr/bin/env node
// Embeds shared/radar.json and shared/sounds.json into tools/preview/index.html (the
// <script type="application/json"> blocks), so the preview opens from file:// with no server.
// --check: fails when the embedded copies do not match the shared files.
import { readFileSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const root = join(here, '..', '..');
const page = join(here, 'index.html');
const rel = 'tools/preview/index.html';

let html = readFileSync(page, 'utf8');
const before = html;
for (const [id, file] of [['radar-json', 'shared/radar.json'], ['sounds-json', 'shared/sounds.json']]) {
  const json = JSON.stringify(JSON.parse(readFileSync(join(root, file), 'utf8')));
  if (json.includes('</')) throw new Error(`${file}: "</" would close the script block`);
  const re = new RegExp(`(<script id="${id}" type="application/json">\\n)[\\s\\S]*?(\\n</script>)`);
  if (!re.test(html)) throw new Error(`${rel}: block ${id} not found`);
  html = html.replace(re, (_, a, b) => a + json + b);
}

if (process.argv.includes('--check')) {
  if (html !== before) {
    console.error(`${rel} is stale: run node tools/preview/build.mjs`);
    process.exit(1);
  }
  console.log(`${rel} up to date`);
} else {
  writeFileSync(page, html);
  console.log(`${rel}: embedded shared/radar.json and shared/sounds.json`);
}
