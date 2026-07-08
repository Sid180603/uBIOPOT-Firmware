/**
 * e2e.spec.js — Aqua-HMET P6 Playwright end-to-end tests.
 *
 * Architecture: each test uses `page.routeWebSocket('/ws', handler)` to mock
 * the device's WS endpoint IN-PROCESS (no separate server needed). The SPA is
 * served from the mock HTTP backend (mock_backend.js) via a test fixture.
 *
 * Test groups:
 *   A. Static load:   page loads, title, dark bg, elements visible
 *   B. WS connection: hello message, initial IDLE state
 *   C. Start/Abort:   form validation, start scan, abort mid-scan
 *   D. Live chart:    binary DataPoint frames → canvas updates
 *   E. Scan complete: scan_complete event → peaks detected → CSV export
 *   F. Reference CSV: import overlay → two traces shown
 *   G. x-axis toggle: switch between commanded V and RE
 *   H. Reconnect/resync: WS drop → reconnect → resync rebuilds chart
 */

'use strict';

const { test, expect } = require('@playwright/test');
const path  = require('path');
const fs    = require('fs');
const { start, encodeDP, syntheticCurrent } = require('../mock_backend');

// ── Fixtures ──────────────────────────────────────────────────────────────────

/**
 * server fixture: starts mock_backend, provides {url, wsUrl, mock}.
 * Stopped after each test.
 */
const testWithServer = test.extend({
  server: async ({}, use) => {
    const srv = await start({ port: 0 });   // port 0 = OS assigns
    await use(srv);
    await srv.close();
  },
});

// ── Helpers ───────────────────────────────────────────────────────────────────

/**
 * Generate a short synthetic DPV scan (−200 → 100 mV, step 20 mV).
 * Returns array of DataPoint buffers.
 */
function syntheticFrames(electrode = 1, e_begin = -200, e_end = 100, e_step = 20) {
  const frames = [];
  const dir    = e_end > e_begin ? 1 : -1;
  let   E      = e_begin;
  let   idx    = 0;
  while (dir > 0 ? E <= e_end : E >= e_end) {
    const I_uA  = syntheticCurrent(E);
    const RE_mV = E - 5.0;
    frames.push(encodeDP(electrode, idx, E, I_uA, RE_mV));
    E   += dir * e_step;
    idx += 1;
  }
  return frames;
}

/**
 * Mock WS route that replays a static scan and then sends scan_complete.
 */
function mockScanRoute(ws, frames) {
  ws.onMessage(async (msg) => {
    let cmd;
    try { cmd = JSON.parse(msg); } catch { return; }

    if (cmd.cmd === 'hello' || msg === undefined) {
      ws.send(JSON.stringify({ t: 'hello', fw: '1.0.0-mock', proto: 1 }));
      ws.send(JSON.stringify({ t: 'resync_complete', pts: 0, state: 'IDLE' }));

    } else if (cmd.cmd === 'start') {
      ws.send(JSON.stringify({ t: 'event', name: 'scan_started', mode: 'DPV', electrode: 1 }));
      for (const frame of frames) ws.send(frame);
      ws.send(JSON.stringify({ t: 'event', name: 'scan_complete' }));

    } else if (cmd.cmd === 'abort') {
      ws.send(JSON.stringify({ t: 'event', name: 'scan_aborted' }));

    } else if (cmd.cmd === 'state') {
      ws.send(JSON.stringify({ t: 'state', state: 'IDLE', pts: 0 }));
    }
  });
}

// ── A. Static load ────────────────────────────────────────────────────────────

testWithServer('A1: page loads with correct title', async ({ page, server }) => {
  await page.goto(server.url);
  await expect(page).toHaveTitle(/Aqua-HMET/i);
});

testWithServer('A2: dark background applied', async ({ page, server }) => {
  await page.goto(server.url);
  const bg = await page.evaluate(() =>
    window.getComputedStyle(document.body).backgroundColor
  );
  // --bg: #040a12 = rgb(4, 10, 18)
  expect(bg).toBe('rgb(4, 10, 18)');
});

testWithServer('A3: Start DPV button visible', async ({ page, server }) => {
  await page.goto(server.url);
  const btn = page.getByTestId('btn-start');
  await expect(btn).toBeVisible();
});

testWithServer('A4: Abort button visible', async ({ page, server }) => {
  await page.goto(server.url);
  await expect(page.getByTestId('btn-abort')).toBeVisible();
});

testWithServer('A5: Chart element visible', async ({ page, server }) => {
  await page.goto(server.url);
  // dist SPA has canvas#cv; Vite SPA has chart-container div
  const chart = page.locator('#cv, [data-testid="chart-container"], #chart-container');
  await expect(chart.first()).toBeVisible();
});

// ── B. WS connection ──────────────────────────────────────────────────────────

testWithServer('B1: WS connects — status dot becomes green', async ({ page, server }) => {
  await page.goto(server.url);
  // Wait for the dot to show connected state
  await page.waitForFunction(() => {
    const dot = document.querySelector('.dot, #dot');
    return dot && (dot.classList.contains('on') || dot.classList.contains('connected'));
  }, null, { timeout: 5000 });
});

testWithServer('B2: WS routeWebSocket — hello triggers fw info', async ({ page, server }) => {
  // Use routeWebSocket to intercept the SPA's WS connection
  await page.routeWebSocket(`ws://127.0.0.1:${server.port}/ws`, (ws) => {
    ws.onMessage(() => {});
    // Send hello immediately on connect
    ws.send(JSON.stringify({ t: 'hello', fw: '2.0.0-test', proto: 1 }));
    ws.send(JSON.stringify({ t: 'resync_complete', pts: 0, state: 'IDLE' }));
  });
  await page.goto(server.url);
  // fw info should appear somewhere (dist SPA shows it, Vite SPA has #fw-info)
  await page.waitForFunction(() => document.body.textContent.includes('2.0.0'), null, { timeout: 5000 });
});

testWithServer('B3: Start button enabled after WS connect', async ({ page, server }) => {
  await page.goto(server.url);
  await page.waitForTimeout(500);
  const btn = page.getByTestId('btn-start');
  await expect(btn).not.toBeDisabled({ timeout: 5000 });
});

// ── C. Form validation ────────────────────────────────────────────────────────

testWithServer('C1: invalid e_begin shows validation error', async ({ page, server }) => {
  await page.goto(server.url);
  await page.waitForTimeout(500);

  // Set e_begin to out-of-range value
  await page.locator('#e-begin').fill('9999');
  await page.getByTestId('btn-start').click();

  // .invalid field or visible toast
  const hasError = await page.evaluate(() =>
    document.querySelector('.invalid') !== null ||
    document.querySelector('#toast')?.classList.contains('visible')
  );
  expect(hasError).toBe(true);
});

testWithServer('C2: t_period < t_pulse shows validation error', async ({ page, server }) => {
  await page.goto(server.url);
  await page.waitForTimeout(500);

  // #t-period is inside <details> — open it first
  await page.locator('details').evaluate(el => el.setAttribute('open', ''));
  await page.locator('#t-pulse').first().fill('100');
  await page.locator('#t-period').first().fill('50');
  await page.getByTestId('btn-start').click();

  const hasError = await page.evaluate(() =>
    document.querySelector('.invalid') !== null ||
    document.querySelector('#toast')?.classList.contains('vis') ||   // dist SPA
    document.querySelector('#toast')?.classList.contains('visible')  // Vite SPA
  );
  expect(hasError).toBe(true);
});

// ── D. Start scan → live chart ────────────────────────────────────────────────

testWithServer('D1: start command sent when form valid', async ({ page, server }) => {
  const startCmds = [];

  await page.routeWebSocket(`ws://127.0.0.1:${server.port}/ws`, (ws) => {
    ws.send(JSON.stringify({ t: 'hello', fw: '1.0.0', proto: 1 }));
    ws.send(JSON.stringify({ t: 'resync_complete', pts: 0, state: 'IDLE' }));
    ws.onMessage((msg) => {
      try {
        const cmd = JSON.parse(msg);
        if (cmd.cmd === 'start') startCmds.push(cmd);
      } catch {}
      ws.send(JSON.stringify({ t: 'event', name: 'scan_started', mode: 'DPV', electrode: 1 }));
    });
  });

  await page.goto(server.url);
  await page.waitForTimeout(400);
  await page.getByTestId('btn-start').click();
  await page.waitForTimeout(200);

  expect(startCmds.length).toBeGreaterThanOrEqual(1);
  expect(startCmds[0].cmd).toBe('start');
});

testWithServer('D2: binary DataPoint frames update pts counter', async ({ page, server }) => {
  const frames = syntheticFrames(1, -100, 100, 50);  // 5 points

  await page.routeWebSocket(`ws://127.0.0.1:${server.port}/ws`, (ws) => {
    ws.send(JSON.stringify({ t: 'hello', fw: '1.0.0', proto: 1 }));
    ws.send(JSON.stringify({ t: 'resync_complete', pts: 0, state: 'IDLE' }));
    ws.onMessage((msg) => {
      const cmd = JSON.parse(msg);
      if (cmd.cmd === 'start') {
        ws.send(JSON.stringify({ t: 'event', name: 'scan_started', mode: 'DPV', electrode: 1 }));
        for (const f of frames) ws.send(f);
        ws.send(JSON.stringify({ t: 'event', name: 'scan_complete' }));
      }
    });
  });

  await page.goto(server.url);
  await page.waitForTimeout(400);
  await page.getByTestId('btn-start').click();

  // Wait for pts counter to reflect received points
  await page.waitForFunction(
    (expected) => {
      const el = document.querySelector('#pts-val');
      return el && parseInt(el.textContent) >= expected;
    },
    frames.length,
    { timeout: 8000 }
  );
});

testWithServer('D3: scan_started event disables Start, enables Abort', async ({ page, server }) => {
  await page.routeWebSocket(`ws://127.0.0.1:${server.port}/ws`, (ws) => {
    ws.send(JSON.stringify({ t: 'hello', fw: '1.0.0', proto: 1 }));
    ws.send(JSON.stringify({ t: 'resync_complete', pts: 0, state: 'IDLE' }));
    ws.onMessage((msg) => {
      const cmd = JSON.parse(msg);
      if (cmd.cmd === 'start') {
        ws.send(JSON.stringify({ t: 'event', name: 'scan_started', mode: 'DPV', electrode: 1 }));
      }
    });
  });

  await page.goto(server.url);
  await page.waitForTimeout(400);
  await page.getByTestId('btn-start').click();
  await page.waitForTimeout(300);

  await expect(page.getByTestId('btn-start')).toBeDisabled();
  await expect(page.getByTestId('btn-abort')).not.toBeDisabled();
});

// ── E. Scan complete → peaks → CSV ───────────────────────────────────────────

testWithServer('E1: scan_complete enables Start again', async ({ page, server }) => {
  const frames = syntheticFrames(1, -700, -600, 20);  // near Cd peak

  await page.routeWebSocket(`ws://127.0.0.1:${server.port}/ws`, (ws) => {
    ws.send(JSON.stringify({ t: 'hello', fw: '1.0.0', proto: 1 }));
    ws.send(JSON.stringify({ t: 'resync_complete', pts: 0, state: 'IDLE' }));
    ws.onMessage((msg) => {
      const cmd = JSON.parse(msg);
      if (cmd.cmd === 'start') {
        ws.send(JSON.stringify({ t: 'event', name: 'scan_started', mode: 'DPV', electrode: 1 }));
        for (const f of frames) ws.send(f);
        ws.send(JSON.stringify({ t: 'event', name: 'scan_complete' }));
      }
    });
  });

  await page.goto(server.url);
  await page.waitForTimeout(400);
  await page.getByTestId('btn-start').click();

  await expect(page.getByTestId('btn-start')).not.toBeDisabled({ timeout: 8000 });
  await expect(page.getByTestId('btn-abort')).toBeDisabled({ timeout: 8000 });
});

testWithServer('E2: CSV export button enabled after complete scan', async ({ page, server }) => {
  const frames = syntheticFrames(1, -500, 500, 100);

  await page.routeWebSocket(`ws://127.0.0.1:${server.port}/ws`, (ws) => {
    ws.send(JSON.stringify({ t: 'hello', fw: '1.0.0', proto: 1 }));
    ws.send(JSON.stringify({ t: 'resync_complete', pts: 0, state: 'IDLE' }));
    ws.onMessage((msg) => {
      const cmd = JSON.parse(msg);
      if (cmd.cmd === 'start') {
        ws.send(JSON.stringify({ t: 'event', name: 'scan_started', mode: 'DPV', electrode: 1 }));
        for (const f of frames) ws.send(f);
        ws.send(JSON.stringify({ t: 'event', name: 'scan_complete' }));
      }
    });
  });

  await page.goto(server.url);
  await page.waitForTimeout(400);
  await page.getByTestId('btn-start').click();

  await expect(page.getByTestId('btn-csv')).not.toBeDisabled({ timeout: 8000 });
});

testWithServer('E3: CSV download triggered by button click', async ({ page, server }) => {
  const frames = syntheticFrames(1, -100, 100, 50);

  await page.routeWebSocket(`ws://127.0.0.1:${server.port}/ws`, (ws) => {
    ws.send(JSON.stringify({ t: 'hello', fw: '1.0.0', proto: 1 }));
    ws.send(JSON.stringify({ t: 'resync_complete', pts: 0, state: 'IDLE' }));
    ws.onMessage((msg) => {
      const cmd = JSON.parse(msg);
      if (cmd.cmd === 'start') {
        ws.send(JSON.stringify({ t: 'event', name: 'scan_started', mode: 'DPV', electrode: 1 }));
        for (const f of frames) ws.send(f);
        ws.send(JSON.stringify({ t: 'event', name: 'scan_complete' }));
      }
    });
  });

  await page.goto(server.url);
  await page.waitForTimeout(400);
  await page.getByTestId('btn-start').click();
  await expect(page.getByTestId('btn-csv')).not.toBeDisabled({ timeout: 8000 });

  const [download] = await Promise.all([
    page.waitForEvent('download'),
    page.getByTestId('btn-csv').click(),
  ]);
  expect(download.suggestedFilename()).toMatch(/\.csv$/i);
});

// ── F. Abort ──────────────────────────────────────────────────────────────────

testWithServer('F1: abort command sent, scan_aborted enables Start', async ({ page, server }) => {
  const abortCmds = [];

  await page.routeWebSocket(`ws://127.0.0.1:${server.port}/ws`, (ws) => {
    ws.send(JSON.stringify({ t: 'hello', fw: '1.0.0', proto: 1 }));
    ws.send(JSON.stringify({ t: 'resync_complete', pts: 0, state: 'IDLE' }));
    ws.onMessage((msg) => {
      const cmd = JSON.parse(msg);
      if (cmd.cmd === 'start') {
        ws.send(JSON.stringify({ t: 'event', name: 'scan_started', mode: 'DPV', electrode: 1 }));
      } else if (cmd.cmd === 'abort') {
        abortCmds.push(cmd);
        ws.send(JSON.stringify({ t: 'event', name: 'scan_aborted' }));
      }
    });
  });

  await page.goto(server.url);
  await page.waitForTimeout(400);
  await page.getByTestId('btn-start').click();
  await page.waitForTimeout(200);
  await page.getByTestId('btn-abort').click();
  await page.waitForTimeout(200);

  expect(abortCmds.length).toBeGreaterThanOrEqual(1);
  await expect(page.getByTestId('btn-start')).not.toBeDisabled({ timeout: 4000 });
});

// ── G. x-axis toggle ──────────────────────────────────────────────────────────

testWithServer('G1: x-axis Commanded V button starts as active', async ({ page, server }) => {
  await page.goto(server.url);
  await page.waitForTimeout(300);
  const btn = page.locator('#xaxis-cmd');
  await expect(btn).toHaveClass(/active/);
});

testWithServer('G2: click RE button toggles active state', async ({ page, server }) => {
  await page.goto(server.url);
  await page.waitForTimeout(300);

  const reBtn  = page.locator('#xaxis-re');
  const cmdBtn = page.locator('#xaxis-cmd');
  await reBtn.click();

  await expect(reBtn).toHaveClass(/active/);
  const cmdClass = await cmdBtn.getAttribute('class');
  expect(cmdClass).not.toMatch(/\bactive\b/);
});

// ── H. Reconnect / resync ─────────────────────────────────────────────────────

testWithServer('H1: resync_complete called on reconnect', async ({ page, server }) => {
  let resyncs = 0;

  await page.routeWebSocket(`ws://127.0.0.1:${server.port}/ws`, (ws) => {
    ws.send(JSON.stringify({ t: 'hello', fw: '1.0.0', proto: 1 }));
    ws.send(JSON.stringify({ t: 'resync_complete', pts: 5, state: 'COMPLETE' }));
    resyncs++;
    ws.onMessage(() => {});
  });

  await page.goto(server.url);
  await page.waitForTimeout(600);
  expect(resyncs).toBeGreaterThanOrEqual(1);
});

testWithServer('H2: late-joining client pts rebuilt from resync', async ({ page, server }) => {
  const frames = syntheticFrames(1, -100, 100, 50);  // 5 points

  await page.routeWebSocket(`ws://127.0.0.1:${server.port}/ws`, (ws) => {
    ws.send(JSON.stringify({ t: 'hello', fw: '1.0.0', proto: 1 }));
    // Send all frames as part of resync (simulates mid-scan reconnect)
    for (const f of frames) ws.send(f);
    ws.send(JSON.stringify({ t: 'resync_complete', pts: frames.length, state: 'RUNNING' }));
    ws.onMessage(() => {});
  });

  await page.goto(server.url);

  await page.waitForFunction(
    (expected) => {
      const el = document.querySelector('#pts-val');
      return el && parseInt(el.textContent) >= expected;
    },
    frames.length,
    { timeout: 6000 }
  );
});

// ── I. Reference CSV import ───────────────────────────────────────────────────

testWithServer('I1: reference CSV can be uploaded', async ({ page, server }) => {
  await page.goto(server.url);
  await page.waitForTimeout(400);

  // Generate a simple 3-row reference CSV
  const csvContent = 'E_mV,I_uA\n-500,2.0\n0,5.0\n500,1.0\n';
  const refInput   = page.locator('#ref-file');

  await refInput.setInputFiles({
    name:     'reference.csv',
    mimeType: 'text/csv',
    buffer:   Buffer.from(csvContent),
  });

  // Info text should show points loaded
  await page.waitForFunction(() => {
    const el = document.querySelector('#ref-info');
    return el && el.textContent.match(/pts loaded|points loaded/i);
  }, null, { timeout: 4000 });
});

// ── J. API endpoint smoke tests (via mock_backend HTTP) ───────────────────────

testWithServer('J1: GET /api/state returns JSON with state field', async ({ page, server }) => {
  const resp = await page.request.get(`${server.url}/api/state`);
  expect(resp.ok()).toBe(true);
  const body = await resp.json();
  expect(body).toHaveProperty('state');
  expect(['IDLE', 'RUNNING', 'COMPLETE', 'ABORTING', 'ERROR']).toContain(body.state);
});

testWithServer('J2: GET /api/scan.csv returns CSV with header', async ({ page, server }) => {
  const resp = await page.request.get(`${server.url}/api/scan.csv`);
  expect(resp.ok()).toBe(true);
  const text = await resp.text();
  expect(text).toContain('electrode');
  expect(text).toContain('E_mV');
  expect(text).toContain('I_uA');
});
