#!/usr/bin/env node
// Embeds into the two radar pages what they share: the renderer (tools/preview/renderer.js,
// between the "// >>> renderer" and "// <<< renderer" lines) and shared/radar.json plus
// shared/sounds.json (the <script type="application/json"> blocks), so both open from file://
// with no server and draw the same picture:
//   tools/preview/index.html  the design preview (JSON verbatim)
//   web/index.html            the board's panel (JSON without "comment" keys, to keep it small)
// Run it before tools/gen_web.mjs, which gzips web/index.html into the firmware.
// --check: fails when a page is stale. tools/check_consistency.mjs runs the same check.
import { readFileSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = join(dirname(fileURLToPath(import.meta.url)), '..', '..');
export const PAGES = [
  { rel: 'tools/preview/index.html', strip: false },
  { rel: 'web/index.html', strip: true },
];

const stripComments = (v) =>
  Array.isArray(v) ? v.map(stripComments)
    : v && typeof v === 'object' ? Object.fromEntries(Object.entries(v).filter(([k]) => k !== 'comment').map(([k, x]) => [k, stripComments(x)]))
      : v;

// The page as it should be: returns { rel, current, expected }.
export function buildPage({ rel, strip }) {
  const current = readFileSync(join(root, rel), 'utf8');
  let html = current;
  for (const [id, file] of [['radar-json', 'shared/radar.json'], ['sounds-json', 'shared/sounds.json']]) {
    let data = JSON.parse(readFileSync(join(root, file), 'utf8'));
    if (strip) data = stripComments(data);
    const json = JSON.stringify(data);
    if (json.includes('</')) throw new Error(`${file}: "</" would close the script block`);
    const re = new RegExp(`(<script id="${id}" type="application/json">\\n)[\\s\\S]*?(\\n</script>)`);
    if (!re.test(html)) throw new Error(`${rel}: block ${id} not found`);
    html = html.replace(re, (_, a, b) => a + json + b);
  }
  const renderer = readFileSync(join(root, 'tools/preview/renderer.js'), 'utf8').trimEnd();
  const re = /(\/\/ >>> renderer[^\n]*\n)[\s\S]*?(\/\/ <<< renderer)/;
  if (!re.test(html)) throw new Error(`${rel}: renderer markers not found`);
  html = html.replace(re, (_, a, b) => a + renderer + '\n' + b);
  return { rel, current, expected: html };
}

if (process.argv[1] && fileURLToPath(import.meta.url) === process.argv[1]) {
  const check = process.argv.includes('--check');
  let stale = false;
  for (const page of PAGES) {
    const { rel, current, expected } = buildPage(page);
    if (check) {
      if (current !== expected) {
        console.error(`${rel} is stale: run node tools/preview/build.mjs`);
        stale = true;
      } else console.log(`${rel} up to date`);
    } else {
      writeFileSync(join(root, rel), expected);
      console.log(`${rel}: embedded the renderer, shared/radar.json and shared/sounds.json`);
    }
  }
  if (stale) process.exit(1);
}
