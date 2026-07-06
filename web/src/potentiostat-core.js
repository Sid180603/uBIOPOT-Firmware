/**
 * potentiostat-core.js — Transport-agnostic core for Aqua-HMET P6/P9.
 *
 * This module is the KEYSTONE of the web stack. It knows NOTHING about
 * transport: P6 feeds it via WebSocket; P9 feeds it via Web Serial.
 * One module, two transports.
 *
 * Responsibilities:
 *   1. Binary WS DataPoint frame parsing (16 bytes LE — mirrors net_comms_protocol.h)
 *   2. JSON event dispatch
 *   3. Client-side scan state machine
 *   4. DPV parameter model + validation (mirrors echem_core dpv.h)
 *   5. CSV export (scan data download)
 *   6. Reference CSV import (Option-B commercial overlay)
 *   7. Simple JS peak detection (no scipy — prominence-based local max)
 *
 * Usage:
 *   import { Core } from './potentiostat-core.js';
 *   const core = new Core({ onPoint, onEvent, onState });
 *   core.feedBinary(arrayBuffer);   // binary DataPoint frame
 *   core.feedText(jsonString);      // JSON event/state/hello
 *   core.reset();                   // start of new scan
 */

// =============================================================================
// Wire protocol constants — mirror net_comms_protocol.h
// =============================================================================

export const PROTOCOL_VERSION     = 1;
export const WS_FRAME_TYPE_DP     = 0x01;
export const WS_FRAME_SIZE        = 16;

// Binary frame layout (little-endian):
//   [0]    frame_type  u8   = 0x01
//   [1]    electrode   u8   (1/2/3)
//   [2-3]  idx         u16  LE
//   [4-7]  E_mV        f32  LE (base potential, mV)
//   [8-11] I_uA        f32  LE (dI = I_pulse - I_base, µA)
//  [12-15] RE_mV       f32  LE (measured RE voltage, mV)

/**
 * Parse a 16-byte binary DataPoint frame.
 * @param {ArrayBuffer|DataView} buf
 * @returns {{electrode,idx,E_mV,I_uA,RE_mV}|null}  null if malformed.
 */
export function parseDataPoint(buf) {
  const dv = buf instanceof DataView ? buf : new DataView(buf);
  if (dv.byteLength !== WS_FRAME_SIZE) return null;
  if (dv.getUint8(0) !== WS_FRAME_TYPE_DP) return null;
  return {
    electrode: dv.getUint8(1),
    idx:       dv.getUint16(2, true /* LE */),
    E_mV:      dv.getFloat32(4, true),
    I_uA:      dv.getFloat32(8, true),
    RE_mV:     dv.getFloat32(12, true),
  };
}

/**
 * Encode a DataPoint as a 16-byte binary frame (for testing mock backends).
 * @returns {ArrayBuffer}
 */
export function encodeDataPoint({ electrode, idx, E_mV, I_uA, RE_mV }) {
  const buf = new ArrayBuffer(WS_FRAME_SIZE);
  const dv  = new DataView(buf);
  dv.setUint8(0,   WS_FRAME_TYPE_DP);
  dv.setUint8(1,   electrode);
  dv.setUint16(2,  idx,   true);
  dv.setFloat32(4, E_mV,  true);
  dv.setFloat32(8, I_uA,  true);
  dv.setFloat32(12,RE_mV, true);
  return buf;
}

// =============================================================================
// DPV parameter model + validation — mirrors echem_core dpv.h
// =============================================================================

export const DPV_DEFAULTS = {
  e_begin_mV:          -500,
  e_end_mV:             500,
  e_step_mV:              5,
  e_pulse_mV:            25,
  t_pulse_ms:            50,
  t_period_ms:          200,
  t_equilibration_ms:  2000,
  cycles:                 1,
  n_avg:                  5,
  electrode:              1,   // 0 = ALL, 1/2/3 = single
};

/**
 * Validate DPV parameters (mirrors echem_core dpv_validate()).
 * @param {object} p   parameter object
 * @returns {string|null}  error message, or null if valid
 */
export function validateDpvParams(p) {
  if (p.e_begin_mV < -1000 || p.e_begin_mV > 1000) return 'e_begin outside ±1000 mV';
  if (p.e_end_mV   < -1000 || p.e_end_mV   > 1000) return 'e_end outside ±1000 mV';
  if (p.e_begin_mV === p.e_end_mV)                  return 'e_begin == e_end';
  if (p.e_step_mV  <= 0)                            return 'e_step must be > 0';
  if (p.e_pulse_mV <= 0)                            return 'e_pulse must be > 0';
  if (p.t_pulse_ms <= 0)                            return 't_pulse must be > 0';
  if (p.t_period_ms <= p.t_pulse_ms)                return 't_period must exceed t_pulse';
  if (p.cycles < 1)                                 return 'cycles must be >= 1';
  if (p.electrode < 0 || p.electrode > 3)           return 'electrode must be 0-3';
  return null;  // valid
}

// =============================================================================
// CSV export / import
// =============================================================================

export const CSV_HEADER = 'electrode,idx,E_mV,I_uA,RE_mV\r\n';

/**
 * Export scan data as CSV string.
 * @param {Array<{electrode,idx,E_mV,I_uA,RE_mV}>} points
 * @returns {string}
 */
export function exportCsv(points) {
  let csv = CSV_HEADER;
  for (const p of points) {
    csv += `${p.electrode},${p.idx},${p.E_mV.toFixed(4)},${p.I_uA.toFixed(4)},${p.RE_mV.toFixed(4)}\r\n`;
  }
  return csv;
}

/**
 * Import a CSV string (from a commercial instrument or device).
 * Accepts both CRLF and LF line endings; skips blank lines and header row.
 * Columns: electrode, idx, E_mV, I_uA, RE_mV  (or just E_mV,I_uA for 2-col).
 *
 * @param {string} text
 * @returns {Array<{electrode,idx,E_mV,I_uA,RE_mV}>}
 */
export function importCsv(text) {
  const lines  = text.replace(/\r\n/g, '\n').replace(/\r/g, '\n').split('\n');
  const points = [];
  let   idx    = 0;

  for (const raw of lines) {
    const line = raw.trim();
    if (!line) continue;

    const cols = line.split(',');

    // Skip header row (contains non-numeric first col)
    if (isNaN(parseFloat(cols[0]))) continue;

    if (cols.length >= 5) {
      // Full format: electrode,idx,E_mV,I_uA,RE_mV
      points.push({
        electrode: parseInt(cols[0], 10) || 1,
        idx:       parseInt(cols[1], 10) || idx,
        E_mV:      parseFloat(cols[2]),
        I_uA:      parseFloat(cols[3]),
        RE_mV:     parseFloat(cols[4]),
      });
    } else if (cols.length >= 2) {
      // Minimal format from commercial instruments: E_mV,I_uA
      points.push({
        electrode: 0,   // unknown — show as grey in UI
        idx,
        E_mV:  parseFloat(cols[0]),
        I_uA:  parseFloat(cols[1]),
        RE_mV: parseFloat(cols[0]),  // no RE — use commanded V
      });
    }
    idx++;
  }
  return points;
}

/**
 * Trigger a browser CSV download without needing a server.
 * @param {string} csv   CSV text content
 * @param {string} name  filename suggestion
 */
export function downloadCsv(csv, name = 'aquahmet_scan.csv') {
  const blob = new Blob([csv], { type: 'text/csv' });
  const url  = URL.createObjectURL(blob);
  const a    = document.createElement('a');
  a.href     = url;
  a.download = name;
  a.click();
  URL.revokeObjectURL(url);
}

// =============================================================================
// Simple JS peak detection
// =============================================================================

/**
 * Find local maxima in a 1-D array using a prominence-based filter.
 *
 * A peak at index i is included if:
 *   - data[i] > data[i-1] && data[i] > data[i+1]  (local max)
 *   - prominence >= minProminence  (signal rise above surrounding valleys)
 *   - data[i] >= absoluteMin
 *
 * @param {number[]} x     x-axis values (potential, mV)
 * @param {number[]} y     y-axis values (current, µA)
 * @param {object}   opts
 *   minProminence {number}  minimum height above surrounding baseline (µA), default 3
 *   absoluteMin   {number}  absolute minimum y value to be a peak, default 0
 *   minSepMv      {number}  minimum separation between peaks in mV, default 50
 * @returns {Array<{E_mV, I_uA, idx}>}
 */
export function findPeaks(x, y, opts = {}) {
  const { minProminence = 3, absoluteMin = 0, minSepMv = 50 } = opts;
  const n = y.length;
  if (n < 3) return [];

  // Step 1: find all local maxima
  const candidates = [];
  for (let i = 1; i < n - 1; i++) {
    if (y[i] > y[i - 1] && y[i] > y[i + 1] && y[i] >= absoluteMin) {
      // Prominence: height above the higher of the two surrounding valleys
      const leftMin  = Math.min(...y.slice(0, i));
      const rightMin = Math.min(...y.slice(i + 1));
      const prominence = y[i] - Math.max(leftMin, rightMin);
      if (prominence >= minProminence) {
        candidates.push({ idx: i, E_mV: x[i], I_uA: y[i], prominence });
      }
    }
  }

  // Step 2: non-maximum suppression (keep tallest in each min-sep window)
  candidates.sort((a, b) => b.I_uA - a.I_uA); // tallest first
  const kept = [];
  for (const c of candidates) {
    const tooClose = kept.some(k => Math.abs(k.E_mV - c.E_mV) < minSepMv);
    if (!tooClose) kept.push(c);
  }

  kept.sort((a, b) => a.E_mV - b.E_mV); // sort by potential
  return kept.map(({ idx, E_mV, I_uA }) => ({ idx, E_mV, I_uA }));
}

// =============================================================================
// Client scan state machine
// =============================================================================

export const STATE = {
  IDLE:          'IDLE',
  EQUILIBRATING: 'EQUILIBRATING',
  RUNNING:       'RUNNING',
  COMPLETE:      'COMPLETE',
  ABORTING:      'ABORTING',
  ERROR:         'ERROR',
};

// =============================================================================
// Core class — wires everything together
// =============================================================================

/**
 * Core — receives raw transport bytes/text, emits decoded application events.
 *
 * @example
 *   const core = new Core({
 *     onPoint:  (pt) => chart.addPoint(pt),
 *     onEvent:  (evt) => ui.handleEvent(evt),
 *     onState:  (state) => ui.setStatus(state),
 *     onHello:  (info) => console.log('fw', info.fw),
 *   });
 *
 *   // Feed from WebSocket:
 *   ws.onmessage = e => {
 *     if (e.data instanceof ArrayBuffer) core.feedBinary(e.data);
 *     else                              core.feedText(e.data);
 *   };
 */
export class Core {
  /**
   * @param {object} callbacks
   *   onPoint (pt) — called for each DataPoint
   *   onEvent (msg) — called for JSON events (scan_started, scan_complete, etc.)
   *   onState (state) — called when state transitions
   *   onHello (info) — called on hello frame
   *   onResync (pts) — called when a resync snapshot arrives
   *   onError (msg)  — called for scan_error events
   */
  constructor(callbacks = {}) {
    this._cb    = callbacks;
    this.points = [];          // current scan buffer (server-authoritative)
    this.state  = STATE.IDLE;
    this.refPoints = [];       // reference CSV points (Option-B overlay)
  }

  /** Feed a raw binary ArrayBuffer (DataPoint frame). */
  feedBinary(buf) {
    const pt = parseDataPoint(buf);
    if (!pt) return;
    this.points.push(pt);
    this._cb.onPoint && this._cb.onPoint(pt);
  }

  /** Feed a JSON text string (event/state/hello/resync). */
  feedText(text) {
    let msg;
    try { msg = JSON.parse(text); } catch { return; }

    if (msg.t === 'hello') {
      this._cb.onHello && this._cb.onHello(msg);

    } else if (msg.t === 'resync_complete') {
      this._cb.onResync && this._cb.onResync(this.points, msg.state);

    } else if (msg.t === 'state') {
      this.state = msg.state || STATE.IDLE;
      this._cb.onState && this._cb.onState(this.state, msg);

    } else if (msg.t === 'event') {
      this._handleEvent(msg);

    } else if (msg.t === 'error') {
      this._cb.onError && this._cb.onError(msg.msg || 'Unknown error');
    }
  }

  _handleEvent(msg) {
    const name = msg.name || '';

    if (name === 'scan_started') {
      this.points = [];   // clear for new scan
      this.state  = STATE.RUNNING;
      this._cb.onState && this._cb.onState(STATE.RUNNING, msg);

    } else if (name === 'equilibrating') {
      this.state = STATE.EQUILIBRATING;
      this._cb.onState && this._cb.onState(STATE.EQUILIBRATING, msg);

    } else if (name === 'scan_complete') {
      this.state = STATE.COMPLETE;
      this._cb.onState && this._cb.onState(STATE.COMPLETE, msg);

    } else if (name === 'scan_aborted') {
      this.state = STATE.IDLE;
      this._cb.onState && this._cb.onState(STATE.IDLE, msg);

    } else if (name === 'scan_error') {
      this.state = STATE.ERROR;
      this._cb.onError && this._cb.onError(msg.msg || 'Scan error');
      this._cb.onState && this._cb.onState(STATE.ERROR, msg);
    }

    this._cb.onEvent && this._cb.onEvent(msg);
  }

  /** Import reference CSV (Option-B commercial overlay). */
  loadReference(csvText) {
    this.refPoints = importCsv(csvText);
    return this.refPoints;
  }

  /** Export current scan as CSV string. */
  exportCsv() {
    return exportCsv(this.points);
  }

  /** Run JS peak detection on the current scan. */
  findPeaks(opts) {
    const xs = this.points.map(p => p.E_mV);
    const ys = this.points.map(p => p.I_uA);
    return findPeaks(xs, ys, opts);
  }

  /** Get unique electrode numbers seen in the current scan. */
  get electrodes() {
    return [...new Set(this.points.map(p => p.electrode))].sort();
  }

  /** Get scan data filtered by electrode (all if electrode === 0). */
  pointsForElectrode(electrode) {
    if (electrode === 0) return this.points;
    return this.points.filter(p => p.electrode === electrode);
  }

  reset() {
    this.points = [];
    this.state  = STATE.IDLE;
  }
}
