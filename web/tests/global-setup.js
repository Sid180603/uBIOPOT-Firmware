/**
 * global-setup.js — Playwright global setup.
 * Verifies that web/dist/index.html exists before running tests.
 */

'use strict';

const fs   = require('fs');
const path = require('path');

module.exports = async function globalSetup() {
  const dist = path.join(__dirname, '..', 'dist', 'index.html');
  if (!fs.existsSync(dist)) {
    console.warn(`
  WARNING: web/dist/index.html not found.
  The Playwright tests will serve the pre-built dist/index.html.
  That file should exist (committed to the repo).
  For the optimised Vite build: cd web && npm run build
`);
  }
};
