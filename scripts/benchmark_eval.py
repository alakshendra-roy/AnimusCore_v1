#!/usr/bin/env python3
"""External evaluation harness -- reproduce Animus Core's Python SDK
batched-ingestion throughput and tail-latency numbers on your own hardware.

This measures the SAME layer BENCHMARK_DATASHEET.md Sec 2 "Python SDK
batched-ingestion throughput" reports (representative run: 6,891,485
events/sec, p50 13.70us, p99 27.60us at batch size 100, source:
AnimusCore_v1/BENCHMARKS.md Phase 13 / benchmarks/fintech_tail_latency.py).
It does NOT touch proprietary engine source or internals -- it drives the
engine exclusively through the same public C-ABI every other script in
this repository uses (animus.bindings.AnimusBindings -> ctypes ->
AnimusNative.dll / libanimus_native.so), the identical path documented in
BENCHMARK_DATASHEET.md Sec 4 "Client Integration Quickstart". No license
is required: this script never calls a licensed feature (CPU core
pinning, the SPSC-pinned path) -- see docs/EVALUATION_GUIDE.md for why
that layer is out of scope for a free, unlicensed evaluation and how to
get it evaluated properly.

For the native, RDTSC-resolution, cross-core SPSC transport tail-latency
numbers (p99 64.9ns) instead of this Python-level number, compile and run
benchmarks/telemetry_benchmark.cpp directly -- a separate, self-contained
C++ file with its own benchmark-only SPSC ring, not this script. Both
paths are covered in docs/EVALUATION_GUIDE.md; conflating the two
methodology layers is treated as an error throughout this repository's
own benchmark culture (see BENCHMARK_DATASHEET.md Sec 2's methodology
note), and this script deliberately doesn't try to produce both from one
run.

Methodology (matches benchmarks/fintech_tail_latency.py's baseline path):
  - The ring buffer is sized to hold the entire run (rounded up to a
    power of two), so no call ever blocks on a full buffer -- every
    sample measures the same thing, not queueing backlog behind a drain.
  - Each animus_record_events_batch call is timed individually with
    time.perf_counter_ns() around the call itself; batch-list
    construction happens before the timer starts, so Python-side list-
    building never contaminates the measured call latency.
  - Throughput = total events pushed / total time spent inside those
    calls (wall-clock time between calls, e.g. building the next batch,
    is excluded -- this is call-path throughput, not process throughput).

Usage:
    python scripts/benchmark_eval.py
    python scripts/benchmark_eval.py --batch-size 1000 --total-events 1000000
    python scripts/benchmark_eval.py --batch-size 100 --json-out results_batch100.json

Run it more than once, and at more than one --batch-size (100 / 1,000 /
10,000 covers the same sweep the datasheet's figures were drawn from) --
a single run on a general-purpose, non-real-time OS is one sample, not a
verified number. See docs/EVALUATION_GUIDE.md for the full walkthrough.
"""
import argparse
import json
import platform
import statistics
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from animus.bindings import AnimusBindings  # noqa: E402

# Published reference figures, BENCHMARK_DATASHEET.md Sec 2 "Python SDK
# batched-ingestion throughput" -- the representative run cited there was
# taken at batch size 100. These are printed for orientation only; they
# are not a pass/fail gate -- per-hardware, per-run variance is expected
# and is exactly what this script exists to measure honestly, not paper
# over (same stance BENCHMARK_DATASHEET.md itself takes).
REFERENCE_BATCH_100 = {
    "throughput_events_per_sec": 6_891_485,
    "p50_us": 13.70,
    "p99_us": 27.60,
}

EVENT_ID = 1  # arbitrary -- no rule is registered, so no event_id matters for this measurement


def next_power_of_two(n: int) -> int:
    p = 1
    while p < n:
        p <<= 1
    return p


def percentile(sorted_samples: "list[float]", pct: float) -> float:
    if not sorted_samples:
        return float("nan")
    k = (len(sorted_samples) - 1) * (pct / 100.0)
    lo = int(k)
    hi = min(lo + 1, len(sorted_samples) - 1)
    if lo == hi:
        return sorted_samples[lo]
    return sorted_samples[lo] + (sorted_samples[hi] - sorted_samples[lo]) * (k - lo)


def run_sweep(batch_size: int, total_events: int, ring_capacity: int) -> "list[int]":
    """Returns one call-latency sample (nanoseconds) per batch."""
    bindings = AnimusBindings()

    if not bindings.using_native_engine:
        raise SystemExit(
            "No compiled native engine found -- this run would measure the "
            "pure-Python reference fallback, not the real transport, and "
            "would not be a valid reproduction of the published numbers.\n"
            "Build the native engine first:\n"
            "  cmake -S . -B build && cmake --build build          (portable)\n"
            "  -- or --\n"
            "  open AnimusCore_v1.slnx in Visual Studio and build   (Windows/MSVC)\n"
            "-- or install a wheel that bundles it for your platform.\n"
            "See docs/EVALUATION_GUIDE.md."
        )

    if not bindings.init(buffer_capacity=ring_capacity):
        raise SystemExit("animus_init failed")

    if total_events % batch_size != 0:
        raise SystemExit(
            f"--total-events ({total_events:,}) must be a multiple of "
            f"--batch-size ({batch_size:,})."
        )
    num_batches = total_events // batch_size

    latencies_ns = []
    total_pushed = 0

    for batch_idx in range(num_batches):
        base = batch_idx * batch_size
        events = [(EVENT_ID, base + i, i % 150) for i in range(batch_size)]

        t0 = time.perf_counter_ns()
        pushed = bindings.record_events_batch(events)
        t1 = time.perf_counter_ns()

        if pushed < len(events):
            raise SystemExit(
                f"Ring buffer filled mid-sweep after {total_pushed:,} events "
                f"(--ring-capacity={ring_capacity:,}). This should not happen "
                "with the default sizing -- pass a larger --ring-capacity, or "
                "check for another process holding the engine's ring open."
            )

        latencies_ns.append(t1 - t0)
        total_pushed += pushed

    assert total_pushed == total_events
    return latencies_ns


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Reproduce Animus Core's Python SDK batched-ingestion "
                     "throughput and tail-latency numbers on this machine."
    )
    parser.add_argument(
        "--batch-size", type=int, default=100,
        help="Events per animus_record_events_batch call. Default: 100 "
             "(matches the datasheet's representative run). Try 1000 and "
             "10000 too -- batch size materially changes both numbers.",
    )
    parser.add_argument(
        "--total-events", type=int, default=1_000_000,
        help="Total events pushed across the whole run. Default: 1,000,000 "
             "(matches benchmarks/fintech_tail_latency.py's methodology).",
    )
    parser.add_argument(
        "--ring-capacity", type=int, default=None,
        help="Ring buffer capacity. Default: next power of two >= "
             "--total-events, so no call ever blocks on a full buffer "
             "and every sample measures the same thing.",
    )
    parser.add_argument(
        "--json-out", type=str, default=None,
        help="Optional path to write the full results (including all raw "
             "per-batch latency samples) as JSON.",
    )
    args = parser.parse_args()

    if args.batch_size <= 0 or args.total_events <= 0:
        raise SystemExit("--batch-size and --total-events must both be positive.")

    ring_capacity = args.ring_capacity or next_power_of_two(args.total_events)
    if ring_capacity < args.total_events:
        raise SystemExit("--ring-capacity must be >= --total-events for this script's methodology to hold.")

    print("=" * 78)
    print("  ANIMUS CORE -- EXTERNAL EVALUATION: SDK BATCHED-INGESTION BENCHMARK")
    print("=" * 78)
    print(f"Platform:       {platform.platform()}")
    print(f"Python:         {platform.python_version()}")
    print(f"Batch size:     {args.batch_size:,}")
    print(f"Total events:   {args.total_events:,}")
    print(f"Ring capacity:  {ring_capacity:,}")
    print()

    latencies_ns = run_sweep(args.batch_size, args.total_events, ring_capacity)

    latencies_us_sorted = sorted(ns / 1000.0 for ns in latencies_ns)
    total_call_time_s = sum(latencies_ns) / 1e9
    throughput_events_per_sec = args.total_events / total_call_time_s

    p50 = percentile(latencies_us_sorted, 50)
    p90 = percentile(latencies_us_sorted, 90)
    p99 = percentile(latencies_us_sorted, 99)
    p999 = percentile(latencies_us_sorted, 99.9)
    mean = statistics.mean(latencies_us_sorted)

    print(f"Throughput:     {throughput_events_per_sec:,.0f} events/sec")
    print(f"Call latency (per animus_record_events_batch call, {args.batch_size:,} events/call):")
    print(f"  mean:  {mean:.2f} us")
    print(f"  p50:   {p50:.2f} us")
    print(f"  p90:   {p90:.2f} us")
    print(f"  p99:   {p99:.2f} us")
    print(f"  p99.9: {p999:.2f} us")

    if args.batch_size == 100:
        ref = REFERENCE_BATCH_100
        print()
        print("Published reference (batch size 100, BENCHMARK_DATASHEET.md Sec 2):")
        print(f"  throughput: {ref['throughput_events_per_sec']:,} events/sec  "
              f"(this run: {throughput_events_per_sec:,.0f})")
        print(f"  p50:        {ref['p50_us']:.2f} us  (this run: {p50:.2f} us)")
        print(f"  p99:        {ref['p99_us']:.2f} us  (this run: {p99:.2f} us)")
        print("  Per-hardware, per-run variance is expected -- this comparison is "
              "for orientation, not a pass/fail gate.")

    if args.json_out:
        payload = {
            "platform": platform.platform(),
            "python_version": platform.python_version(),
            "batch_size": args.batch_size,
            "total_events": args.total_events,
            "ring_capacity": ring_capacity,
            "throughput_events_per_sec": throughput_events_per_sec,
            "call_latency_us": {
                "mean": mean, "p50": p50, "p90": p90, "p99": p99, "p99_9": p999,
            },
            "raw_call_latencies_ns": latencies_ns,
        }
        Path(args.json_out).write_text(json.dumps(payload, indent=2))
        print(f"\nFull results (including raw samples) written to {args.json_out}")


if __name__ == "__main__":
    main()
