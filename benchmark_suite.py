"""
Animus Core v1.0 production benchmark pipeline.

Replaces the placeholder pipeline that used to feed BENCHMARK_REPORT.json.
That version drove animus.core.EventEngine.process_telemetry_batch(), which
ignores its total_events parameter entirely and does nothing but iterate
over rules.json's signature list (see animus/core.py) -- it never touched
the native engine, so its "latency" numbers measured rules-list iteration,
not telemetry throughput, and its peak_throughput_ops field was a hardcoded
600,000,000 with no measurement behind it at all.

Every metric below is measured, not invented:

  - Multi-producer ring throughput (pushes/sec) and median tick-to-trade
    latency come from AnimusCore_v1/animus_benchmark_suite.cpp, compiled
    and run fresh by this script. That harness drives
    animus::LockFreeRingBuffer and the MarketDataFeed -> ExecutionClient
    decision loop directly, with real OS threads pinned to real cores --
    see that file's own header for why 8-thread ring contention and
    sub-microsecond tick-to-trade latency can't be honestly measured from
    Python (the GIL serializes "threads" onto one core, and the ctypes
    call-marshalling tax alone costs more than the latency budget being
    measured here).
  - Zero-copy Python drain latency comes from
    benchmarks/python_interop_latency.py's drain-only measurement, which
    drives the real nanobind TelemetryStream extension (animus/
    _animus_native) against a live, unthrottled background producer
    thread -- this script imports and calls that measurement directly
    rather than re-implementing it, so there is exactly one audited
    implementation of this number in the repo.

No fallback numbers: if the native harness can't be built, a harness run
doesn't emit the expected result line, or the nanobind extension isn't
available, this script raises instead of substituting a placeholder.
"""
import json
import re
import shutil
import statistics
import subprocess
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).parent
NATIVE_SRC = SCRIPT_DIR / "AnimusCore_v1" / "animus_benchmark_suite.cpp"
NATIVE_BIN = SCRIPT_DIR / "AnimusCore_v1" / "animus_benchmark_suite.exe"

DRAIN_EVENTS_PER_RUN = 2_000_000
DRAIN_NUM_RUNS = 3

RESULT_LINE_RE = re.compile(r"^(BENCHMARK_RESULT_\w+) (.+)$")


def build_native_benchmark_suite() -> Path:
    """Compile AnimusCore_v1/animus_benchmark_suite.cpp if missing or stale."""
    if NATIVE_BIN.exists() and NATIVE_BIN.stat().st_mtime >= NATIVE_SRC.stat().st_mtime:
        return NATIVE_BIN

    compiler = shutil.which("g++")
    if compiler is None:
        raise RuntimeError(
            "g++ not found on PATH -- required to build "
            f"{NATIVE_SRC} into {NATIVE_BIN}. Install MinGW/MSYS2 g++ "
            "(C++17) or build it manually per that file's own header comment."
        )

    cmd = [compiler, "-std=c++17", "-O2", "-pthread", str(NATIVE_SRC), "-o", str(NATIVE_BIN)]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        raise RuntimeError(
            f"Failed to compile {NATIVE_SRC}:\n{proc.stdout}\n{proc.stderr}"
        )
    return NATIVE_BIN


def run_native_benchmark_suite() -> dict:
    """Run animus_benchmark_suite.exe and parse its BENCHMARK_RESULT_* lines."""
    binary = build_native_benchmark_suite()
    proc = subprocess.run([str(binary)], capture_output=True, text=True, timeout=120)
    if proc.returncode != 0:
        raise RuntimeError(
            f"{binary} exited with code {proc.returncode}:\n{proc.stdout}\n{proc.stderr}"
        )

    results = {}
    for line in proc.stdout.splitlines():
        match = RESULT_LINE_RE.match(line)
        if match:
            results[match.group(1)] = json.loads(match.group(2))

    required = {
        "BENCHMARK_RESULT_TICK_TO_TRADE",
        "BENCHMARK_RESULT_RING_THROUGHPUT",
        "BENCHMARK_RESULT_SYSTEM_INFO",
    }
    missing = required - results.keys()
    if missing:
        raise RuntimeError(
            f"{binary} did not emit expected result line(s) {sorted(missing)}. "
            f"Full stdout:\n{proc.stdout}"
        )
    return results


def measure_zero_copy_drain_latency(events_per_run: int, num_runs: int) -> dict:
    """Real, re-runnable measurement -- see benchmarks/python_interop_latency.py."""
    sys.path.insert(0, str(SCRIPT_DIR))
    try:
        from benchmarks.python_interop_latency import _drain_only_ns_per_event
    except ImportError as exc:
        raise RuntimeError(
            "Could not import benchmarks.python_interop_latency -- this needs the "
            "compiled nanobind extension (animus/_animus_native*). Build it per "
            "that module's own docstring before running this benchmark."
        ) from exc

    samples_ns = [_drain_only_ns_per_event(events_per_run) for _ in range(num_runs)]
    median_ns = statistics.median(samples_ns)
    return {
        "events_per_run": events_per_run,
        "num_runs": num_runs,
        "ns_per_event_samples": [round(s, 3) for s in samples_ns],
        "median_ns_per_event": round(median_ns, 3),
        "median_events_per_sec": round(1e9 / median_ns, 1),
    }


def run_production_benchmark() -> dict:
    print("===================================================")
    print("      ANIMUS CORE v1.0 ENTERPRISE BENCHMARK")
    print("===================================================")

    print("[1/2] Building/running native animus_benchmark_suite (tick-to-trade + ring throughput)...")
    native = run_native_benchmark_suite()
    tick_to_trade = native["BENCHMARK_RESULT_TICK_TO_TRADE"]
    ring_throughput = native["BENCHMARK_RESULT_RING_THROUGHPUT"]
    system_info = native["BENCHMARK_RESULT_SYSTEM_INFO"]
    print(
        f"      tick-to-trade: {tick_to_trade['num_ticks']:,} ticks, "
        f"median {tick_to_trade['p50_ns']:.1f} ns, p99 {tick_to_trade['p99_ns']:.1f} ns"
    )
    print(
        f"      ring throughput: {ring_throughput['num_producer_threads']} producers, "
        f"{ring_throughput['throughput_pushes_per_sec']:,.0f} pushes/sec"
    )

    print("[2/2] Measuring zero-copy Python drain latency (real nanobind extension)...")
    drain = measure_zero_copy_drain_latency(DRAIN_EVENTS_PER_RUN, DRAIN_NUM_RUNS)
    print(
        f"      drain-only: median {drain['median_ns_per_event']:.2f} ns/event "
        f"({drain['median_events_per_sec']:,.0f} events/sec) over {DRAIN_NUM_RUNS} runs"
    )

    metrics = {
        "engine_version": "1.0.0",
        "median_tick_to_trade_latency_ns": tick_to_trade["p50_ns"],
        "multi_producer_throughput_pushes_per_sec": ring_throughput["throughput_pushes_per_sec"],
        "zero_copy_python_drain_median_ns_per_event": drain["median_ns_per_event"],
        "tick_to_trade": tick_to_trade,
        "ring_throughput": ring_throughput,
        "zero_copy_python_drain": drain,
        "system_info": system_info,
    }

    report_path = SCRIPT_DIR / "BENCHMARK_REPORT.json"
    with open(report_path, "w") as f:
        json.dump(metrics, f, indent=4)

    print("-------------------------------------------------")
    print(f"Benchmark summary saved to {report_path}")
    return metrics


if __name__ == "__main__":
    run_production_benchmark()
