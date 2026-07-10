/**
 * main.js — Aqua-HMET P6 SPA entry point.
 *
 * Orchestrates:
 *   - WebSocket connection (auto-reconnect)
 *   - potentiostat-core (transport-agnostic data processing)
 *   - uPlot chart (live streaming, per-frame rAF batch)
 *   - DPV param form (validation, start/abort)
 *   - Reference CSV import (Option-B overlay)
 *   - x-axis toggle (commanded V vs measured RE)
 *   - Peak detection + display
 *   - CSV export
 *   - Toast notifications
 */

import uPlot from 'uplot';
import 'uplot/dist/uPlot.min.css';
import {
  Core, STATE,
  validateDpvParams, DPV_DEFAULTS,
  findPeaks, exportCsv, downloadCsv, importCsv,
} from './potentiostat-core.js';

// =============================================================================
// Config
// =============================================================================

const WS_URL    = `${location.protocol === 'https:' ? 'wss' : 'ws'}://${location.host}/ws`;
const WS_RETRY  = 3000;   // ms between reconnection attempts

// Series colors — electrode 1 is accent-aligned; ref is amber (high contrast dashed)
const COLORS = {
  1: '#38bdf8',   // sky blue — matches --accent
  2: '#f9826c',   // salmon-orange
  3: '#a78bfa',   // violet (distinct from accent + electrode 2)
  0: '#fbbf24',   // amber — reference overlay, reads well dashed on dark
};

// =============================================================================
// State
// =============================================================================

let ws        = null;
let wsRetryId = null;
let chart     = null;         // uPlot instance
let xAxisMode = 'cmd';        // 'cmd' = E_mV, 're' = RE_mV
let pendingPts = [];          // buffer for rAF batch flush
let rafPending = false;

// Scan data per electrode: {1: {xs, ys}, 2: {xs, ys}, 3: {xs, ys}}
let scanSeries = {};
let refSeries  = { xs: [], ys: [] };   // Option-B reference
let hasRef     = false;

const core = new Core({
  onPoint:  handlePoint,
  onEvent:  handleEvent,
  onState:  updateState,
  onHello:  handleHello,
  onResync: handleResync,
  onError:  showError,
});

// =============================================================================
// DOM references
// =============================================================================

const $dot        = document.getElementById('dot');
const $wsLabel    = document.getElementById('ws-label');
const $ptsCount   = document.getElementById('pts-count');
const $ptsVal     = document.getElementById('pts-val');
const $btnStart   = document.getElementById('btn-start');
const $btnAbort   = document.getElementById('btn-abort');
const $btnCsv     = document.getElementById('btn-csv');
const $liveE      = document.getElementById('live-e');
const $liveI      = document.getElementById('live-i');
const $liveElec   = document.getElementById('live-elec');
const $peaksList  = document.getElementById('peaks-list');
const $peaksEmpty = document.getElementById('peaks-empty');
const $toast      = document.getElementById('toast');
const $chartWrap  = document.getElementById('chart-container');
const $fwInfo     = document.getElementById('fw-info');
const $devUrl     = document.getElementById('device-url');
const $refInfo    = document.getElementById('ref-info');
const $refLegend  = document.getElementById('ref-legend');
const $refFile    = document.getElementById('ref-file');
const $btnClearRef= document.getElementById('btn-clear-ref');
const $concSection= document.getElementById('conc-section');
const $concList   = document.getElementById('conc-list');

document.getElementById('device-url').textContent = location.host;

// =============================================================================
// uPlot chart initialisation
// =============================================================================

function buildChartData() {
  // uPlot columnar format: [xs, ys_e1, ys_e2, ys_e3, ys_ref]
  // All arrays must be same length; missing values = null.
  const allX = [...new Set([
    ...Object.values(scanSeries).flatMap(s => s.xs),
    ...(hasRef ? refSeries.xs : []),
  ])].sort((a, b) => a - b);

  if (allX.length === 0) return [[0], [null], [null], [null], [null]];

  function mapY(xs_src, ys_src) {
    if (!xs_src.length) return allX.map(() => null);
    const m = new Map(xs_src.map((x, i) => [x, ys_src[i]]));
    return allX.map(x => m.has(x) ? m.get(x) : null);
  }

  return [
    allX,
    mapY(scanSeries[1]?.xs || [], scanSeries[1]?.ys || []),
    mapY(scanSeries[2]?.xs || [], scanSeries[2]?.ys || []),
    mapY(scanSeries[3]?.xs || [], scanSeries[3]?.ys || []),
    hasRef ? mapY(refSeries.xs, refSeries.ys) : allX.map(() => null),
  ];
}

function createChart() {
  if (chart) { chart.destroy(); chart = null; }

  const W = $chartWrap.clientWidth  || 600;
  const H = $chartWrap.clientHeight || 300;

  const opts = {
    width:  W,
    height: H,
    padding: [8, 8, 0, 0],

    scales: {
      x: { time: false, auto: true },
      y: { auto: true },
    },

    axes: [
      {
        label:  'E (mV)',
        stroke: '#7491a8',
        grid:   { stroke: '#162033', width: 1 },
        ticks:  { stroke: '#162033' },
        font:   '11px IBM Plex Mono, monospace',
        values: (_, vals) => vals.map(v => v == null ? '' : v.toFixed(0)),
      },
      {
        label:   'ΔI (µA)',
        stroke:  '#7491a8',
        grid:    { stroke: '#162033', width: 1 },
        ticks:   { stroke: '#162033' },
        font:    '11px IBM Plex Mono, monospace',
        values:  (_, vals) => vals.map(v => v == null ? '' : v.toFixed(1)),
        size:    50,
      },
    ],

    series: [
      {},   // x
      { label: 'Electrode 1', stroke: COLORS[1], width: 2, spanGaps: true },
      { label: 'Electrode 2', stroke: COLORS[2], width: 2, spanGaps: true },
      { label: 'Electrode 3', stroke: COLORS[3], width: 2, spanGaps: true },
      { label: 'Reference',   stroke: COLORS[0], width: 1.5, dash: [4, 4], spanGaps: true },
    ],

    legend: { show: true, live: false },
    cursor: { show: true, drag: { x: true, y: true } },
  };

  chart = new uPlot(opts, buildChartData(), $chartWrap);
}

// Resize chart when window resizes
const resizeObs = new ResizeObserver(() => {
  if (!chart) return;
  const W = $chartWrap.clientWidth;
  const H = $chartWrap.clientHeight || 300;
  if (W > 0 && H > 0) chart.setSize({ width: W, height: H });
});
resizeObs.observe($chartWrap.parentElement);

// =============================================================================
// rAF batch flush (anti-stutter: never redraw more than once per frame)
// =============================================================================

function scheduleFlush() {
  if (rafPending) return;
  rafPending = true;
  requestAnimationFrame(flushPendingPoints);
}

function flushPendingPoints() {
  rafPending = false;
  if (!pendingPts.length) return;

  for (const pt of pendingPts) {
    const xVal = xAxisMode === 're' ? pt.RE_mV : pt.E_mV;
    if (!scanSeries[pt.electrode]) {
      scanSeries[pt.electrode] = { xs: [], ys: [] };
    }
    scanSeries[pt.electrode].xs.push(xVal);
    scanSeries[pt.electrode].ys.push(pt.I_uA);
  }

  // Live readout: last point
  const last = pendingPts[pendingPts.length - 1];
  $liveE.textContent    = (xAxisMode === 're' ? last.RE_mV : last.E_mV).toFixed(1);
  $liveI.textContent    = last.I_uA.toFixed(2);
  $liveElec.textContent = last.electrode;

  pendingPts = [];

  // Update pts count
  const total = Object.values(scanSeries).reduce((s, ss) => s + ss.xs.length, 0);
  $ptsVal.textContent = total;

  // Update chart data
  if (chart) chart.setData(buildChartData(), false /* no scale reset during live */);
}

// =============================================================================
// Core callbacks
// =============================================================================

function handlePoint(pt) {
  pendingPts.push(pt);
  scheduleFlush();
}

function handleEvent(msg) {
  // handled via updateState
}

function updateState(state, msg) {
  // Ambient visual state — CSS uses body[data-state='...'] for chart glow
  document.body.dataset.state = state.toLowerCase();

  switch (state) {
    case STATE.IDLE:
    case STATE.COMPLETE:
      $dot.className    = 'status-dot connected';
      $wsLabel.textContent = state === STATE.COMPLETE ? 'Scan complete' : 'Ready';
      $btnStart.disabled = false;
      $btnAbort.disabled = true;
      $btnCsv.disabled   = core.points.length === 0;
      if (state === STATE.COMPLETE) {
        runPeakDetection();
        showToast('Scan complete', 'info');
      }
      break;

    case STATE.EQUILIBRATING:
      $dot.className    = 'status-dot running';
      $wsLabel.textContent = 'Equilibrating…';
      break;

    case STATE.RUNNING:
      $dot.className    = 'status-dot running';
      $wsLabel.textContent = 'Running scan…';
      $btnStart.disabled = true;
      $btnAbort.disabled = false;
      $ptsCount.style.display = '';
      clearChart();
      break;

    case STATE.ABORTING:
      $wsLabel.textContent = 'Aborting…';
      break;

    case STATE.ERROR:
      $dot.className = 'status-dot error';
      $wsLabel.textContent = 'Scan error';
      $btnStart.disabled = false;
      $btnAbort.disabled = true;
      break;
  }
}

function handleHello(info) {
  $fwInfo.textContent = `fw ${info.fw || '?'} / proto ${info.proto || '?'}`;
}

// =============================================================================
// Scan time estimate
// =============================================================================

function updateScanEstimate() {
  const p = readParams();
  if (!p) return;
  const steps = Math.abs(p.e_end_mV - p.e_begin_mV) / p.e_step_mV;
  const ms    = Math.ceil(steps) * p.t_period_ms + p.t_equilibration_ms;
  const el    = document.getElementById('scan-estimate-val');
  if (!el) return;
  el.textContent = ms < 60000
    ? `~${(ms / 1000).toFixed(0)} s`
    : `~${(ms / 60000).toFixed(1)} min`;
}

function handleResync(pts, state) {
  // Rebuild scanSeries from replayed points
  clearChart(false /* don't destroy uPlot */);
  for (const pt of pts) {
    const xVal = xAxisMode === 're' ? pt.RE_mV : pt.E_mV;
    if (!scanSeries[pt.electrode]) scanSeries[pt.electrode] = { xs: [], ys: [] };
    scanSeries[pt.electrode].xs.push(xVal);
    scanSeries[pt.electrode].ys.push(pt.I_uA);
  }
  if (chart) chart.setData(buildChartData());
  $ptsVal.textContent = pts.length;
  if (pts.length > 0) {
    $ptsCount.style.display = '';
    $btnCsv.disabled = false;
  }
}

// =============================================================================
// WebSocket management
// =============================================================================

function connect() {
  if (ws) return;

  ws = new WebSocket(WS_URL);
  ws.binaryType = 'arraybuffer';

  ws.onopen = () => {
    $dot.className = 'status-dot connected';
    $wsLabel.textContent = 'Connected';
    $btnStart.disabled   = false;
    document.body.dataset.state = 'idle';
    clearTimeout(wsRetryId);
    wsRetryId = null;
  };

  ws.onclose = () => {
    ws = null;
    $dot.className       = 'status-dot';
    $wsLabel.textContent = 'Disconnected — retrying…';
    $btnStart.disabled   = true;
    $btnAbort.disabled   = true;
    wsRetryId = setTimeout(connect, WS_RETRY);
  };

  ws.onerror = () => { /* onclose handles retry */ };

  ws.onmessage = (ev) => {
    if (ev.data instanceof ArrayBuffer) core.feedBinary(ev.data);
    else                                core.feedText(ev.data);
  };
}

function sendCmd(obj) {
  if (ws && ws.readyState === WebSocket.OPEN) {
    ws.send(JSON.stringify(obj));
  }
}

// =============================================================================
// Chart helpers
// =============================================================================

function clearChart(destroyFirst = true) {
  pendingPts  = [];
  scanSeries  = {};
  $liveE.textContent    = '—';
  $liveI.textContent    = '—';
  $liveElec.textContent = '—';
  $ptsVal.textContent   = '0';
  $peaksEmpty.textContent = 'No peaks detected — run a DPV scan.';
  while ($peaksList.children.length > 1) $peaksList.removeChild($peaksList.lastChild);
  $concSection.style.display = 'none';

  if (destroyFirst) {
    createChart();  // re-create with empty data
  } else if (chart) {
    chart.setData(buildChartData());
  }
}

// =============================================================================
// Peak detection
// =============================================================================

function runPeakDetection() {
  const peaks = core.findPeaks({ minProminence: 3, minSepMv: 50 });

  // Remove old peak items (keep first child = empty message)
  while ($peaksList.children.length > 1) $peaksList.removeChild($peaksList.lastChild);

  if (!peaks.length) {
    $peaksEmpty.textContent = 'No peaks detected.';
    return;
  }

  $peaksEmpty.textContent = '';

  for (const pk of peaks) {
    const item = document.createElement('div');
    item.className = 'peak-item';
    item.setAttribute('data-testid', 'peak-item');
    item.innerHTML = `
      <span class="peak-E">E = ${pk.E_mV.toFixed(1)} mV</span>
      <span class="peak-I">ΔI = ${pk.I_uA.toFixed(2)} µA</span>
    `;
    $peaksList.appendChild(item);
  }
}

// =============================================================================
// DPV form handling
// =============================================================================

function readParams() {
  return {
    e_begin_mV:         parseFloat(document.getElementById('e-begin').value),
    e_end_mV:           parseFloat(document.getElementById('e-end').value),
    e_step_mV:          parseFloat(document.getElementById('e-step').value),
    e_pulse_mV:         parseFloat(document.getElementById('e-pulse').value),
    t_pulse_ms:         parseInt(document.getElementById('t-pulse').value, 10),
    t_period_ms:        parseInt(document.getElementById('t-period').value, 10),
    t_equilibration_ms: parseInt(document.getElementById('t-equil').value, 10),
    cycles:             1,
    n_avg:              5,
    electrode:          parseInt(document.getElementById('electrode').value, 10),
  };
}

function validateForm() {
  const params = readParams();
  const err    = validateDpvParams(params);
  // Basic visual feedback
  const fields = {
    'f-e-begin': (p) => p.e_begin_mV < -1000 || p.e_begin_mV > 1000,
    'f-e-end':   (p) => p.e_end_mV   < -1000 || p.e_end_mV   > 1000,
    'f-e-step':  (p) => p.e_step_mV  <= 0,
    'f-e-pulse': (p) => p.e_pulse_mV <= 0,
    'f-t-period':(p) => p.t_period_ms <= p.t_pulse_ms,
  };
  let anyInvalid = false;
  for (const [id, test] of Object.entries(fields)) {
    const el = document.getElementById(id);
    if (test(params)) { el.classList.add('invalid'); anyInvalid = true; }
    else              { el.classList.remove('invalid'); }
  }
  // If t_period is invalid it's hidden inside <details> — force-open so the red field is visible
  if (document.getElementById('f-t-period')?.classList.contains('invalid')) {
    document.querySelector('details')?.setAttribute('open', '');
  }
  return anyInvalid ? null : params;
}

$btnStart.addEventListener('click', () => {
  const params = validateForm();
  if (!params) { showToast('Please fix the parameter errors.', 'error'); return; }

  sendCmd({
    cmd:       'start',
    technique: 'DPV',
    electrode: params.electrode,
    params: {
      e_begin_mV:          params.e_begin_mV,
      e_end_mV:            params.e_end_mV,
      e_step_mV:           params.e_step_mV,
      e_pulse_mV:          params.e_pulse_mV,
      t_pulse_ms:          params.t_pulse_ms,
      t_period_ms:         params.t_period_ms,
      t_equilibration_ms:  params.t_equilibration_ms,
    },
  });
});

$btnAbort.addEventListener('click', () => {
  sendCmd({ cmd: 'abort' });
});

// =============================================================================
// CSV export
// =============================================================================

$btnCsv.addEventListener('click', () => {
  if (!core.points.length) return;
  downloadCsv(core.exportCsv(), 'aquahmet_scan.csv');
});

// =============================================================================
// Reference CSV import (Option-B)
// =============================================================================

$refFile.addEventListener('change', (ev) => {
  const file = ev.target.files[0];
  if (!file) return;
  const reader = new FileReader();
  reader.onload = (e) => {
    const pts = importCsv(e.target.result);
    core.loadReference(e.target.result);
    refSeries.xs = pts.map(p => xAxisMode === 're' ? p.RE_mV : p.E_mV);
    refSeries.ys = pts.map(p => p.I_uA);
    hasRef = pts.length > 0;
    $refInfo.textContent = hasRef ? `${pts.length} reference points loaded.` : 'No points found.';
    $refLegend.style.display = hasRef ? '' : 'none';
    $btnClearRef.disabled = !hasRef;
    if (chart) chart.setData(buildChartData());
    // Also upload to device
    const formData = new FormData();
    formData.append('csv', file);
    fetch('/api/reference.csv', { method: 'POST', body: file }).catch(() => {});
  };
  reader.readAsText(file);
  ev.target.value = '';  // allow re-upload of same file
});

$btnClearRef.addEventListener('click', () => {
  hasRef = false;
  refSeries = { xs: [], ys: [] };
  $refInfo.textContent = '';
  $refLegend.style.display = 'none';
  $btnClearRef.disabled = true;
  if (chart) chart.setData(buildChartData());
});

// =============================================================================
// x-axis toggle
// =============================================================================

document.getElementById('xaxis-cmd').addEventListener('click', () => setXAxis('cmd'));
document.getElementById('xaxis-re').addEventListener('click',  () => setXAxis('re'));

// =============================================================================
// Remote TFT navigation (POST /api/nav) — drives the on-device screen
// =============================================================================
document.querySelectorAll('#nav-btn-row [data-nav]').forEach((b) => {
  b.addEventListener('click', () => {
    const screen = b.getAttribute('data-nav');
    fetch('/api/nav', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ screen }),
    })
      .then((r) => (r.ok
        ? showToast(`Display \u2192 ${screen}`, 'info')
        : showToast(`Nav failed (${r.status})`, 'error')))
      .catch(() => showToast('Nav request failed', 'error'));
  });
});

function setXAxis(mode) {
  xAxisMode = mode;
  document.getElementById('xaxis-cmd').classList.toggle('active', mode === 'cmd');
  document.getElementById('xaxis-re').classList.toggle('active',  mode === 're');

  // Rebuild scan series with new axis values
  const pts = core.points;
  scanSeries = {};
  for (const pt of pts) {
    const xVal = mode === 're' ? pt.RE_mV : pt.E_mV;
    if (!scanSeries[pt.electrode]) scanSeries[pt.electrode] = { xs: [], ys: [] };
    scanSeries[pt.electrode].xs.push(xVal);
    scanSeries[pt.electrode].ys.push(pt.I_uA);
  }
  if (hasRef) {
    refSeries.xs = core.refPoints.map(p => mode === 're' ? p.RE_mV : p.E_mV);
  }
  if (chart) chart.setData(buildChartData());
}

// =============================================================================
// Toast
// =============================================================================

let toastTimer = null;

function showToast(msg, type = 'error') {
  $toast.textContent = msg;
  $toast.className   = type === 'info' ? 'visible info' : 'visible';
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { $toast.className = ''; }, 3500);
}

function showError(msg) {
  showToast(msg, 'error');
}

// =============================================================================
// Boot
// =============================================================================

// Initial body state
document.body.dataset.state = 'idle';

// Hook scan estimate to form changes
const $dpvForm = document.getElementById('dpv-form');
if ($dpvForm) {
  $dpvForm.addEventListener('input', updateScanEstimate);
  updateScanEstimate();
}

createChart();
connect();
