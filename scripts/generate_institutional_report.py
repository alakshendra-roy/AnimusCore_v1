#!/usr/bin/env python3
"""Runs animus_bench (sustained + burst passes) on this machine and renders
benchmarks/reports/ANIMUS_BENCHMARK_REPORT.html from the real output --
every number in the generated report traces back to one real run of the
compiled binary plus this machine's own detected specs, never a hand-typed
or hardcoded figure (same discipline as
benchmarks/generate_benchmark_report.py's own header comment).

Driven by deploy_verify.sh (root of the repo) as the last step of the
institutional verification pass, but also runnable standalone:

    python scripts/generate_institutional_report.py --binary build/bin/animus_bench

If --binary is omitted, this looks for animus_bench at the conventional
build/bin/animus_bench (Linux/macOS) or build/bin/Release/animus_bench.exe
(Windows multi-config) locations relative to the repo root.
"""
from __future__ import annotations

import argparse
import datetime
import html
import os
import platform
import re
import subprocess
import sys

_REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
_REPORT_PATH = os.path.join(_REPO_ROOT, "benchmarks", "reports", "ANIMUS_BENCHMARK_REPORT.html")

_DEFAULT_BINARY_CANDIDATES = [
    os.path.join(_REPO_ROOT, "build", "bin", "animus_bench"),
    os.path.join(_REPO_ROOT, "build", "bin", "Release", "animus_bench.exe"),
    os.path.join(_REPO_ROOT, "build", "bin", "animus_bench.exe"),
]

_FIELD_PATTERNS = {
    "target_rate": r"Target Rate\s*:\s*([\d,]+)\s*msgs/sec",
    "duration_s": r"Duration\s*:\s*([\d.]+)\s*s",
    "ring_capacity": r"Ring Buffer Capacity\s*:\s*([\d,]+)\s*frames",
    "frames_processed": r"Total Frames Processed\s*:\s*([\d,]+)\s*frames",
    "sustained_rate": r"Sustained Ingest Rate\s*:\s*([\d,]+)\s*ops/sec",
    "drop_rate_pct": r"Packet Drop Rate\s*:\s*([\d.]+)\s*%",
    "corruption": r"Sequence Corruption\s*:\s*([\d,]+)\s*frames",
    "hot_allocs": r"Hot-Path Heap Allocations:\s*([\d,]+)\s*calls",
    "hot_deallocs": r"Hot-Path Heap Frees\s*:\s*([\d,]+)\s*calls",
    "p50_ns": r"p50 \(median\)\s*:\s*([\d,]+)\s*ns",
    "p90_ns": r"p90\s*:\s*([\d,]+)\s*ns",
    "p99_ns": r"p99\s*:\s*([\d,]+)\s*ns",
    "p999_ns": r"p99\.9\s*:\s*([\d,]+)\s*ns",
    "max_ns": r"Max \(tail jitter\)\s*:\s*([\d,]+)\s*ns",
}


def find_binary(explicit: str) -> str:
    # Always returns an absolute path: on Windows, subprocess/CreateProcess
    # does not reliably resolve a relative path containing forward slashes
    # against the current working directory the way a shell would --
    # confirmed by actually hitting FileNotFoundError with a relative path
    # here, not a theoretical concern.
    if explicit:
        if not os.path.isfile(explicit):
            raise SystemExit(f"error: --binary path does not exist: {explicit}")
        return os.path.abspath(explicit)
    for candidate in _DEFAULT_BINARY_CANDIDATES:
        if os.path.isfile(candidate):
            return os.path.abspath(candidate)
    raise SystemExit(
        "error: animus_bench binary not found at any conventional path "
        f"({', '.join(_DEFAULT_BINARY_CANDIDATES)}). Build it first "
        "(see deploy_verify.sh step 3, or `cmake --build build --target animus_bench`) "
        "or pass --binary explicitly."
    )


def run_pass(binary: str, rate: int, duration: float, burst: bool) -> str:
    args = [binary, "--rate", str(rate), "--duration", str(duration)]
    if burst:
        args.append("--burst")
    print(f"[generate_institutional_report] running: {' '.join(args)}")
    proc = subprocess.run(args, capture_output=True, text=True, timeout=max(60.0, duration * 4 + 30))
    if proc.returncode != 0:
        raise RuntimeError(
            f"animus_bench exited {proc.returncode}:\nstdout:\n{proc.stdout}\nstderr:\n{proc.stderr}"
        )
    return proc.stdout


def parse_pass(stdout: str) -> dict:
    result = {}
    for key, pattern in _FIELD_PATTERNS.items():
        m = re.search(pattern, stdout)
        if not m:
            raise RuntimeError(f"could not find field '{key}' in animus_bench output:\n{stdout}")
        raw = m.group(1).replace(",", "")
        result[key] = float(raw) if "." in raw else int(raw)
    result["verified"] = "VERIFIED: 0 dropped frames, 0 corrupted frames, 0 hot-path heap allocations." in stdout
    result["raw_stdout"] = stdout
    return result


def _read_sysfs_cache_sizes() -> "list[str]":
    """Reads L1d/L1i/L2/L3 sizes from Linux sysfs (/sys/devices/system/cpu/cpu0/cache/index*),
    a portable mechanism across distros that doesn't depend on any particular
    lscpu version/output format being installed. Returns [] on any platform
    or environment where this path doesn't exist (e.g. Windows, macOS, a
    restricted container)."""
    base = "/sys/devices/system/cpu/cpu0/cache"
    if not os.path.isdir(base):
        return []
    entries = []
    for name in sorted(os.listdir(base)):
        idx_dir = os.path.join(base, name)
        level_f = os.path.join(idx_dir, "level")
        type_f = os.path.join(idx_dir, "type")
        size_f = os.path.join(idx_dir, "size")
        if not (os.path.isfile(level_f) and os.path.isfile(type_f) and os.path.isfile(size_f)):
            continue
        try:
            with open(level_f) as f:
                level = f.read().strip()
            with open(type_f) as f:
                ctype = f.read().strip()
            with open(size_f) as f:
                size = f.read().strip()
        except OSError:
            continue
        label = f"L{level}" + ({"Data": "d", "Instruction": "i", "Unified": ""}.get(ctype, ""))
        entries.append(f"{label}: {size}")
    return entries


def detect_machine_specs() -> dict:
    """Best-effort, platform-appropriate machine identification. Every
    field is either read directly from the OS or left as 'unknown' --
    never guessed or filled with a placeholder that looks like real data."""
    specs = {
        "cpu_model": "unknown",
        "logical_cores": os.cpu_count() or 0,
        "physical_cores": "unknown",
        "cache_sizes": "unknown",
        "os": f"{platform.system()} {platform.release()}",
        "arch": platform.machine(),
    }

    if platform.system() == "Linux":
        try:
            with open("/proc/cpuinfo") as f:
                cpuinfo = f.read()
            m = re.search(r"model name\s*:\s*(.+)", cpuinfo)
            if m:
                specs["cpu_model"] = m.group(1).strip()
        except OSError:
            pass
        try:
            lscpu = subprocess.run(["lscpu"], capture_output=True, text=True, timeout=5)
            if lscpu.returncode == 0:
                m = re.search(r"Core\(s\) per socket:\s*(\d+)", lscpu.stdout)
                s = re.search(r"Socket\(s\):\s*(\d+)", lscpu.stdout)
                if m and s:
                    specs["physical_cores"] = str(int(m.group(1)) * int(s.group(1)))
        except (OSError, subprocess.SubprocessError):
            pass
        cache = _read_sysfs_cache_sizes()
        if cache:
            specs["cache_sizes"] = ", ".join(cache)
    elif platform.system() == "Windows":
        specs["cpu_model"] = platform.processor() or "unknown"
        try:
            ps = subprocess.run(
                ["powershell", "-NoProfile", "-Command",
                 "Get-CimInstance Win32_Processor | Select-Object -First 1 -ExpandProperty Name"],
                capture_output=True, text=True, timeout=10,
            )
            if ps.returncode == 0 and ps.stdout.strip():
                specs["cpu_model"] = ps.stdout.strip()
        except (OSError, subprocess.SubprocessError):
            pass
    else:
        specs["cpu_model"] = platform.processor() or platform.machine() or "unknown"

    return specs


def _esc(value) -> str:
    return html.escape(str(value))


def render_html(sustained: dict, burst: dict, specs: dict, rate_target: int) -> str:
    now = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S %Z").strip()
    all_verified = sustained["verified"] and burst["verified"]
    status_color = "#10b981" if all_verified else "#f87171"
    status_text = "ALL PASSES VERIFIED" if all_verified else "VERIFICATION FAILED -- see raw output below"

    def pass_rows(p: dict) -> str:
        return f"""
        <tr><td>Total Frames Processed</td><td>{_esc(f"{p['frames_processed']:,}")}</td></tr>
        <tr><td>Sustained Ingest Rate</td><td>{_esc(f"{p['sustained_rate']:,}")} ops/sec</td></tr>
        <tr><td>Packet Drop Rate</td><td>{_esc(p['drop_rate_pct'])}%</td></tr>
        <tr><td>Sequence Corruption</td><td>{_esc(f"{p['corruption']:,}")} frames</td></tr>
        <tr><td>Hot-Path Heap Allocations</td><td>{_esc(f"{p['hot_allocs']:,}")} calls</td></tr>
        <tr><td>Hot-Path Heap Frees</td><td>{_esc(f"{p['hot_deallocs']:,}")} calls</td></tr>
        <tr><td>p50 / p99 / p99.9 / Max Latency (ns)</td>
            <td>{_esc(f"{p['p50_ns']:,}")} / {_esc(f"{p['p99_ns']:,}")} / {_esc(f"{p['p999_ns']:,}")} / {_esc(f"{p['max_ns']:,}")}</td></tr>
"""

    return f"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Animus Core &mdash; Institutional Benchmark Verification Report</title>
<style>
  :root{{
    --bg:#090d16; --surface:#0f172a; --surface-2:#1e293b;
    --accent:#38bdf8; --pass:#10b981; --fail:#f87171;
    --text:#f8fafc; --text-dim:#94a3b8; --border:rgba(148,163,184,0.16);
    --mono: ui-monospace, "SFMono-Regular", Consolas, "Liberation Mono", Menlo, monospace;
    --sans: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
  }}
  *{{box-sizing:border-box;}}
  body{{background:var(--bg);color:var(--text);font-family:var(--sans);margin:0;padding:40px 24px;line-height:1.5;}}
  .wrap{{max-width:960px;margin:0 auto;}}
  h1{{font-size:22px;letter-spacing:0.02em;margin-bottom:4px;}}
  .subtitle{{color:var(--accent);font-family:var(--mono);font-size:13px;margin-bottom:28px;}}
  .status-banner{{
    border:1px solid {status_color}; background:rgba(16,185,129,0.08);
    color:{status_color}; font-family:var(--mono); font-weight:700;
    padding:14px 18px; border-radius:8px; margin-bottom:28px; font-size:14px;
  }}
  .card{{background:var(--surface);border:1px solid var(--border);border-radius:10px;padding:20px 24px;margin-bottom:20px;}}
  .card h2{{font-size:14px;text-transform:uppercase;letter-spacing:0.06em;color:var(--accent);margin:0 0 14px;}}
  table{{width:100%;border-collapse:collapse;font-family:var(--mono);font-size:13px;}}
  td{{padding:6px 8px;border-bottom:1px solid var(--border);}}
  td:first-child{{color:var(--text-dim);}}
  td:last-child{{text-align:right;color:var(--text);}}
  pre{{background:#050810;border:1px solid var(--border);border-radius:8px;padding:14px;overflow-x:auto;font-size:11.5px;color:var(--text-dim);}}
  .footnote{{color:var(--text-dim);font-size:12px;margin-top:24px;}}
</style>
</head>
<body>
<div class="wrap">
  <h1>Animus Core &mdash; Institutional Benchmark Verification Report</h1>
  <div class="subtitle">Generated {_esc(now)} by scripts/generate_institutional_report.py</div>

  <div class="status-banner">{_esc(status_text)}</div>

  <div class="card">
    <h2>System Under Test</h2>
    <table>
      <tr><td>CPU Model</td><td>{_esc(specs['cpu_model'])}</td></tr>
      <tr><td>Logical Cores</td><td>{_esc(specs['logical_cores'])}</td></tr>
      <tr><td>Physical Cores</td><td>{_esc(specs['physical_cores'])}</td></tr>
      <tr><td>Cache Topology</td><td>{_esc(specs['cache_sizes'])}</td></tr>
      <tr><td>OS</td><td>{_esc(specs['os'])}</td></tr>
      <tr><td>Architecture</td><td>{_esc(specs['arch'])}</td></tr>
    </table>
  </div>

  <div class="card">
    <h2>Sustained Pass (target {_esc(f"{rate_target:,}")} msgs/sec)</h2>
    <table>{pass_rows(sustained)}</table>
  </div>

  <div class="card">
    <h2>Burst Pass</h2>
    <table>{pass_rows(burst)}</table>
  </div>

  <div class="card">
    <h2>Raw animus_bench Output</h2>
    <pre>{_esc(sustained['raw_stdout'])}</pre>
    <pre>{_esc(burst['raw_stdout'])}</pre>
  </div>

  <p class="footnote">
    Every figure above comes from one real run of the compiled <code>animus_bench</code>
    binary on this machine, invoked by <code>deploy_verify.sh</code> (or directly via
    <code>python scripts/generate_institutional_report.py</code>). Regenerate this file
    on any other machine to reproduce or refresh it -- do not hand-edit the numbers above.
  </p>
</div>
</body>
</html>
"""


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--binary", default="", help="path to the animus_bench executable")
    p.add_argument("--rate", type=int, default=10_000_000, help="sustained-pass target rate (default: 10,000,000)")
    p.add_argument("--duration", type=float, default=5.0, help="each pass's duration in seconds (default: 5.0)")
    p.add_argument("--out", default=_REPORT_PATH, help="output HTML path (default: benchmarks/reports/ANIMUS_BENCHMARK_REPORT.html)")
    args = p.parse_args()

    binary = find_binary(args.binary)
    sustained_stdout = run_pass(binary, args.rate, args.duration, burst=False)
    burst_stdout = run_pass(binary, args.rate, args.duration, burst=True)

    sustained = parse_pass(sustained_stdout)
    burst = parse_pass(burst_stdout)
    specs = detect_machine_specs()

    report = render_html(sustained, burst, specs, args.rate)
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as f:
        f.write(report)

    print(f"[generate_institutional_report] wrote {args.out}")
    if not (sustained["verified"] and burst["verified"]):
        raise SystemExit(
            "error: one or both animus_bench passes did not report the "
            "VERIFIED line (0 dropped frames, 0 corrupted frames, 0 "
            "hot-path heap allocations) -- see the report's raw output section"
        )


if __name__ == "__main__":
    main()
