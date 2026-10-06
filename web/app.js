// Copyright (c) 2026 Jacek Fedorynski
// SPDX-License-Identifier: MIT

'use strict';

const state = {
  port: null,
  reader: null,
  keepReading: false,
  lineBuffer: '',
  disconnectRequested: false,
  testRunning: false,
  chartView: 0, // which chart is shown, cycled by clicking it
  testFailed: false,
  testSucceeded: false,
  deviceConnected: false, // meaningless while port is null
  device: null, // {vid, pid, manufacturer, product} of the connected DUT
  buildId: '',

  // Test runs, newest first, and the one whose data is shown. Runs that
  // never started a test are dropped when a new run starts.
  runs: [],
  run: null,

  totalSamples: 0,
  expectedSamples: 0,
  validSamples: 0,
  droppedSamples: 0,

  sumLatency: 0,
  // Number of samples by the frame the response arrived in, counted from
  // the frame of the toggle.
  frameCounts: new Map(),
  sumSofToIn: 0,
  sumSofToInSq: 0,
  sumRespDur: 0,
  sumRespDurSq: 0,

  sofGapSamples: 0,
  sumSofGap: 0,
  sumSofGapSq: 0,

  points: [], // {sofToToggle, nextSofToResponseEnd, toggleToResponseEnd}, in ns
};

let chartDirty = false;

const THRESHOLD_Y_NS = 1000 * 1000;

const FRAME_NS = 1000 * 1000;

// Fixed so that rare offsets slightly past one frame don't stretch the axis.
const X_AXIS_MAX_NS = 1000 * 1000;

const el = {
  dotSerial: document.getElementById('dotSerial'),
  dotDut: document.getElementById('dotDut'),
  dotTest: document.getElementById('dotTest'),
  statusMessage: document.getElementById('statusMessage'),
  connectBtn: document.getElementById('connectBtn'),
  startBtn: document.getElementById('startBtn'),
  statManufacturer: document.getElementById('statManufacturer'),
  statProduct: document.getElementById('statProduct'),
  statVidPid: document.getElementById('statVidPid'),
  statValid: document.getElementById('statValid'),
  statAvgLatency: document.getElementById('statAvgLatency'),
  statFrames: document.getElementById('statFrames'),
  statSofGap: document.getElementById('statSofGap'),
  statSofToIn: document.getElementById('statSofToIn'),
  statRespDur: document.getElementById('statRespDur'),
  statBuildId: document.getElementById('statBuildId'),
  runList: document.getElementById('runList'),
  exportBtn: document.getElementById('exportBtn'),
  importBtn: document.getElementById('importBtn'),
  importFile: document.getElementById('importFile'),
  progressBar: document.getElementById('progressBar'),
  progressFill: document.getElementById('progressFill'),
  log: document.getElementById('log'),
  chart: document.getElementById('chart'),
};

const ctx = el.chart.getContext('2d');

function fmtUs(ns) {
  return (ns / 1000).toFixed(1) + ' µs';
}

function fmtNs(ns) {
  return ns.toFixed(1) + ' ns';
}

function fmtMeanStddev(sum, sumSq, n, fmt) {
  if (n === 0) {
    return '–';
  }
  const mean = sum / n;
  const variance = n > 1 ? Math.max(0, sumSq / n - mean * mean) * (n / (n - 1)) : 0;
  const stddev = Math.sqrt(variance);
  return fmt(mean) + ' ± ' + fmt(stddev);
}

function logLine(text) {
  const atBottom = el.log.scrollTop + el.log.clientHeight >= el.log.scrollHeight - 4;
  const line = document.createTextNode(text + '\n');
  el.log.appendChild(line);
  while (el.log.childNodes.length > 1000) {
    el.log.removeChild(el.log.firstChild);
  }
  if (atBottom) {
    el.log.scrollTop = el.log.scrollHeight;
  }
}

function setStatusMessage(text) {
  el.statusMessage.textContent = text;
}

function clearStatusMessage() {
  el.statusMessage.textContent = '';
}

function markProtocolError(line) {
  logLine('(protocol error) ' + line);
  state.testFailed = true;
  setStatusMessage('Protocol error');
  updateDots();
}

function hex4(n) {
  return n.toString(16).padStart(4, '0');
}

function fmtVidPid(vid, pid) {
  return hex4(vid) + ':' + hex4(pid);
}

function updateDots() {
  el.dotSerial.className = 'status-dot ' + (state.port ? 'status-dot-green' : 'status-dot-red');
  el.dotSerial.title = state.port ? 'Serial: connected' : 'Serial: not connected';

  if (!state.port) {
    el.dotDut.className = 'status-dot status-dot-grey';
    el.dotDut.title = 'DUT: unknown (not connected to tester)';
  } else if (state.deviceConnected) {
    el.dotDut.className = 'status-dot status-dot-green';
    el.dotDut.title = 'DUT: ' + fmtVidPid(state.device.vid, state.device.pid);
  } else {
    el.dotDut.className = 'status-dot status-dot-red';
    el.dotDut.title = 'DUT: not connected';
  }

  if (state.testRunning) {
    el.dotTest.className = 'status-dot status-dot-blink';
    el.dotTest.title = 'Test: running';
  } else if (state.testFailed) {
    el.dotTest.className = 'status-dot status-dot-red';
    el.dotTest.title = 'Test: failed';
  } else if (state.testSucceeded) {
    el.dotTest.className = 'status-dot status-dot-green';
    el.dotTest.title = 'Test: finished successfully';
  } else {
    el.dotTest.className = 'status-dot status-dot-grey';
    el.dotTest.title = 'Test: not running';
  }

  el.startBtn.disabled = !state.testRunning && (!state.port || !state.deviceConnected);
  renderRuns();
}

function updateStatsDisplay() {
  el.statValid.textContent = String(state.validSamples);
  el.statAvgLatency.textContent = state.validSamples > 0 ? fmtUs(state.sumLatency / state.validSamples) : '–';
  el.statFrames.textContent = [...state.frameCounts]
    .sort(([a], [b]) => a - b)
    .map(([k, count]) => ((count / state.validSamples) * 100).toFixed(1) + '% F' + k)
    .join(', ');
  el.statSofGap.textContent = fmtMeanStddev(state.sumSofGap, state.sumSofGapSq, state.sofGapSamples, fmtNs);
  el.statSofToIn.textContent = fmtMeanStddev(state.sumSofToIn, state.sumSofToInSq, state.validSamples, fmtNs);
  el.statRespDur.textContent = fmtMeanStddev(state.sumRespDur, state.sumRespDurSq, state.validSamples, fmtNs);
}

function updateProgress() {
  el.progressBar.classList.toggle('active', state.testRunning);
  const pct = state.expectedSamples > 0 ? Math.min(100, (state.totalSamples / state.expectedSamples) * 100) : 0;
  el.progressFill.style.width = pct + '%';
}

function makeRun(device) {
  return {
    device, // {vid, pid, manufacturer, product}, or null
    buildId: '',
    samples: [], // raw sample lines from the tester, as arrays of numbers
    started: false,
    finished: false,
    succeeded: false,
    finishedAt: null, // unix timestamp, in seconds
    failureReason: '',
  };
}

// Clears the data shown and shows the current run's device info.
function resetDisplay() {
  state.totalSamples = 0;
  state.expectedSamples = 0;
  state.validSamples = 0;
  state.droppedSamples = 0;
  state.sumLatency = 0;
  state.frameCounts = new Map();
  state.sumSofToIn = 0;
  state.sumSofToInSq = 0;
  state.sumRespDur = 0;
  state.sumRespDurSq = 0;
  state.sofGapSamples = 0;
  state.sumSofGap = 0;
  state.sumSofGapSq = 0;
  state.points = [];
  state.testFailed = false;
  state.testSucceeded = false;

  const device = state.run.device;
  el.statVidPid.textContent = device ? fmtVidPid(device.vid, device.pid) : '–';
  el.statManufacturer.textContent = (device && device.manufacturer) || '–';
  el.statProduct.textContent = (device && device.product) || '–';
  el.statBuildId.textContent = (state.run.started ? state.run.buildId : state.buildId) || '–';

  updateStatsDisplay();
  updateDots();
  updateProgress();
  chartDirty = true;
}

// Starts a new run and shows it. Only runs with a device are listed.
function newRun(device) {
  state.runs = state.runs.filter((run) => run.started);
  state.run = makeRun(device);
  if (device) {
    state.runs.unshift(state.run);
  }
  resetDisplay();
}

function finishRun() {
  const run = state.run;
  run.finished = true;
  run.succeeded = state.testSucceeded && !state.testFailed;
  run.finishedAt = Math.floor(Date.now() / 1000);
  run.failureReason = run.succeeded ? '' : el.statusMessage.textContent;
}

// Shows a past run as if it just happened.
function loadRun(run) {
  if (state.testRunning) {
    return;
  }
  state.run = run;
  resetDisplay();
  for (const f of run.samples) {
    accumulateSample(f);
  }
  state.testFailed = run.finished && !run.succeeded;
  state.testSucceeded = run.finished && run.succeeded;
  setStatusMessage(run.failureReason);
  updateStatsDisplay();
  updateDots();
}

function accumulateSample(f) {
  const [, sofToToggle, nextSofToResponseEnd, toggleToResponseStart, sofToIn, responseDuration, sofGap] = f;

  const toggleToResponseEnd = toggleToResponseStart + responseDuration;
  state.points.push({ sofToToggle, nextSofToResponseEnd, toggleToResponseEnd });
  chartDirty = true;

  state.sofGapSamples++;
  state.sumSofGap += sofGap;
  state.sumSofGapSq += sofGap * sofGap;

  state.validSamples++;
  state.sumLatency += toggleToResponseEnd;
  const frame = Math.floor((sofToToggle + toggleToResponseEnd) / FRAME_NS);
  state.frameCounts.set(frame, (state.frameCounts.get(frame) || 0) + 1);
  state.sumSofToIn += sofToIn;
  state.sumSofToInSq += sofToIn * sofToIn;
  state.sumRespDur += responseDuration;
  state.sumRespDurSq += responseDuration * responseDuration;
}

function handleSample(f) {
  state.totalSamples++;
  updateProgress();
  state.run.samples.push(f);
  accumulateSample(f);
  updateStatsDisplay();
}

function setTestRunning(running) {
  state.testRunning = running;
  if (!running && state.run.started && !state.run.finished) {
    finishRun();
  }
  el.startBtn.textContent = running ? 'Stop test' : 'Start test';
  updateDots();
  updateProgress();
}

function setDutConnected(vid, pid, manufacturer, product) {
  state.deviceConnected = true;
  state.device = { vid, pid, manufacturer: manufacturer || '', product: product || '' };
  newRun(state.device);
}

function setDutDisconnected() {
  state.deviceConnected = false;
  state.device = null;
  state.runs = state.runs.filter((run) => run.started);
  // Data from a test stays shown, along with its device.
  if (!state.run.started) {
    newRun(null);
  }
  updateDots();
}

function runLabel(run) {
  const d = run.device;
  if (!d) {
    return '–';
  }
  return [fmtVidPid(d.vid, d.pid), d.manufacturer, d.product].filter((x) => x).join(' ');
}

function renderRuns() {
  el.runList.replaceChildren();
  el.runList.classList.toggle('disabled', state.testRunning);
  for (const run of state.runs) {
    const item = document.createElement('button');
    item.className = 'run-item' + (run === state.run ? ' selected' : '');
    item.disabled = state.testRunning;

    const dot = document.createElement('span');
    dot.className =
      'status-dot ' + (!run.finished ? 'status-dot-grey' : run.succeeded ? 'status-dot-green' : 'status-dot-red');
    const label = document.createElement('span');
    label.className = 'run-label';
    label.textContent = runLabel(run);
    const time = document.createElement('span');
    time.className = 'run-time';
    time.textContent = run.finished
      ? new Date(run.finishedAt * 1000).toLocaleString()
      : run.started
        ? 'running'
        : 'not started';
    item.append(dot, label, time);
    item.title = label.textContent + '\n' + time.textContent;
    item.addEventListener('click', () => loadRun(run));
    el.runList.append(item);
  }
  el.exportBtn.disabled = state.testRunning || !state.run.finished;
  el.importBtn.disabled = state.testRunning;
}

function exportRun() {
  const run = state.run;
  if (state.testRunning || !run.finished) {
    return;
  }
  const d = run.device || { vid: 0, pid: 0, manufacturer: '', product: '' };
  const data = {
    format: 'latency-tester-run',
    version: 1,
    vid: d.vid,
    pid: d.pid,
    manufacturer: d.manufacturer,
    product: d.product,
    build_id: run.buildId,
    finished_at: run.finishedAt,
    succeeded: run.succeeded,
    failure_reason: run.failureReason,
    samples: run.samples,
  };
  const blob = new Blob([JSON.stringify(data)], { type: 'application/json' });
  const a = document.createElement('a');
  a.href = URL.createObjectURL(blob);
  a.download = 'latency-' + hex4(d.vid) + '-' + hex4(d.pid) + '-' + run.finishedAt + '.json';
  a.click();
  setTimeout(() => URL.revokeObjectURL(a.href), 1000);
}

// Returns the run, or throws with a message saying what's wrong.
function parseRun(text) {
  const data = JSON.parse(text);
  const isInt = (x) => Number.isInteger(x);
  const optString = (x) => x === undefined || typeof x === 'string';
  if (typeof data !== 'object' || data === null || data.format !== 'latency-tester-run') {
    throw new Error('not a latency tester data file');
  }
  if (data.version !== 1) {
    throw new Error('unsupported version ' + data.version);
  }
  if (
    !isInt(data.vid) ||
    !isInt(data.pid) ||
    !isInt(data.finished_at) ||
    typeof data.succeeded !== 'boolean' ||
    !optString(data.manufacturer) ||
    !optString(data.product) ||
    !optString(data.build_id) ||
    !optString(data.failure_reason) ||
    !Array.isArray(data.samples) ||
    !data.samples.every((f) => Array.isArray(f) && f.length === 7 && f.every(isInt))
  ) {
    throw new Error('invalid data');
  }
  return {
    device: { vid: data.vid, pid: data.pid, manufacturer: data.manufacturer || '', product: data.product || '' },
    buildId: data.build_id || '',
    samples: data.samples,
    started: true,
    finished: true,
    succeeded: data.succeeded,
    finishedAt: data.finished_at,
    failureReason: data.failure_reason || '',
  };
}

async function importRun(file) {
  let run;
  try {
    run = parseRun(await file.text());
  } catch (e) {
    logLine('(import failed) ' + file.name + ': ' + e.message);
    setStatusMessage('Import failed: ' + e.message);
    return;
  }
  if (state.testRunning) {
    return;
  }
  logLine('(imported) ' + file.name);
  state.runs.unshift(run);
  loadRun(run);
}

function handleComment(line) {
  logLine(line);
}

// #["type", {...}]
function handleDeviceMessage(line) {
  logLine(line);

  let msg;
  try {
    msg = JSON.parse(line.slice(1));
  } catch (e) {
    markProtocolError(line);
    return;
  }
  if (
    !Array.isArray(msg) ||
    msg.length !== 2 ||
    typeof msg[0] !== 'string' ||
    typeof msg[1] !== 'object' ||
    msg[1] === null
  ) {
    markProtocolError(line);
    return;
  }
  const [type, data] = msg;

  switch (type) {
    case 'starting_test':
      if (typeof data.total_samples !== 'number') {
        markProtocolError(line);
        return;
      }
      // A test started while showing a past run gets a run of its own.
      if (state.run.started) {
        newRun(state.device);
      }
      if (!state.runs.includes(state.run)) {
        state.runs.unshift(state.run);
      }
      state.run.started = true;
      state.run.buildId = state.buildId;
      state.testFailed = false;
      state.testSucceeded = false;
      state.expectedSamples = data.total_samples;
      clearStatusMessage();
      setTestRunning(true);
      break;
    case 'test_finished':
      if (typeof data.average_latency_ns !== 'number') {
        markProtocolError(line);
        return;
      }
      if (!state.testFailed) {
        state.testSucceeded = true;
      }
      setTestRunning(false);
      break;
    case 'test_aborted':
      if (typeof data.reason !== 'string') {
        markProtocolError(line);
        return;
      }
      if (data.reason === 'input dropped') {
        state.droppedSamples++;
        updateStatsDisplay();
      }
      state.testFailed = true;
      // Before the run is finished, so that it keeps the reason.
      setStatusMessage('Test aborted: ' + data.reason);
      setTestRunning(false);
      break;
    case 'test_not_running':
      setTestRunning(false);
      break;
    case 'device_connected':
      if (typeof data.vid !== 'number' || typeof data.pid !== 'number') {
        markProtocolError(line);
        return;
      }
      setDutConnected(data.vid, data.pid, data.manufacturer, data.product);
      break;
    case 'device_disconnected':
      setDutDisconnected();
      break;
    case 'status':
      if (
        typeof data.connected !== 'boolean' ||
        typeof data.build_id !== 'string' ||
        (data.connected && (typeof data.vid !== 'number' || typeof data.pid !== 'number'))
      ) {
        markProtocolError(line);
        return;
      }
      state.buildId = data.build_id;
      if (!state.run.started) {
        el.statBuildId.textContent = data.build_id;
      }
      if (data.connected) {
        setDutConnected(data.vid, data.pid, data.manufacturer, data.product);
      } else {
        setDutDisconnected();
      }
      break;
    default:
      markProtocolError(line);
      break;
  }
}

function handleLine(line) {
  if (line.length === 0) {
    return;
  }
  if (line.startsWith('#[')) {
    handleDeviceMessage(line);
    return;
  }
  if (line.startsWith('#')) {
    handleComment(line);
    return;
  }

  const parts = line.trim().split(/\s+/);
  if (parts.length !== 7 || !parts.every((p) => /^-?\d+$/.test(p))) {
    logLine('(unrecognized) ' + line);
    if (state.testRunning) {
      state.testFailed = true;
      updateDots();
    }
    return;
  }
  if (!state.testRunning) {
    return;
  }
  handleSample(parts.map(Number));
}

async function readLoop() {
  const textDecoder = new TextDecoderStream();
  const readableStreamClosed = state.port.readable.pipeTo(textDecoder.writable);
  const reader = textDecoder.readable.getReader();
  state.reader = reader;

  try {
    while (state.keepReading) {
      const { value, done } = await reader.read();
      if (done) {
        break;
      }
      state.lineBuffer += value;
      let idx;
      while ((idx = state.lineBuffer.indexOf('\n')) >= 0) {
        const line = state.lineBuffer.slice(0, idx).replace(/\r$/, '');
        state.lineBuffer = state.lineBuffer.slice(idx + 1);
        handleLine(line);
      }
    }
  } catch (e) {
    logLine('(read error) ' + e.message);
  } finally {
    reader.releaseLock();
    await readableStreamClosed.catch(() => {});
    onDisconnected();
  }
}

async function onDisconnected() {
  const port = state.port;
  state.port = null;
  state.reader = null;
  if (port) {
    try {
      await port.close();
    } catch (e) {
      // Already gone.
    }
  }
  if (!state.disconnectRequested) {
    setStatusMessage('Lost connection to the tester');
    if (state.testRunning) {
      state.testFailed = true;
    }
  }
  state.disconnectRequested = false;
  // Device info and build_id stay, as they still describe the data shown.
  el.connectBtn.disabled = false;
  el.connectBtn.textContent = 'Connect';
  setTestRunning(false);
}

async function connect() {
  try {
    const port = await navigator.serial.requestPort();
    await port.open({ baudRate: 921600 });
    state.port = port;
    state.keepReading = true;
    state.lineBuffer = '';
    newRun(null);
    clearStatusMessage();

    el.connectBtn.textContent = 'Disconnect';
    updateDots();

    readLoop();

    // Abort any test left running from a previous connection, and get the
    // current status.
    await writeText('x');
    await writeText('s');
  } catch (e) {
    logLine('(connect failed) ' + e.message);
  }
}

async function disconnect() {
  state.disconnectRequested = true;
  state.keepReading = false;
  if (state.reader) {
    try {
      // readLoop() then closes the port.
      await state.reader.cancel();
    } catch (e) {
      // Already done.
    }
  }
}

async function onConnectButtonClick() {
  if (state.port) {
    await disconnect();
  } else {
    await connect();
  }
}

async function writeText(text) {
  if (!state.port || !state.port.writable) {
    return;
  }
  logLine('> ' + text);
  const writer = state.port.writable.getWriter();
  try {
    await writer.write(new TextEncoder().encode(text));
  } finally {
    writer.releaseLock();
  }
}

async function startTest() {
  if (!state.port || state.testRunning) {
    return;
  }
  newRun(state.device);
  clearStatusMessage();
  // Re-enabled as "Stop test" once the device sends starting_test.
  el.startBtn.disabled = true;
  await writeText('t');
}

async function onStartButtonClick() {
  if (state.testRunning) {
    await writeText('x');
  } else {
    await startTest();
  }
}

function resizeCanvas() {
  const dpr = window.devicePixelRatio || 1;
  const rect = el.chart.getBoundingClientRect();
  el.chart.width = Math.max(1, Math.round(rect.width * dpr));
  el.chart.height = Math.max(1, Math.round(rect.height * dpr));
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  chartDirty = true;
}

function niceMax(v) {
  if (v <= 0) {
    return 1;
  }
  const mag = Math.pow(10, Math.floor(Math.log10(v)));
  const norm = v / mag;
  let step;
  if (norm <= 1) step = 1;
  else if (norm <= 2) step = 2;
  else if (norm <= 5) step = 5;
  else step = 10;
  return step * mag;
}

function drawChart() {
  const cssW = el.chart.clientWidth;
  const cssH = el.chart.clientHeight;
  ctx.clearRect(0, 0, cssW, cssH);

  const styles = getComputedStyle(document.body);
  const colorText = styles.getPropertyValue('--text').trim() || '#1a1a1a';
  const colorMuted = styles.getPropertyValue('--muted').trim() || '#6b7280';
  const colorBorder = styles.getPropertyValue('--border').trim() || '#e2e2e6';
  const colorPoint2 = styles.getPropertyValue('--ok-weak').trim() || 'rgba(22,163,74,0.55)';
  const colorBand = styles.getPropertyValue('--bad-weak').trim() || 'rgba(220,38,38,0.15)';

  ctx.font = '12px ui-monospace, SFMono-Regular, Menlo, Consolas, monospace';

  if (state.points.length === 0) {
    ctx.fillStyle = colorMuted;
    ctx.textAlign = 'center';
    ctx.textBaseline = 'middle';
    ctx.fillText('no data', cssW / 2, cssH / 2);
    return;
  }

  const margin = { left: 64, right: 16, top: 16, bottom: 44 };
  const plotW = Math.max(1, cssW - margin.left - margin.right);
  const plotH = Math.max(1, cssH - margin.top - margin.bottom);

  const opts = { colorText, colorMuted, colorBorder, colorBand, styles, cssW, cssH };
  const views = [null, drawArcChart, drawHistogramChart];
  if (views[state.chartView]) {
    views[state.chartView](opts);
    return;
  }

  const xMax = X_AXIS_MAX_NS;
  let yMax = 0;
  let yMin = 0;
  let xObsMin = Infinity;
  let xObsMax = -Infinity;
  let minBadX = Infinity;
  let maxGoodX = -Infinity;
  for (const { sofToToggle: x, nextSofToResponseEnd: yNextSof, toggleToResponseEnd: yToggle } of state.points) {
    if (x > xMax) continue;
    if (yToggle > yMax) yMax = yToggle;
    if (yToggle < yMin) yMin = yToggle;
    if (x < xObsMin) xObsMin = x;
    if (x > xObsMax) xObsMax = x;
    // Whether the response ended within a frame of the SOF after the toggle.
    if (yNextSof >= THRESHOLD_Y_NS) {
      if (x < minBadX) minBadX = x;
    } else if (x > maxGoodX) {
      maxGoodX = x;
    }
  }
  yMax = niceMax(yMax);
  yMin = yMin < 0 ? -niceMax(-yMin) : 0;

  const xToPx = (x) => margin.left + (x / xMax) * plotW;
  const yToPx = (y) => margin.top + plotH - ((y - yMin) / (yMax - yMin)) * plotH;

  // Red band: the range of toggle offsets where responses are neither all
  // under nor all over the threshold.
  const leftThreshold = Number.isFinite(minBadX) ? minBadX : xObsMax;
  const rightThreshold = Number.isFinite(maxGoodX) ? maxGoodX : xObsMin;
  const bandLeftPx = xToPx(Math.min(leftThreshold, rightThreshold));
  const bandRightPx = xToPx(Math.max(leftThreshold, rightThreshold));
  ctx.fillStyle = colorBand;
  ctx.fillRect(bandLeftPx, margin.top, bandRightPx - bandLeftPx, plotH);

  const nTicksX = 8;
  const nTicksY = 8;
  ctx.strokeStyle = colorBorder;
  ctx.fillStyle = colorMuted;
  ctx.lineWidth = 1;

  ctx.textAlign = 'center';
  ctx.textBaseline = 'top';
  for (let i = 0; i <= nTicksX; i++) {
    const x = (xMax / nTicksX) * i;
    const px = xToPx(x);
    ctx.beginPath();
    ctx.moveTo(px, margin.top);
    ctx.lineTo(px, margin.top + plotH);
    ctx.stroke();
    ctx.fillText((x / 1000).toFixed(0), px, margin.top + plotH + 6);
  }

  ctx.textAlign = 'right';
  ctx.textBaseline = 'middle';
  for (let i = 0; i <= nTicksY; i++) {
    const y = yMin + ((yMax - yMin) / nTicksY) * i;
    const py = yToPx(y);
    ctx.beginPath();
    ctx.moveTo(margin.left, py);
    ctx.lineTo(margin.left + plotW, py);
    ctx.stroke();
    ctx.fillText((y / 1000).toFixed(0), margin.left - 8, py);
  }

  ctx.fillStyle = colorText;
  ctx.textAlign = 'center';
  ctx.textBaseline = 'alphabetic';
  ctx.fillText('SOF → button toggle (µs)', margin.left + plotW / 2, cssH - 6);

  ctx.save();
  ctx.translate(14, margin.top + plotH / 2);
  ctx.rotate(-Math.PI / 2);
  ctx.fillText('button toggle → response end (µs)', 0, 0);
  ctx.restore();

  ctx.fillStyle = colorPoint2;
  for (const { sofToToggle: x, toggleToResponseEnd: y } of state.points) {
    if (x > xMax) continue;
    ctx.beginPath();
    ctx.arc(xToPx(x), yToPx(y), 2.2, 0, Math.PI * 2);
    ctx.fill();
  }
}


const CHART_VIEW_COUNT = 3;

// Samples grouped by the frame their response arrived in, counting frames
// from the SOF of the frame the button was toggled in.
function groupBySlot(styles) {
  // Fixed per frame, so that a frame has the same color whichever frames
  // the responses arrive in. Frames past F3 reuse the F1 to F3 colors.
  const slotColors = [
    styles.getPropertyValue('--frame0').trim() || '#9333ea',
    styles.getPropertyValue('--accent').trim() || '#2563eb',
    styles.getPropertyValue('--warn').trim() || '#ca8a04',
    styles.getPropertyValue('--ok').trim() || '#16a34a',
  ];
  const samples = [];
  const slots = new Set();
  let hObsMax = 0;
  let spanMax = 0;
  let minBadV = Infinity;
  let maxGoodV = -Infinity;
  for (const { sofToToggle: v, nextSofToResponseEnd: yNextSof, toggleToResponseEnd: span } of state.points) {
    if (v > FRAME_NS) continue;
    const h = v + span;
    const k = Math.floor(h / FRAME_NS);
    samples.push({ v, span, h, k });
    slots.add(k);
    if (h > hObsMax) hObsMax = h;
    if (span > spanMax) spanMax = span;
    // Same as the red band on the green chart.
    if (yNextSof >= THRESHOLD_Y_NS) {
      if (v < minBadV) minBadV = v;
    } else if (v > maxGoodV) {
      maxGoodV = v;
    }
  }
  const slotColor = (k) => slotColors[k <= 0 ? 0 : 1 + ((k - 1) % 3)];
  return { samples, slots, hObsMax, spanMax, minBadV, maxGoodV, slotColor };
}

// Opacity of the line for a sample, given how many samples came after it.
// The most recent ones stand out, fading to the same opacity as the rest,
// which is lower the more samples there are, so that they don't merge into
// a solid band.
function fadeAlpha(age, n) {
  const kFadeSamples = 40;
  const baseAlpha = Math.min(0.5, Math.max(0.05, 40 / Math.max(1, n)));
  return age < kFadeSamples ? 1 - (1 - baseAlpha) * (age / kFadeSamples) : baseAlpha;
}

// Ends at 1250, 2250, 3250, ... µs.
function hAxisMax(hObsMax) {
  return FRAME_NS * Math.max(1, Math.ceil((hObsMax - FRAME_NS / 4) / FRAME_NS)) + FRAME_NS / 4;
}

// One time axis, starting at the SOF of the frame the button was toggled in,
// with an arc from each toggle to the end of its response. Arcs for odd
// frames go above the axis and for even frames below it, so that neighboring
// frames don't overlap. An arc's height is proportional to its latency.
function drawArcChart({ colorText, colorMuted, colorBorder, colorBand, styles, cssW, cssH }) {
  const { samples, hObsMax, spanMax, minBadV, maxGoodV, slotColor } = groupBySlot(styles);

  const margin = { left: 36, right: 24, top: 24, bottom: 26 };
  const plotW = Math.max(1, cssW - margin.left - margin.right);
  const plotH = Math.max(1, cssH - margin.top - margin.bottom);
  const axisY = margin.top + plotH / 2;
  const hMax = hAxisMax(hObsMax);
  const hToPx = (h) => margin.left + (h / hMax) * plotW;
  // The same scale for all arcs, so that the longest one just fits.
  const halfSpanMaxPx = ((spanMax / hMax) * plotW) / 2;
  const arcScale = Math.min(1, (plotH / 2 - 4) / Math.max(1, halfSpanMaxPx));

  if (minBadV <= maxGoodV) {
    ctx.fillStyle = colorBand;
    ctx.fillRect(hToPx(minBadV), margin.top, hToPx(maxGoodV) - hToPx(minBadV), plotH);
  }

  ctx.lineWidth = 1;
  ctx.textAlign = 'center';
  ctx.textBaseline = 'top';
  for (let h = 0; h <= hMax; h += FRAME_NS / 4) {
    const px = hToPx(h);
    const isSof = h % FRAME_NS === 0;
    ctx.strokeStyle = isSof ? colorMuted : colorBorder;
    ctx.setLineDash(isSof ? [4, 4] : []);
    ctx.beginPath();
    ctx.moveTo(px, margin.top);
    ctx.lineTo(px, margin.top + plotH);
    ctx.stroke();
    ctx.setLineDash([]);
    ctx.fillStyle = isSof ? colorText : colorMuted;
    ctx.fillText(isSof ? 'SOF +' + h / FRAME_NS + ' ms' : (h / 1000).toFixed(0), px, margin.top + plotH + 6);
  }

  // Oldest first, so that the most recent arcs, which stand out, are on top.
  for (let i = 0; i < samples.length; i++) {
    const { v, h, k } = samples[i];
    const age = samples.length - 1 - i;
    const x0 = hToPx(v);
    const x1 = hToPx(h);
    const rx = Math.max(0, (x1 - x0) / 2);
    ctx.globalAlpha = fadeAlpha(age, samples.length);
    ctx.strokeStyle = slotColor(k);
    ctx.beginPath();
    if (k % 2 === 1) {
      ctx.ellipse(x0 + rx, axisY, rx, rx * arcScale, 0, Math.PI, 2 * Math.PI);
    } else {
      ctx.ellipse(x0 + rx, axisY, rx, rx * arcScale, 0, Math.PI, 0, true);
    }
    ctx.stroke();
  }
  ctx.globalAlpha = 1;

  ctx.strokeStyle = colorText;
  ctx.beginPath();
  ctx.moveTo(margin.left, Math.round(axisY) + 0.5);
  ctx.lineTo(margin.left + plotW, Math.round(axisY) + 0.5);
  ctx.stroke();
}

// Stacked histogram of the time of the toggle within its frame, colored by
// the frame the response arrived in.
function drawHistogramChart({ colorText, colorMuted, colorBorder, styles, cssW, cssH }) {
  const { samples, slots, slotColor } = groupBySlot(styles);

  const margin = { left: 64, right: 16, top: 24, bottom: 44 };
  const plotW = Math.max(1, cssW - margin.left - margin.right);
  const plotH = Math.max(1, cssH - margin.top - margin.bottom);
  // The tester spreads 2048 toggles evenly over the frame, so the number of
  // bins is a power of two as well, to get the same number in each. Every
  // 32nd toggle is then right at the start of a bin, so the bins are taken
  // half a toggle step early to keep measurement jitter from moving those to
  // the previous bin.
  const binNs = FRAME_NS / 64;
  const binOffsetNs = FRAME_NS / 2048 / 2;
  const xMax = FRAME_NS;
  const xTickNs = FRAME_NS / 8;
  const nBins = Math.ceil(xMax / binNs);
  const slotKeys = [...slots].sort((a, b) => a - b);
  const bins = Array.from({ length: nBins }, () => new Map());
  for (const { v, k } of samples) {
    const bin = bins[Math.min(nBins - 1, Math.max(0, Math.floor((v + binOffsetNs) / binNs)))];
    bin.set(k, (bin.get(k) || 0) + 1);
  }
  let yMax = 0;
  for (const bin of bins) {
    let total = 0;
    for (const c of bin.values()) total += c;
    if (total > yMax) yMax = total;
  }
  // At most five gridlines above zero, at round counts: the smallest step
  // of 1, 2 or 5 times a power of ten that gets there.
  let yStep;
  for (let mag = 1; !yStep; mag *= 10) {
    yStep = [1, 2, 5].map((m) => m * mag).find((step) => Math.ceil(yMax / step) <= 5);
  }
  yMax = Math.max(1, Math.ceil(yMax / yStep)) * yStep;
  const xToPx = (x) => margin.left + (x / xMax) * plotW;
  const yToPx = (y) => margin.top + plotH - (y / yMax) * plotH;

  ctx.lineWidth = 1;
  ctx.strokeStyle = colorBorder;
  ctx.fillStyle = colorMuted;
  ctx.textAlign = 'center';
  ctx.textBaseline = 'top';
  for (let x = 0; x <= xMax; x += xTickNs) {
    const px = xToPx(x);
    ctx.beginPath();
    ctx.moveTo(px, margin.top);
    ctx.lineTo(px, margin.top + plotH);
    ctx.stroke();
    ctx.fillText((x / 1000).toFixed(0), px, margin.top + plotH + 6);
  }
  ctx.textAlign = 'right';
  ctx.textBaseline = 'middle';
  for (let y = 0; y <= yMax; y += yStep) {
    const py = yToPx(y);
    ctx.beginPath();
    ctx.moveTo(margin.left, py);
    ctx.lineTo(margin.left + plotW, py);
    ctx.stroke();
    ctx.fillText(String(y), margin.left - 8, py);
  }

  for (let i = 0; i < nBins; i++) {
    let y = 0;
    for (const k of slotKeys) {
      const c = bins[i].get(k) || 0;
      if (c === 0) continue;
      ctx.fillStyle = slotColor(k);
      const x0 = xToPx(i * binNs) + 0.5;
      const x1 = xToPx((i + 1) * binNs) - 0.5;
      ctx.fillRect(x0, yToPx(y + c), Math.max(1, x1 - x0), yToPx(y) - yToPx(y + c));
      y += c;
    }
  }

  // Legend, on a background as it can be on top of the bars.
  const labels = slotKeys.map((k) => 'report received after SOF +' + k + ' ms');
  const legendW = Math.max(0, ...labels.map((label) => ctx.measureText(label).width)) + 32;
  ctx.fillStyle = styles.getPropertyValue('--panel').trim() || '#ffffff';
  ctx.globalAlpha = 0.85;
  ctx.fillRect(margin.left + plotW - legendW, margin.top + 1, legendW - 1, slotKeys.length * 18 + 2);
  ctx.globalAlpha = 1;
  ctx.textAlign = 'right';
  ctx.textBaseline = 'middle';
  let ly = margin.top + 10;
  for (const k of slotKeys) {
    const label = 'report received after SOF +' + k + ' ms';
    ctx.fillStyle = colorText;
    ctx.fillText(label, margin.left + plotW - 20, ly);
    ctx.fillStyle = slotColor(k);
    ctx.fillRect(margin.left + plotW - 14, ly - 5, 10, 10);
    ly += 18;
  }

  ctx.fillStyle = colorText;
  ctx.textAlign = 'center';
  ctx.textBaseline = 'alphabetic';
  ctx.fillText('SOF → button toggle (µs)', margin.left + plotW / 2, cssH - 6);
  ctx.save();
  ctx.translate(14, margin.top + plotH / 2);
  ctx.rotate(-Math.PI / 2);
  ctx.fillText('samples', 0, 0);
  ctx.restore();
}

function renderLoop() {
  if (chartDirty) {
    drawChart();
    if (state.points.length > 0) {
      ctx.fillStyle = getComputedStyle(document.body).getPropertyValue('--muted').trim() || '#6b7280';
      ctx.textAlign = 'right';
      ctx.textBaseline = 'top';
      ctx.fillText(state.chartView + 1 + '/' + CHART_VIEW_COUNT, el.chart.clientWidth - 4, 2);
    }
    chartDirty = false;
  }
  requestAnimationFrame(renderLoop);
}

function init() {
  newRun(null);

  // Past runs can be imported and looked at without Web Serial.
  el.exportBtn.addEventListener('click', exportRun);
  el.importBtn.addEventListener('click', () => el.importFile.click());
  el.importFile.addEventListener('change', () => {
    const file = el.importFile.files[0];
    el.importFile.value = '';
    if (file) {
      importRun(file);
    }
  });
  el.chart.addEventListener('click', () => {
    state.chartView = (state.chartView + 1) % CHART_VIEW_COUNT;
    chartDirty = true;
  });
  window.addEventListener('resize', resizeCanvas);
  resizeCanvas();
  requestAnimationFrame(renderLoop);

  if (!('serial' in navigator)) {
    setStatusMessage('WebSerial not supported');
    el.connectBtn.disabled = true;
    logLine('Web Serial API not available.');
    return;
  }

  navigator.serial.addEventListener('disconnect', (e) => {
    if (e.target === state.port) {
      state.keepReading = false;
    }
  });

  el.connectBtn.addEventListener('click', onConnectButtonClick);
  el.startBtn.addEventListener('click', onStartButtonClick);
  updateDots();
  updateProgress();
}

init();
