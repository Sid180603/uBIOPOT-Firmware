/**
 * mock_backend.js — Aqua-HMET P6 development + Playwright mock server.
 *
 * Serves the SPA from web/dist/ and provides a /ws WebSocket endpoint
 * that speaks the exact P5 binary wire protocol.
 *
 * Usage:
 *   node web/mock_backend.js [--port 3000] [--dist ./web/dist]
 *
 * The mock server is also importable by Playwright tests as a fixture
 * (see web/tests/e2e.spec.js — or use Playwright's routeWebSocket instead).
 *
 * Wire protocol: mirrors net_comms_protocol.h + mock_ws_server.py
 *   Binary outbound : 16-byte DataPoint frame (LE float32, DP_TYPE=0x01)
 *   Text outbound   : JSON events (hello/event/state/resync_complete)
 *   Text inbound    : JSON commands (start/abort/state/hello/zero)
 */

'use strict';

const http    = require('http');
const fs      = require('fs');
const path    = require('path');
const { WebSocketServer, WebSocket } = require('ws');

// ── Binary frame helpers ──────────────────────────────────────────────────────

const DP_TYPE = 0x01;
const DP_SIZE = 16;

function encodeDP(electrode, idx, E_mV, I_uA, RE_mV) {
  const buf = Buffer.alloc(DP_SIZE);
  buf.writeUInt8(DP_TYPE,  0);
  buf.writeUInt8(electrode, 1);
  buf.writeUInt16LE(idx,    2);
  buf.writeFloatLE(E_mV,    4);
  buf.writeFloatLE(I_uA,    8);
  buf.writeFloatLE(RE_mV,  12);
  return buf;
}

// ── Synthetic cell model — Gaussian peaks ────────────────────────────────────
// Same model as mock_ws_server.py + shared echem_core test model.
// Peaks: Cd²⁺ ~−700 mV, Pb²⁺ ~−400 mV, Cu²⁺ ~0 mV

const MOCK_PEAKS = [
  { E_peak: -700, I_peak: 45, sigma: 60 },   // Cd²⁺
  { E_peak: -400, I_peak: 30, sigma: 50 },   // Pb²⁺
  { E_peak:    0, I_peak: 20, sigma: 55 },   // Cu²⁺
];

function syntheticCurrent(E_mV) {
  let I = 0;
  for (const pk of MOCK_PEAKS) {
    I += pk.I_peak * Math.exp(-0.5 * ((E_mV - pk.E_peak) / pk.sigma) ** 2);
  }
  I += 0.5 * Math.sin(E_mV * 0.01);   // deterministic noise
  return I;
}

// ── Server state ──────────────────────────────────────────────────────────────

class MockServer {
  constructor() {
    this.clients  = new Set();
    this.scanBuf  = [];      // [{electrode,idx,E_mV,I_uA,RE_mV}]
    this.state    = 'IDLE';
    this.scanning = false;
  }

  broadcast(data) {
    for (const ws of this.clients) {
      if (ws.readyState === WebSocket.OPEN) ws.send(data);
    }
  }

  async startScan(electrode = 1, e_begin = -900, e_end = 200, e_step = 10) {
    if (this.scanning) return;
    this.scanning = true;
    this.scanBuf  = [];
    this.state    = 'RUNNING';

    this.broadcast(JSON.stringify({
      t: 'event', name: 'scan_started', mode: 'DPV', electrode,
    }));

    const dir   = e_end > e_begin ? 1 : -1;
    let   E     = e_begin;
    let   idx   = 0;

    while (this.scanning && (dir > 0 ? E <= e_end : E >= e_end)) {
      const I_uA  = syntheticCurrent(E);
      const RE_mV = E - 5.0;
      const frame = encodeDP(electrode, idx, E, I_uA, RE_mV);
      this.broadcast(frame);
      this.scanBuf.push({ electrode, idx, E_mV: E, I_uA, RE_mV });
      E   += dir * e_step;
      idx += 1;
      // Simulate 2 ms per step
      await new Promise(r => setTimeout(r, 2));
    }

    if (this.scanning) {
      this.state    = 'COMPLETE';
      this.scanning = false;
      this.broadcast(JSON.stringify({ t: 'event', name: 'scan_complete' }));
      await new Promise(r => setTimeout(r, 100));
      this.state = 'IDLE';
    }
  }

  abort() {
    if (!this.scanning) return;
    this.scanning = false;
    this.state    = 'IDLE';
    this.broadcast(JSON.stringify({ t: 'event', name: 'scan_aborted' }));
  }

  resync(ws) {
    // Replay scan buffer to a single client
    for (const pt of this.scanBuf) {
      const frame = encodeDP(pt.electrode, pt.idx, pt.E_mV, pt.I_uA, pt.RE_mV);
      ws.send(frame);
    }
    ws.send(JSON.stringify({
      t:     'resync_complete',
      pts:   this.scanBuf.length,
      state: this.state,
    }));
  }

  handleClient(ws) {
    this.clients.add(ws);

    // Send hello
    ws.send(JSON.stringify({
      t:     'hello',
      fw:    '1.0.0-mock',
      proto: 1,
    }));

    // Resync if scan in progress
    this.resync(ws);

    ws.on('message', (raw) => {
      let cmd;
      try { cmd = JSON.parse(raw.toString()); } catch { return; }

      switch (cmd.cmd) {
        case 'start': {
          if (this.state !== 'IDLE') {
            ws.send(JSON.stringify({ t: 'error', msg: 'scan already running' }));
            break;
          }
          const p   = cmd.params || {};
          const elecParsed = parseInt(cmd.electrode, 10);
          const elec = isNaN(elecParsed) ? 1 : elecParsed;
          this.startScan(
            elec,
            parseFloat(p.e_begin_mV) || -900,
            parseFloat(p.e_end_mV)   || 200,
            parseFloat(p.e_step_mV)  || 10,
          );
          break;
        }
        case 'abort':
          this.abort();
          break;
        case 'state':
          ws.send(JSON.stringify({ t: 'state', state: this.state, pts: this.scanBuf.length }));
          break;
        case 'hello':
          ws.send(JSON.stringify({ t: 'hello', fw: '1.0.0-mock', proto: 1 }));
          this.resync(ws);
          break;
        case 'zero':
          ws.send(JSON.stringify({ t: 'event', name: 'zero_done' }));
          break;
        default:
          break;
      }
    });

    ws.on('close', () => {
      this.clients.delete(ws);
    });
  }
}

// ── HTTP static file server ───────────────────────────────────────────────────

const MIME = {
  '.html': 'text/html',
  '.js':   'application/javascript',
  '.css':  'text/css',
  '.gz':   'application/gzip',
  '.json': 'application/json',
  '.ico':  'image/x-icon',
  '.png':  'image/png',
  '.svg':  'image/svg+xml',
};

function serveFile(distDir, urlPath, res) {
  // Sanitise path (prevent traversal)
  const safe = path.normalize(urlPath).replace(/^(\.\.[\\/])+/, '');
  let   file = path.join(distDir, safe);

  // Default to index.html — handle both Unix '/' and Windows '\\' normalised forms
  if (safe === '/' || safe === '\\' || safe === '' || safe === '.') file = path.join(distDir, 'index.html');

  // Also guard against a directory being resolved (e.g. trailing slash on sub-path)
  try {
    if (fs.statSync(file).isDirectory()) {
      file = path.join(file, 'index.html');
      if (!fs.existsSync(file)) file = path.join(distDir, 'index.html');
    }
  } catch (_) { /* file doesn't exist — let the 404 path below handle it */ }

  // Try .gz variant first
  const gzFile = file + '.gz';
  if (fs.existsSync(gzFile)) {
    const ext  = path.extname(file);
    const mime = MIME[ext] || 'text/plain';
    res.writeHead(200, { 'Content-Type': mime, 'Content-Encoding': 'gzip' });
    fs.createReadStream(gzFile).pipe(res);
    return;
  }

  if (!fs.existsSync(file)) {
    // SPA fallback: serve index.html for client-side routing
    const idx = path.join(distDir, 'index.html');
    if (fs.existsSync(idx)) {
      res.writeHead(200, { 'Content-Type': 'text/html' });
      fs.createReadStream(idx).pipe(res);
    } else {
      res.writeHead(404);
      res.end('Not found');
    }
    return;
  }

  const ext  = path.extname(file);
  const mime = MIME[ext] || 'text/plain';
  res.writeHead(200, { 'Content-Type': mime });
  fs.createReadStream(file).pipe(res);
}

// ── Start server ──────────────────────────────────────────────────────────────

/**
 * Create and start the mock server.
 * @param {object} opts
 *   port    {number}   HTTP port (default 3000)
 *   distDir {string}   path to SPA dist directory
 * @returns {Promise<{server, wss, mock, port, url, wsUrl}>}
 */
async function start({ port = 3000, distDir = path.join(__dirname, 'dist') } = {}) {
  const mock = new MockServer();

  const server = http.createServer((req, res) => {
    // REST API stubs for Playwright tests
    if (req.method === 'GET' && req.url === '/api/state') {
      res.writeHead(200, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({ state: mock.state, pts: mock.scanBuf.length }));
      return;
    }
    if (req.method === 'GET' && req.url === '/api/scan.csv') {
      let csv = 'electrode,idx,E_mV,I_uA,RE_mV\r\n';
      for (const p of mock.scanBuf) {
        csv += `${p.electrode},${p.idx},${p.E_mV.toFixed(4)},${p.I_uA.toFixed(4)},${p.RE_mV.toFixed(4)}\r\n`;
      }
      res.writeHead(200, { 'Content-Type': 'text/csv',
                           'Content-Disposition': 'attachment; filename="scan.csv"' });
      res.end(csv);
      return;
    }
    if (req.method === 'POST' && req.url === '/api/reference.csv') {
      let body = '';
      req.on('data', c => body += c);
      req.on('end', () => {
        res.writeHead(200, { 'Content-Type': 'application/json' });
        res.end('{"ok":true}');
      });
      return;
    }

    // Static file serving
    serveFile(distDir, req.url, res);
  });

  // Track raw TCP sockets so close() can destroy keep-alive connections instantly
  const sockets = new Set();
  server.on('connection', s => {
    sockets.add(s);
    s.on('close', () => sockets.delete(s));
  });

  const wss = new WebSocketServer({ server, path: '/ws' });
  wss.on('connection', (ws) => mock.handleClient(ws));

  await new Promise((resolve, reject) => {
    server.listen(port, '127.0.0.1', resolve);
    server.once('error', reject);
  });

  const actualPort = server.address().port;
  return {
    server, wss, mock,
    port:  actualPort,
    url:   `http://127.0.0.1:${actualPort}`,
    wsUrl: `ws://127.0.0.1:${actualPort}/ws`,
    close: () => new Promise(r => {
      // Terminate all open WebSocket connections first
      for (const ws of mock.clients) ws.terminate();
      // Destroy all TCP sockets so http.server.close() resolves immediately
      for (const s of sockets) s.destroy();
      server.close(r);
    }),
  };
}

module.exports = { start, encodeDP, syntheticCurrent, MockServer };

// ── CLI entry-point ───────────────────────────────────────────────────────────
if (require.main === module) {
  const args   = process.argv.slice(2);
  const port   = parseInt(args[args.indexOf('--port') + 1] || '3000', 10);
  const distDir = args[args.indexOf('--dist') + 1] || path.join(__dirname, 'dist');

  start({ port, distDir }).then(({ url, wsUrl }) => {
    console.log(`Mock server:  ${url}`);
    console.log(`WebSocket:    ${wsUrl}`);
    console.log('Ctrl+C to stop.');
  });
}
