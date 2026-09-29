"""
Animus Core sandbox -- live telemetry dashboard.

Serves a single-page, real-time view of the same MPMC ring buffer /
nanobind bridge this sandbox's other harnesses exercise (see
../README.md, nanobind_bridge.cpp): a native background thread feeds the
ring via TelemetryStream.start_continuous() (a best-effort, bounded-retry
producer -- unlike start_producer()'s unbounded-retry mode used by
python_bridge_test.py, this one can genuinely drop events under
sustained overload, so the dashboard's drop counter means something).
A second, plain Python thread drains it in a tight loop and folds each
batch's per-event latency (TSC now - TSC at dispatch, converted via the
bridge's own once-calibrated cycles_per_ns) into a fixed-size rolling
window; an asyncio task on the FastAPI event loop reads that window a
few times a second and pushes throughput / P50 / P99 / buffer occupancy /
drop-counter snapshots to every connected browser over one WebSocket.

Run (from this directory, after building animus_sandbox_bridge -- see
../README.md):
    python dashboard_server.py [--host 127.0.0.1] [--port 8765]
                                [--capacity 1048576] [--batch 16384]
                                [--max-retry-spins 64]
Then open http://127.0.0.1:8765/ in a browser.
"""
from __future__ import annotations

import argparse
import asyncio
import json
import os
import sys
import threading
import time
from contextlib import asynccontextmanager

import numpy as np
from fastapi import FastAPI, WebSocket, WebSocketDisconnect
from fastapi.responses import HTMLResponse
import uvicorn

if sys.platform == "win32":
    # See python_bridge_test.py: only needed for a MinGW-built .pyd (its
    # runtime DLLs aren't found via PATH alone under PEP 578 / bpo-36085);
    # harmless no-op against an MSVC-built, statically-linked .pyd.
    for _dll_dir in (r"C:\msys64\ucrt64\bin", r"C:\msys64\mingw64\bin"):
        if os.path.isdir(_dll_dir):
            os.add_dll_directory(_dll_dir)

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import animus_sandbox_bridge as bridge  # noqa: E402

# Must mirror sandbox_event.hpp's Event layout exactly -- both structs are
# validated against the bridge's own EVENT_SIZE_BYTES at startup below.
EVENT_DTYPE = np.dtype([
    ("sequence", "<u8"),
    ("dispatch_tsc", "<u8"),
    ("producer_id", "<u4"),
    ("reserved0", "<u4"),
    ("value", "<u8"),
    ("reserved1", "<u8", (4,)),
])
assert EVENT_DTYPE.itemsize == bridge.EVENT_SIZE_BYTES, (
    f"EVENT_DTYPE ({EVENT_DTYPE.itemsize} bytes) no longer matches "
    f"sandbox::Event ({bridge.EVENT_SIZE_BYTES} bytes) -- update the dtype "
    f"above to match sandbox_event.hpp"
)

LATENCY_WINDOW_SIZE = 20_000
BROADCAST_INTERVAL_S = 0.2


class TelemetryAggregator:
    """Owns the rolling latency window; written by drain_loop() (a plain
    thread), read by snapshot() (called from the asyncio broadcast loop).
    The lock only ever guards a small fixed-size numpy array copy/percentile
    call, so contention is not a concern at these rates."""

    def __init__(self, stream: "bridge.TelemetryStream", drain_batch: int):
        self.stream = stream
        self.drain_batch = drain_batch
        self._window = np.zeros(LATENCY_WINDOW_SIZE, dtype=np.float64)
        self._window_pos = 0
        self._window_filled = 0
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._last_consumed = 0
        self._last_dropped = 0
        self._last_time = time.perf_counter()

    def drain_loop(self) -> None:
        while not self._stop.is_set():
            raw = self.stream.drain(self.drain_batch)
            if raw.shape[0] == 0:
                time.sleep(0)  # yield without a fixed delay; ring was empty
                continue
            events = np.asarray(raw).view(EVENT_DTYPE)
            now_tsc = self.stream.tsc_now()
            dispatch = events["dispatch_tsc"].astype(np.int64)
            latency_ns = (np.int64(now_tsc) - dispatch) / self.stream.cycles_per_ns
            # Clock skew between the producer's and this thread's TSC reads
            # (or scheduling delay before this batch was drained) can put a
            # sample below zero; clip rather than let it corrupt percentiles.
            np.clip(latency_ns, 0.0, None, out=latency_ns)
            with self._lock:
                self._push(latency_ns)

    def _push(self, latencies: np.ndarray) -> None:
        n = latencies.shape[0]
        w = self._window.shape[0]
        if n >= w:
            self._window[:] = latencies[-w:]
            self._window_pos = 0
            self._window_filled = w
            return
        end = self._window_pos + n
        if end <= w:
            self._window[self._window_pos:end] = latencies
        else:
            first = w - self._window_pos
            self._window[self._window_pos:] = latencies[:first]
            self._window[:n - first] = latencies[first:]
        self._window_pos = end % w
        self._window_filled = min(w, self._window_filled + n)

    def stop(self) -> None:
        self._stop.set()

    def snapshot(self) -> dict:
        now_t = time.perf_counter()
        produced = self.stream.produced_total
        consumed = self.stream.consumed_total
        dropped = self.stream.dropped_total
        occupancy = max(0, self.stream.occupancy_estimate)
        capacity = self.stream.capacity

        dt = now_t - self._last_time
        throughput = (consumed - self._last_consumed) / dt if dt > 0 else 0.0
        drop_rate = (dropped - self._last_dropped) / dt if dt > 0 else 0.0
        self._last_consumed = consumed
        self._last_dropped = dropped
        self._last_time = now_t

        with self._lock:
            filled = self._window_filled
            if filled > 0:
                valid = self._window[:filled]
                p50, p90, p99 = np.percentile(valid, [50, 90, 99])
            else:
                p50 = p90 = p99 = 0.0

        return {
            "throughput_eps": throughput,
            "p50_ns": float(p50),
            "p90_ns": float(p90),
            "p99_ns": float(p99),
            "occupancy": int(occupancy),
            "capacity": int(capacity),
            "produced_total": int(produced),
            "consumed_total": int(consumed),
            "dropped_total": int(dropped),
            "drop_rate_eps": drop_rate,
        }


class ConnectionManager:
    def __init__(self) -> None:
        self._active: set[WebSocket] = set()

    async def connect(self, ws: WebSocket) -> None:
        await ws.accept()
        self._active.add(ws)

    def disconnect(self, ws: WebSocket) -> None:
        self._active.discard(ws)

    async def broadcast(self, message: str) -> None:
        dead = []
        for ws in self._active:
            try:
                await ws.send_text(message)
            except Exception:
                dead.append(ws)
        for ws in dead:
            self._active.discard(ws)


def build_app(capacity: int, drain_batch: int, max_retry_spins: int) -> FastAPI:
    stream = bridge.TelemetryStream(capacity, drain_batch)
    aggregator = TelemetryAggregator(stream, drain_batch)
    manager = ConnectionManager()
    drain_thread = threading.Thread(target=aggregator.drain_loop, daemon=True)

    async def broadcast_loop() -> None:
        while True:
            await asyncio.sleep(BROADCAST_INTERVAL_S)
            await manager.broadcast(json.dumps(aggregator.snapshot()))

    @asynccontextmanager
    async def lifespan(_: FastAPI):
        stream.start_continuous(max_retry_spins)
        drain_thread.start()
        task = asyncio.create_task(broadcast_loop())
        try:
            yield
        finally:
            task.cancel()
            aggregator.stop()
            stream.stop_continuous()
            drain_thread.join(timeout=2.0)

    app = FastAPI(lifespan=lifespan)

    @app.get("/", response_class=HTMLResponse)
    async def index() -> str:
        return HTML_PAGE.replace("__CAPACITY__", str(capacity))

    @app.websocket("/ws")
    async def ws_endpoint(websocket: WebSocket) -> None:
        await manager.connect(websocket)
        try:
            while True:
                await websocket.receive_text()
        except WebSocketDisconnect:
            manager.disconnect(websocket)

    return app


HTML_PAGE = """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>AnimusCore -- Live Telemetry</title>
<style>
  :root {
    color-scheme: light;
    --surface-1:      #fcfcfb;
    --page-plane:     #f9f9f7;
    --text-primary:   #0b0b0b;
    --text-secondary: #52514e;
    --text-muted:     #898781;
    --gridline:       #e1e0d9;
    --baseline:       #c3c2b7;
    --border:         rgba(11,11,11,0.10);
    --series-1:       #2a78d6; /* throughput / p50 */
    --series-2:       #eb6834; /* p99 */
    --status-good:     #0ca30c;
    --status-warning:  #fab219;
    --status-critical: #d03b3b;
  }
  @media (prefers-color-scheme: dark) {
    :root:not([data-theme="light"]) {
      color-scheme: dark;
      --surface-1:      #1a1a19;
      --page-plane:     #0d0d0d;
      --text-primary:   #ffffff;
      --text-secondary: #c3c2b7;
      --text-muted:     #898781;
      --gridline:       #2c2c2a;
      --baseline:       #383835;
      --border:         rgba(255,255,255,0.10);
      --series-1:       #3987e5;
      --series-2:       #d95926;
      --status-good:     #0ca30c;
      --status-warning:  #fab219;
      --status-critical: #d03b3b;
    }
  }
  * { box-sizing: border-box; }
  body {
    margin: 0;
    background: var(--page-plane);
    color: var(--text-primary);
    font-family: system-ui, -apple-system, "Segoe UI", sans-serif;
    padding: 16px;
  }
  h1 {
    font-size: 15px;
    font-weight: 600;
    letter-spacing: 0.02em;
    text-transform: uppercase;
    color: var(--text-secondary);
    margin: 0 0 4px;
  }
  .subtitle { font-size: 13px; color: var(--text-muted); margin: 0 0 20px; }
  .conn-dot {
    display: inline-block; width: 8px; height: 8px; border-radius: 50%;
    background: var(--status-critical); margin-right: 6px; vertical-align: middle;
  }
  .conn-dot.live { background: var(--status-good); }
  .kpi-row {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(150px, 1fr));
    gap: 12px;
    margin-bottom: 16px;
  }
  .tile {
    background: var(--surface-1);
    border: 1px solid var(--border);
    border-radius: 10px;
    padding: 14px 16px;
  }
  .tile-label {
    font-size: 11px; letter-spacing: 0.04em; text-transform: uppercase;
    color: var(--text-secondary); margin-bottom: 6px;
  }
  .tile-value { font-size: 26px; font-weight: 600; line-height: 1.1; }
  .tile-unit { font-size: 13px; color: var(--text-muted); margin-left: 4px; }
  .tile-value.good { color: var(--status-good); }
  .tile-value.warning { color: var(--status-warning); }
  .tile-value.critical { color: var(--status-critical); }
  .charts-row {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 12px;
    margin-bottom: 16px;
  }
  @media (max-width: 760px) { .charts-row { grid-template-columns: 1fr; } }
  .chart-card {
    background: var(--surface-1);
    border: 1px solid var(--border);
    border-radius: 10px;
    padding: 14px 16px;
    position: relative;
  }
  .chart-title {
    font-size: 12px; color: var(--text-secondary); margin-bottom: 8px;
    display: flex; justify-content: space-between; align-items: center;
  }
  .legend { font-size: 11px; color: var(--text-muted); }
  .legend .swatch {
    display: inline-block; width: 8px; height: 8px; border-radius: 2px;
    margin-right: 4px; vertical-align: middle;
  }
  .legend .s1 { background: var(--series-1); }
  .legend .s2 { background: var(--series-2); }
  canvas { width: 100%; height: 160px; display: block; }
  .occ-card { background: var(--surface-1); border: 1px solid var(--border);
    border-radius: 10px; padding: 14px 16px; }
  .occ-bar-track {
    height: 14px; border-radius: 7px; background: var(--gridline);
    overflow: hidden; margin-top: 8px;
  }
  .occ-bar-fill {
    height: 100%; width: 0%; background: var(--series-1);
    transition: width 0.2s linear, background-color 0.2s linear;
  }
  .occ-caption { font-size: 12px; color: var(--text-muted); margin-top: 6px; }
  .tooltip {
    position: absolute; pointer-events: none; font-size: 11px;
    background: var(--text-primary); color: var(--surface-1);
    padding: 3px 6px; border-radius: 4px; opacity: 0; transform: translate(-50%, -120%);
    white-space: nowrap;
  }
</style>
</head>
<body>
  <h1><span id="conn-dot" class="conn-dot"></span>AnimusCore -- Live Pipeline Telemetry</h1>
  <p class="subtitle">Zero-copy nanobind bridge over the sandbox MPMC ring buffer (capacity __CAPACITY__) -- live, not a replay.</p>

  <div class="kpi-row">
    <div class="tile"><div class="tile-label">Throughput</div>
      <div class="tile-value" id="v-throughput">0<span class="tile-unit">events/s</span></div></div>
    <div class="tile"><div class="tile-label">P50 latency</div>
      <div class="tile-value" id="v-p50">0<span class="tile-unit">ns</span></div></div>
    <div class="tile"><div class="tile-label">P99 latency</div>
      <div class="tile-value" id="v-p99">0<span class="tile-unit">ns</span></div></div>
    <div class="tile"><div class="tile-label">Buffer occupancy</div>
      <div class="tile-value" id="v-occ">0<span class="tile-unit">%</span></div></div>
    <div class="tile"><div class="tile-label">Dropped (best-effort mode)</div>
      <div class="tile-value good" id="v-drops">0</div></div>
  </div>

  <div class="charts-row">
    <div class="chart-card">
      <div class="chart-title"><span>Throughput (events/sec)</span></div>
      <canvas id="chart-throughput" width="600" height="160"></canvas>
      <div class="tooltip" id="tt-throughput"></div>
    </div>
    <div class="chart-card">
      <div class="chart-title">
        <span>Latency distribution</span>
        <span class="legend"><span class="swatch s1"></span>P50 <span class="swatch s2"></span>P99</span>
      </div>
      <canvas id="chart-latency" width="600" height="160"></canvas>
      <div class="tooltip" id="tt-latency"></div>
    </div>
  </div>

  <div class="occ-card">
    <div class="chart-title"><span>Ring buffer occupancy</span><span class="legend" id="occ-caption-inline"></span></div>
    <div class="occ-bar-track"><div class="occ-bar-fill" id="occ-fill"></div></div>
    <div class="occ-caption" id="occ-caption">0 / __CAPACITY__ slots</div>
  </div>

<script>
const MAX_POINTS = 120;
const css = getComputedStyle(document.documentElement);
const colorSeries1 = css.getPropertyValue('--series-1').trim();
const colorSeries2 = css.getPropertyValue('--series-2').trim();
const colorGrid = css.getPropertyValue('--gridline').trim();
const colorMuted = css.getPropertyValue('--text-muted').trim();
const colorGood = css.getPropertyValue('--status-good').trim();
const colorWarning = css.getPropertyValue('--status-warning').trim();
const colorCritical = css.getPropertyValue('--status-critical').trim();

const throughputHistory = [];
const p50History = [];
const p99History = [];

function fmtNs(ns) {
  if (ns >= 1e6) return (ns / 1e6).toFixed(2) + '\u00a0ms';
  if (ns >= 1e3) return (ns / 1e3).toFixed(2) + '\u00a0\u00b5s';
  return Math.round(ns) + '\u00a0ns';
}
function fmtRate(v) {
  if (v >= 1e6) return (v / 1e6).toFixed(2) + 'M';
  if (v >= 1e3) return (v / 1e3).toFixed(1) + 'K';
  return Math.round(v).toString();
}

function drawLineChart(canvas, series, colors, tooltipEl) {
  const ctx = canvas.getContext('2d');
  const dpr = window.devicePixelRatio || 1;
  const w = canvas.clientWidth, h = canvas.clientHeight;
  canvas.width = w * dpr; canvas.height = h * dpr;
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, w, h);

  const allVals = series.flat().filter(v => Number.isFinite(v));
  const maxV = allVals.length ? Math.max(...allVals, 1) : 1;
  const pad = { top: 8, right: 8, bottom: 8, left: 8 };
  const plotW = w - pad.left - pad.right;
  const plotH = h - pad.top - pad.bottom;

  ctx.strokeStyle = colorGrid;
  ctx.lineWidth = 1;
  for (let i = 0; i <= 3; i++) {
    const y = pad.top + (plotH * i) / 3;
    ctx.beginPath(); ctx.moveTo(pad.left, y); ctx.lineTo(w - pad.right, y); ctx.stroke();
  }

  series.forEach((data, si) => {
    if (data.length < 2) return;
    ctx.strokeStyle = colors[si];
    ctx.lineWidth = 2;
    ctx.lineJoin = 'round'; ctx.lineCap = 'round';
    ctx.beginPath();
    data.forEach((v, i) => {
      const x = pad.left + (plotW * i) / (MAX_POINTS - 1);
      const y = pad.top + plotH - (v / maxV) * plotH;
      if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
    });
    ctx.stroke();
  });

  canvas.onmousemove = (ev) => {
    const rect = canvas.getBoundingClientRect();
    const mx = ev.clientX - rect.left;
    const idx = Math.round(((mx - pad.left) / plotW) * (MAX_POINTS - 1));
    const primary = series[0];
    if (idx < 0 || idx >= primary.length || !Number.isFinite(primary[idx])) {
      tooltipEl.style.opacity = 0; return;
    }
    const x = pad.left + (plotW * idx) / (MAX_POINTS - 1);
    const y = pad.top + plotH - (primary[idx] / maxV) * plotH;
    tooltipEl.style.left = x + 'px';
    tooltipEl.style.top = y + 'px';
    tooltipEl.style.opacity = 1;
    tooltipEl.textContent = series.length > 1
      ? `P50 ${fmtNs(series[0][idx])} / P99 ${fmtNs(series[1][idx])}`
      : fmtRate(primary[idx]) + '/s';
  };
  canvas.onmouseleave = () => { tooltipEl.style.opacity = 0; };
}

function pushHistory(arr, val) {
  arr.push(val);
  if (arr.length > MAX_POINTS) arr.shift();
}

function connect() {
  const proto = location.protocol === 'https:' ? 'wss' : 'ws';
  const ws = new WebSocket(`${proto}://${location.host}/ws`);
  const dot = document.getElementById('conn-dot');
  ws.onopen = () => dot.classList.add('live');
  ws.onclose = () => { dot.classList.remove('live'); setTimeout(connect, 1000); };
  ws.onerror = () => ws.close();
  ws.onmessage = (ev) => {
    const s = JSON.parse(ev.data);
    document.getElementById('v-throughput').innerHTML = fmtRate(s.throughput_eps) + '<span class="tile-unit">events/s</span>';
    document.getElementById('v-p50').innerHTML = fmtNs(s.p50_ns);
    document.getElementById('v-p99').innerHTML = fmtNs(s.p99_ns);

    const occPct = s.capacity > 0 ? (100 * s.occupancy / s.capacity) : 0;
    document.getElementById('v-occ').innerHTML = occPct.toFixed(1) + '<span class="tile-unit">%</span>';
    const fill = document.getElementById('occ-fill');
    fill.style.width = Math.min(100, occPct) + '%';
    fill.style.background = occPct >= 95 ? colorCritical : occPct >= 80 ? colorWarning : colorSeries1;
    document.getElementById('occ-caption').textContent = `${s.occupancy.toLocaleString()} / ${s.capacity.toLocaleString()} slots`;

    const dropsEl = document.getElementById('v-drops');
    dropsEl.textContent = s.dropped_total.toLocaleString();
    dropsEl.className = 'tile-value ' + (s.dropped_total === 0 ? 'good' : (s.drop_rate_eps > 0 ? 'critical' : 'warning'));

    pushHistory(throughputHistory, s.throughput_eps);
    pushHistory(p50History, s.p50_ns);
    pushHistory(p99History, s.p99_ns);

    drawLineChart(document.getElementById('chart-throughput'), [throughputHistory], [colorSeries1],
      document.getElementById('tt-throughput'));
    drawLineChart(document.getElementById('chart-latency'), [p50History, p99History], [colorSeries1, colorSeries2],
      document.getElementById('tt-latency'));
  };
}
connect();
</script>
</body>
</html>
"""


def main() -> None:
    parser = argparse.ArgumentParser(description="Animus Core sandbox live telemetry dashboard")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--capacity", type=int, default=1 << 20)
    parser.add_argument("--batch", type=int, default=16384)
    parser.add_argument("--max-retry-spins", type=int, default=64)
    args = parser.parse_args()

    app = build_app(args.capacity, args.batch, args.max_retry_spins)
    uvicorn.run(app, host=args.host, port=args.port, log_level="info")


if __name__ == "__main__":
    main()
