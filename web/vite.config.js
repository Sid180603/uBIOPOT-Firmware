/**
 * vite.config.js — Aqua-HMET P6 SPA build configuration.
 *
 * Build target: web/dist/ (served as LittleFS image on the ESP32).
 * Constraint: all assets must fit in ~1MB LittleFS — gzip is critical.
 *
 * Output structure (fixed names, no content-hash → predictable paths in LittleFS):
 *   dist/index.html
 *   dist/assets/main.js    (app + potentiostat-core + uPlot bundled)
 *   dist/assets/style.css
 *
 * The ESP32 file server (net_comms.c) transparently serves .gz files
 * by checking for <filename>.gz first, so gzip pre-compression is safe.
 */

import { defineConfig } from 'vite';
import viteCompression from 'vite-plugin-compression';

export default defineConfig({
  root: 'src',

  build: {
    outDir: '../dist',
    emptyOutDir: true,
    sourcemap: false,

    rollupOptions: {
      input: 'src/index.html',
      output: {
        // Fixed names — no content hash (LittleFS path must be stable).
        entryFileNames: 'assets/main.js',
        chunkFileNames:  'assets/[name].js',
        assetFileNames:  'assets/[name].[ext]',
        // Bundle everything into a single JS chunk (no code-splitting).
        // Target: ~80-100KB minified, ~30KB gzipped.
        manualChunks: undefined,
      },
    },
  },

  plugins: [
    // Emit pre-compressed .gz alongside every asset.
    // net_comms.c serve_file() tries <path>.gz first → automatic gzip serving.
    viteCompression({
      algorithm:        'gzip',
      ext:              '.gz',
      deleteOriginFile: false,  // keep originals for non-gzip clients
      threshold:        1024,   // only compress files > 1 KB
    }),
  ],
});
