"""10-minute continuous soak test for the Animus Core native engine.

Runs a fixed wall-clock duration (default 600s / 10 minutes) of continuous,
rate-paced record_events_batch() calls against the real compiled native
engine to evaluate three things benchmarks/stress_test_engine.py's shorter,
fixed-event-count run doesn't cover: whether memory stays flat under
*sustained* load (not just a single pass), whether the engine's
zero-per-event-heap-allocation design (animus.hpp's ring buffers are
allocated once at construction -- see BENCHMARKS.md Phase 12) holds up over
minutes rather than seconds, and whether p99 batch-call latency drifts as
the run goes on (a lock-free ring under sustained pressure is exactly the
kind of thing that can look fine for 30 seconds and degrade at minute 8 --
e.g. from allocator fragmentation, persistence-worker backlog, or
GC-adjacent pauses on the Python side).

Why paced, not max-throughput: the tail-latency and stress-test benchmarks
elsewhere in this repo already push the engine flat-out for a fixed event
count; this script's job is different -- a bounded, sustained rate held for
a long duration, which is what "soak test" means (and keeps the persisted
log file in the low single-digit GB range instead of growing unbounded for
ten minutes at the engine's actual multi-million-events/sec ceiling).

Method:
  - Real pipeline: init -> add_rule -> start_logging -> a loop of
    record_events_batch() calls paced to TARGET_EVENTS_PER_SEC, draining
    signals periodically and on backpressure (same handling as
    benchmarks/stress_test_engine.py) -> stop_logging.
  - RSS sampled on a wall-clock timer independent of batch cadence, so
    memory samples are evenly spaced across the full duration.
  - Every batch's call latency (perf_counter_ns around record_events_batch
    only -- Python-side list construction happens before the timer starts,
    same discipline as benchmarks/fintech_tail_latency.py) is bucketed into
    fixed-length time windows. Percentiles are computed per window, not
    just once for the whole run, so a p99 that's fine in aggregate but
    creeping upward window-over-window is visible instead of averaged away.

Run with:
    python benchmarks/soak_test_engine.py [duration_seconds]
"""
import ctypes
import gc
import json
import math
import os
import sys
import tempfile
import time
from dataclasses import dataclass
from typing import List, Optional

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from animus.bindings import AnimusBindings, RuleComparator  # noqa: E402

DEFAULT_DURATION_S = 600.0
BATCH_SIZE = 500
TARGET_EVENTS_PER_SEC = 50_000
RING_CAPACITY = 1 << 16  # 65,536
RSS_SAMPLE_INTERVAL_S = 15.0
LATENCY_WINDOW_S = 30.0
RULE_EVENT_ID = 1
RULE_THRESHOLD = 90  # metric_value = i % 100 -> ~9% match rate
PAYLOAD_SIZE_BYTES = 64  # sizeof(TelemetryPayload), alignas(64) -- BENCHMARKS.md Phase 1/12
RSS_GROWTH_FLAG_PCT = 10.0  # same threshold as stress_test_engine.py Part 1
LATENCY_DRIFT_FLAG_PCT = 50.0  # flag if any window's p99 exceeds the stable baseline by more than this


def get_rss_bytes() -> int:
    """Copied from benchmarks/stress_test_engine.py -- see that file's
    docstring for why each platform branch is implemented this way,
    including the explicit argtypes/restype on Windows that fixes a real
    WinError 6 found while building that script.
    """
    if sys.platform == "win32":
        from ctypes import wintypes

        class PROCESS_MEMORY_COUNTERS(ctypes.Structure):
            _fields_ = [
                ("cb", wintypes.DWORD),
                ("PageFaultCount", wintypes.DWORD),
                ("PeakWorkingSetSize", ctypes.c_size_t),
                ("WorkingSetSize", ctypes.c_size_t),
                ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
                ("QuotaPagedPoolUsage", ctypes.c_size_t),
                ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
                ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
                ("PagefileUsage", ctypes.c_size_t),
                ("PeakPagefileUsage", ctypes.c_size_t),
            ]

        kernel32 = ctypes.windll.kernel32
        psapi = ctypes.windll.psapi
        kernel32.GetCurrentProcess.restype = wintypes.HANDLE
        kernel32.GetCurrentProcess.argtypes = []
        psapi.GetProcessMemoryInfo.restype = wintypes.BOOL
        psapi.GetProcessMemoryInfo.argtypes = [
            wintypes.HANDLE, ctypes.POINTER(PROCESS_MEMORY_COUNTERS), wintypes.DWORD,
        ]

        counters = PROCESS_MEMORY_COUNTERS()
        counters.cb = ctypes.sizeof(PROCESS_MEMORY_COUNTERS)
        handle = kernel32.GetCurrentProcess()
        if not psapi.GetProcessMemoryInfo(handle, ctypes.byref(counters), counters.cb):
            raise ctypes.WinError()
        return counters.WorkingSetSize

    try:
        with open("/proc/self/status") as fh:
            for line in fh:
                if line.startswith("VmRSS:"):
                    return int(line.split()[1]) * 1024
    except FileNotFoundError:
        pass
    import resource
    ru = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
    return ru if sys.platform == "darwin" else ru * 1024


def fmt_mb(nbytes: int) -> str:
    return f"{nbytes / (1024 * 1024):,.2f} MB"


def percentile(sorted_data: List[float], pct: float) -> float:
    """Linear-interpolation percentile, same implementation as
    benchmarks/fintech_tail_latency.py (kept in sync deliberately -- no
    numpy dependency, consistent with this SDK's zero-dependency design).
    """
    if not sorted_data:
        return float("nan")
    if len(sorted_data) == 1:
        return sorted_data[0]
    k = (pct / 100.0) * (len(sorted_data) - 1)
    f = math.floor(k)
    c = math.ceil(k)
    if f == c:
        return sorted_data[int(k)]
    d0 = sorted_data[int(f)] * (c - k)
    d1 = sorted_data[int(c)] * (k - f)
    return d0 + d1


@dataclass
class RssSample:
    elapsed_s: float
    rss_bytes: int


@dataclass
class WindowStats:
    window_idx: int
    start_s: float
    end_s: float
    num_calls: int
    num_events: int
    mean_us: float
    p50_us: float
    p99_us: float
    max_us: float


def summarize_window(idx: int, start_s: float, end_s: float, latencies_us: List[float], events_per_call: int) -> WindowStats:
    data = sorted(latencies_us)
    return WindowStats(
        window_idx=idx,
        start_s=start_s,
        end_s=end_s,
        num_calls=len(data),
        num_events=len(data) * events_per_call,
        mean_us=(sum(data) / len(data)) if data else float("nan"),
        p50_us=percentile(data, 50),
        p99_us=percentile(data, 99),
        max_us=data[-1] if data else float("nan"),
    )


def run_soak_test(duration_s: float) -> dict:
    bindings = AnimusBindings()
    if not bindings.using_native_engine:
        raise RuntimeError(
            "no compiled native engine found -- this soak test requires the real "
            "AnimusNative/AnimusCore_v1 binary, not the pure-Python fallback "
            "(build AnimusCore_v1.slnx or CMakeLists.txt first)"
        )

    gc.collect()
    cold_rss = get_rss_bytes()

    if not bindings.init(buffer_capacity=RING_CAPACITY):
        raise RuntimeError("animus_init failed")
    bindings.add_rule(
        rule_id=1, event_id=RULE_EVENT_ID, threshold=RULE_THRESHOLD,
        comparator=RuleComparator.GREATER_THAN, severity=5,
    )

    print("=" * 100)
    print(f"  ANIMUS CORE SOAK TEST -- {duration_s:.0f}s continuous sustained load")
    print("=" * 100)
    print(f"Batch size: {BATCH_SIZE:,} | Target rate: {TARGET_EVENTS_PER_SEC:,} events/sec | "
          f"Ring capacity: {RING_CAPACITY:,}")
    print(f"RSS sample interval: {RSS_SAMPLE_INTERVAL_S:.0f}s | Latency window: {LATENCY_WINDOW_S:.0f}s")
    print(f"Cold baseline RSS (before init): {fmt_mb(cold_rss)}")

    rss_samples: List[RssSample] = []
    windows: List[WindowStats] = []
    warm_sample: Optional[RssSample] = None
    warm_threshold_events = 2 * RING_CAPACITY
    batch_interval_s = BATCH_SIZE / TARGET_EVENTS_PER_SEC

    with tempfile.TemporaryDirectory() as tmp:
        log_path = os.path.join(tmp, "soak_telemetry.log")
        bindings.start_logging(log_path)

        gc.collect()
        post_init_rss = get_rss_bytes()
        rss_samples.append(RssSample(0.0, post_init_rss))
        print(f"Post-init RSS: {fmt_mb(post_init_rss)}")

        total_events = 0
        total_matches = 0
        batch_idx = 0

        t_start = time.perf_counter()
        next_rss_sample_at = RSS_SAMPLE_INTERVAL_S
        window_idx = 0
        window_start_s = 0.0
        window_latencies: List[float] = []

        while True:
            t_batch_start = time.perf_counter()
            elapsed = t_batch_start - t_start
            if elapsed >= duration_s:
                break

            events = [
                (RULE_EVENT_ID, total_events + i, (total_events + i) % 100)
                for i in range(BATCH_SIZE)
            ]

            t0 = time.perf_counter_ns()
            pushed = bindings.record_events_batch(events)
            t1 = time.perf_counter_ns()

            if pushed < BATCH_SIZE:
                # Ring momentarily full: drain signals to make room, then push
                # the remainder. Not expected at this target rate (well below
                # the engine's proven multi-million-events/sec capacity -- see
                # BENCHMARKS.md Phase 11), but handled rather than assumed
                # away over a 10-minute run.
                total_matches += len(bindings.poll_signals(max_count=4096))
                remaining = events[pushed:]
                while remaining:
                    n = bindings.record_events_batch(remaining)
                    if n == 0:
                        total_matches += len(bindings.poll_signals(max_count=4096))
                        time.sleep(0.001)
                        continue
                    remaining = remaining[n:]

            window_latencies.append((t1 - t0) / 1000.0)
            total_events += BATCH_SIZE
            batch_idx += 1

            if batch_idx % 50 == 0:
                total_matches += len(bindings.poll_signals(max_count=4096))

            if warm_sample is None and total_events >= warm_threshold_events:
                rss = get_rss_bytes()
                warm_sample = RssSample(elapsed, rss)
                rss_samples.append(warm_sample)
                print(f"  [{elapsed:6.1f}s] Warm baseline reached ({total_events:,} events, "
                      f">= 2x ring capacity) | RSS: {fmt_mb(rss)}")

            if elapsed >= next_rss_sample_at:
                rss = get_rss_bytes()
                rss_samples.append(RssSample(elapsed, rss))
                print(f"  [{elapsed:6.1f}s] {total_events:>12,} events | RSS: {fmt_mb(rss)}")
                next_rss_sample_at += RSS_SAMPLE_INTERVAL_S

            if elapsed - window_start_s >= LATENCY_WINDOW_S:
                windows.append(summarize_window(window_idx, window_start_s, elapsed, window_latencies, BATCH_SIZE))
                window_idx += 1
                window_start_s = elapsed
                window_latencies = []

            sleep_s = batch_interval_s - (time.perf_counter() - t_batch_start)
            if sleep_s > 0:
                time.sleep(sleep_s)

        final_elapsed = time.perf_counter() - t_start
        if window_latencies:
            windows.append(summarize_window(window_idx, window_start_s, final_elapsed, window_latencies, BATCH_SIZE))

        bindings.stop_logging()
        total_matches += len(bindings.poll_signals(max_count=1_000_000))

        gc.collect()
        time.sleep(0.05)
        final_rss = get_rss_bytes()
        rss_samples.append(RssSample(final_elapsed, final_rss))

        written_bytes = os.path.getsize(log_path) if os.path.exists(log_path) else 0

    expected_bytes = total_events * PAYLOAD_SIZE_BYTES
    persistence_ok = written_bytes == expected_bytes

    print(f"\nFinal RSS (after stop_logging + gc.collect): {fmt_mb(final_rss)}")
    print(f"Total events pushed: {total_events:,} over {final_elapsed:.1f}s "
          f"({total_events / final_elapsed:,.0f} events/sec)")
    print(f"Threat signals matched and drained: {total_matches:,}")
    print(f"Persistence integrity: {'OK' if persistence_ok else 'MISMATCH'} "
          f"({written_bytes:,} / {expected_bytes:,} bytes)")

    if warm_sample is None:
        warm_sample = rss_samples[0]
    max_rss = max(s.rss_bytes for s in rss_samples)
    rss_growth = final_rss - warm_sample.rss_bytes
    rss_growth_pct = (rss_growth / warm_sample.rss_bytes * 100) if warm_sample.rss_bytes else 0.0
    memory_stable = rss_growth_pct <= RSS_GROWTH_FLAG_PCT

    print(f"\nWarm baseline RSS: {fmt_mb(warm_sample.rss_bytes)} (at {warm_sample.elapsed_s:.1f}s)")
    print(f"Max RSS observed during run: {fmt_mb(max_rss)}")
    print(f"RSS growth, warm baseline -> final: {fmt_mb(rss_growth)} ({rss_growth_pct:+.2f}%)")
    print(f"Memory stability: {'PASS' if memory_stable else f'FLAG (> {RSS_GROWTH_FLAG_PCT:.0f}% growth)'}")

    print("\n--- Per-window latency (drift check) ---")
    header = f"{'Window':>8}{'Time (s)':>16}{'Calls':>10}{'Mean (us)':>12}{'p50 (us)':>12}{'p99 (us)':>12}{'Max (us)':>12}"
    print(header)
    print("-" * len(header))
    for w in windows:
        print(f"{w.window_idx:>8}{f'{w.start_s:.0f}-{w.end_s:.0f}':>16}{w.num_calls:>10,}"
              f"{w.mean_us:>12.2f}{w.p50_us:>12.2f}{w.p99_us:>12.2f}{w.max_us:>12.2f}")

    # Skip the first window (ramp-up/page-commit warm-up, same reasoning as
    # Phase 12's warm RSS baseline) and the last (may be a short, partial
    # window) when picking the drift baseline.
    stable_windows = windows[1:-1] if len(windows) > 2 else windows
    baseline_p99 = stable_windows[0].p99_us if stable_windows else float("nan")
    worst_window = max(windows, key=lambda w: w.p99_us) if windows else None
    drift_pct = ((worst_window.p99_us - baseline_p99) / baseline_p99 * 100) if worst_window and baseline_p99 else 0.0
    latency_stable = drift_pct <= LATENCY_DRIFT_FLAG_PCT

    # Approximates the whole-run p99 from each window's own p99 weighted by
    # its call count -- exact per-call percentiles aren't retained past each
    # window's summary (only its stats are kept), and this is close enough
    # for a headline number; the per-window table above is the real source
    # of truth for anything more precise.
    overall_p99 = percentile(sorted(l for w in windows for l in ([w.p99_us] * w.num_calls)), 99) if windows else float("nan")

    print(f"\nFirst-stable-window p99: {baseline_p99:.2f} us "
          f"(window {stable_windows[0].window_idx if stable_windows else 'n/a'})")
    if worst_window:
        print(f"Worst window p99: {worst_window.p99_us:.2f} us (window {worst_window.window_idx}, "
              f"{worst_window.start_s:.0f}-{worst_window.end_s:.0f}s) -- "
              f"drift {drift_pct:+.1f}% vs. first-stable-window baseline")
    print(f"P99 latency persistence: {'PASS' if latency_stable else f'FLAG (> {LATENCY_DRIFT_FLAG_PCT:.0f}% drift)'}")

    print("\n--- Zero dynamic allocation (proxy check) ---")
    print(
        "This engine's ring buffers and persistence batch buffer are sized once at "
        "construction (animus.hpp: LockFreeRingBuffer's backing std::vector<Cell>, "
        "reserve()'d + clear()'d, never reallocated per drain -- see BENCHMARKS.md "
        "Phase 12) -- there is no native API that counts allocations directly, so "
        "this is the same proxy Phase 12 used: a flat RSS curve after warm-up over "
        "the full run is what a zero-per-event-allocation hot path looks like from "
        "the outside; a real per-event leak would show up as RSS climbing "
        "monotonically with events processed instead of plateauing."
    )
    zero_alloc_consistent = memory_stable
    print(f"Zero dynamic allocation (RSS-plateau proxy): "
          f"{'CONSISTENT' if zero_alloc_consistent else 'INCONSISTENT -- investigate'}")

    overall_pass = memory_stable and latency_stable and persistence_ok

    print("\n" + "=" * 100)
    print(f"  SOAK TEST {'PASS' if overall_pass else 'FAIL'}")
    print("=" * 100)

    report = {
        "duration_s": final_elapsed,
        "batch_size": BATCH_SIZE,
        "target_events_per_sec": TARGET_EVENTS_PER_SEC,
        "ring_capacity": RING_CAPACITY,
        "total_events": total_events,
        "total_batches": batch_idx,
        "throughput_events_per_sec": total_events / final_elapsed if final_elapsed else 0.0,
        "threat_signals_matched": total_matches,
        "persistence_integrity_ok": persistence_ok,
        "bytes_written": written_bytes,
        "bytes_expected": expected_bytes,
        "cold_baseline_rss_bytes": cold_rss,
        "post_init_rss_bytes": post_init_rss,
        "warm_baseline_rss_bytes": warm_sample.rss_bytes,
        "warm_baseline_at_s": warm_sample.elapsed_s,
        "final_rss_bytes": final_rss,
        "max_rss_bytes": max_rss,
        "rss_growth_bytes": rss_growth,
        "rss_growth_pct": rss_growth_pct,
        "memory_stable": memory_stable,
        "windows": [w.__dict__ for w in windows],
        "overall_p99_us": overall_p99,
        "first_stable_window_p99_us": baseline_p99,
        "worst_window_p99_us": worst_window.p99_us if worst_window else None,
        "worst_window_idx": worst_window.window_idx if worst_window else None,
        "latency_drift_pct": drift_pct,
        "latency_stable": latency_stable,
        "zero_alloc_consistent": zero_alloc_consistent,
        "overall_pass": overall_pass,
    }

    report_path = os.path.join(os.path.dirname(__file__), "SOAK_TEST_REPORT.json")
    with open(report_path, "w") as f:
        json.dump(report, f, indent=2)
    print(f"\nFull report saved to {report_path}")

    return report


def main() -> None:
    duration_s = float(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_DURATION_S
    run_soak_test(duration_s)


if __name__ == "__main__":
    main()
