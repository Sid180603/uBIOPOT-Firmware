/**
 * inline.mjs — post-build step for the Aqua-HMET SPA.
 *
 * `vite build` emits multi-file output (dist/index.html + dist/assets/*).
 * The repo commits a STANDALONE dist/index.html (assets/ and *.gz are gitignored)
 * so the SPA works without an npm build. This script inlines the built JS/CSS
 * back into a single self-contained index.html, emits a gzip copy for the ESP32
 * file server (net_comms serves <file>.gz first), and removes the now-inlined
 * external assets. Runs automatically via `npm run build`.
 */
import { readFileSync, writeFileSync, rmSync } from 'node:fs';
import { gzipSync } from 'node:zlib';

const dist = new URL('./dist/', import.meta.url);
const idx  = new URL('index.html', dist);

let html = readFileSync(idx, 'utf8');
const js  = readFileSync(new URL('assets/main.js', dist), 'utf8');
const css = readFileSync(new URL('assets/index.css', dist), 'utf8');

html = html.replace(
  /<script type="module" crossorigin src="\/assets\/main\.js"><\/script>/,
  `<script type="module">\n${js}\n</script>`
);
html = html.replace(
  /<link rel="stylesheet" crossorigin href="\/assets\/index\.css">/,
  `<style>\n${css}\n</style>`
);

if (html.includes('/assets/')) {
  throw new Error('inline.mjs: external /assets/ references remain — check vite output tag format');
}

writeFileSync(idx, html);
writeFileSync(new URL('index.html.gz', dist), gzipSync(html, { level: 9 }));
rmSync(new URL('assets', dist), { recursive: true, force: true });

console.log(`inline.mjs: standalone dist/index.html (${html.length} bytes) + .gz`);
