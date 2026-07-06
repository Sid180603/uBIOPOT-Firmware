/**
 * playwright.config.js — Aqua-HMET P6 E2E test configuration.
 *
 * Tests use Playwright's routeWebSocket() to mock the device's /ws endpoint
 * without starting a separate backend server. The SPA is served by the
 * Vite preview server (or directly from web/dist/ via a static server fixture).
 *
 * Run:
 *   npx playwright install chromium   # first time only
 *   npm test                          # from web/ directory
 *
 * CI: see .github/workflows/ci.yml job 4 (playwright-e2e).
 */

'use strict';

const { defineConfig, devices } = require('@playwright/test');
const path = require('path');

module.exports = defineConfig({
  testDir:    './tests',
  timeout:    30_000,
  retries:    1,
  workers:    1,    // serial: mock server holds shared state

  use: {
    // Base URL: tests start the mock backend and set this via fixture.
    // Default port if mock_backend starts on 3000.
    baseURL:     'http://127.0.0.1:3000',
    headless:    true,
    viewport:    { width: 1280, height: 720 },
    // Capture trace on retry
    trace:       'on-first-retry',
    screenshot:  'only-on-failure',
  },

  projects: [
    {
      name:  'chromium',
      use:   { ...devices['Desktop Chrome'] },
    },
  ],

  reporter: [['list'], ['html', { open: 'never', outputFolder: 'playwright-report' }]],

  // Global setup: ensure dist/ exists
  globalSetup: path.join(__dirname, 'tests', 'global-setup.js'),
});
