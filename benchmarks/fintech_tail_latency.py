"""Tail-latency benchmark for the native batch ingestion API
(animus_record_events_batch, via AnimusBindings.record_events_batch),
framed the way a latency-sensitive fintech caller (order/market-data
ingestion, a risk check on the hot path) would actually care about it:
not just an average, but the shape of the tail -- p50 through p99.99.

For each of three batch sizes (100 / 1,000 / 10,000 events), this script
pushes exactly 1,000,000 events total through record_events_batch(),
timing *only the call itself* with time.perf_counter_ns() (nanosecond
resolution, reported in microseconds) -- batch construction happens
before the timer starts, so Python-side list-building never contaminates
the measured call latency.

Each batch size runs in its own fresh subprocess (`--run-sweep <n>`),
not sequentially in this process. Two reasons, both specific to tail
latency (not just an average, where this would matter less):

  1. The ring buffer is sized to hold the whole 1,000,000-event sweep, so
     no call ever blocks on a full buffer or needs draining -- every call
     measures the same thing. Reusing one process across sweeps would mean
     either resizing a buffer that can't be resized after animus_init(),
     or draining between sweeps, either of which risks leaving the
     allocator/ring in a different state for sweep 2 than sweep 1 started
     in -- exactly the kind of hard-to-see contamination that quietly
     skews a p99.99 without ever showing up in the mean.
  2. A fresh process means a fresh page-fault/allocator warm-up curve each
     time (see benchmarks/stress_test_engine.py's Part 1 for why that
     curve exists at all) -- consistent conditions across all three
     sweeps, not "sweep 1 pays the warm-up cost, sweeps 2 and 3 don't."

Run with:
    python benchmarks/fintech_tail_latency.py
"""
import json
import math
import os
import subprocess
import sys
import time
from dataclasses import dataclass
from typing import List

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from animus.bindings import AnimusBindings  # noqa: E402

TOTAL_EVENTS = 1_000_000
BATCH_SIZES = [100, 1_000, 10_000]
EVENT_ID = 1
SWEEP_TIMEOUT_S = 60.0


def percentile(sorted_data: List[float], pct: float) -> float:
    """Linear-interpolation percentile (matches numpy.percentile's default
    'linear' method) -- no numpy dependency, consistent with this SDK's
    zero-dependency stdlib-only design.
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


def run_sweep(batch_size: int, total_events: int) -> dict:
    """Runs exactly one batch-size sweep in this process. Only ever called
    from the --run-sweep child process path (see __main__ below) -- the
    parent process never calls this directly, to keep sweeps isolated
    (see the module docstring for why that matters for tail latency).
    """
    if total_events % batch_size != 0:
        raise ValueError(f"total_events ({total_events}) must be a multiple of batch_size ({batch_size})")
    num_calls = total_events // batch_size

    bindings = AnimusBindings()
    if not bindings.using_native_engine:
        raise RuntimeError(
            "no compiled native engine found -- this benchmark requires the real "
            "AnimusNative/AnimusCore_v1 binary, not the pure-Python fallback"
        )
    # Sized to hold the entire sweep so no call ever blocks on a full ring or
    # needs draining -- every one of the num_calls calls below measures the
    # same thing (a pure ring-buffer push), not "push, and sometimes also wait
    # for the persistence worker to make room."
    bindings.init(buffer_capacity=total_events)

    latencies_us: List[float] = []
    processed = 0

    for call_idx in range(num_calls):
        batch = [
            (EVENT_ID, processed + i, (processed + i) % 100)
            for i in range(batch_size)
        ]
        processed += batch_size

        t0 = time.perf_counter_ns()
        pushed = bindings.record_events_batch(batch)
        t1 = time.perf_counter_ns()

        if pushed != batch_size:
            raise RuntimeError(
                f"short push at call {call_idx}: pushed {pushed}/{batch_size} "
                "-- ring buffer should never fill given buffer_capacity=total_events"
            )

        latencies_us.append((t1 - t0) / 1000.0)

    latencies_us.sort()
    total_call_time_s = sum(latencies_us) / 1_000_000.0

    return {
        "batch_size": batch_size,
        "num_calls": num_calls,
        "total_events": total_events,
        "total_call_time_s": total_call_time_s,
        "mean_us": sum(latencies_us) / len(latencies_us),
        "min_us": latencies_us[0],
        "max_us": latencies_us[-1],
        "p50_us": percentile(latencies_us, 50),
        "p90_us": percentile(latencies_us, 90),
        "p99_us": percentile(latencies_us, 99),
        "p99_99_us": percentile(latencies_us, 99.99),
    }


@dataclass
class SweepSummary:
    batch_size: int
    num_calls: int
    throughput_eps: float
    mean_us: float
    p50_us: float
    p90_us: float
    p99_us: float
    p99_99_us: float
    p99_99_sample_note: str


def _p99_99_sample_note(num_calls: int) -> str:
    """p99.99 needs on the order of 10,000+ samples to land on a real data
    point rather than interpolating near the max -- flag it plainly instead
    of printing a falsely precise number with no comment.
    """
    if num_calls >= 10_000:
        return ""
    if num_calls >= 1_000:
        return "*low sample count for p99.99 -- close to interpolating the max"
    return "*p99.99 not meaningful at this sample count -- effectively the max"


def run_sweep_isolated(batch_size: int, total_events: int) -> SweepSummary:
    proc = subprocess.run(
        [sys.executable, __file__, "--run-sweep", str(batch_size)],
        capture_output=True, text=True, timeout=SWEEP_TIMEOUT_S,
    )
    if proc.returncode != 0:
        raise RuntimeError(
            f"sweep batch_size={batch_size} failed (exit {proc.returncode}):\n{proc.stderr}"
        )
    result_line = next(
        (line for line in proc.stdout.splitlines() if line.startswith("SWEEP_RESULT ")), None,
    )
    if result_line is None:
        raise RuntimeError(f"sweep batch_size={batch_size} produced no result:\n{proc.stdout}\n{proc.stderr}")
    data = json.loads(result_line[len("SWEEP_RESULT "):])

    throughput_eps = data["total_events"] / data["total_call_time_s"]
    return SweepSummary(
        batch_size=data["batch_size"],
        num_calls=data["num_calls"],
        throughput_eps=throughput_eps,
        mean_us=data["mean_us"],
        p50_us=data["p50_us"],
        p90_us=data["p90_us"],
        p99_us=data["p99_us"],
        p99_99_us=data["p99_99_us"],
        p99_99_sample_note=_p99_99_sample_note(data["num_calls"]),
    )


def print_summary(summaries: List[SweepSummary]) -> None:
    header = (
        f"{'Batch Size':<12}{'Calls':>10}{'Throughput (ev/s)':>20}"
        f"{'Mean (us)':>12}{'p50 (us)':>12}{'p90 (us)':>12}{'p99 (us)':>12}{'p99.99 (us)':>14}"
    )
    rule = "-" * len(header)

    print("=" * len(header))
    print("  ANIMUS FINTECH TAIL LATENCY: animus_record_events_batch")
    print(f"  {TOTAL_EVENTS:,} events per batch size, timed call-by-call (perf_counter_ns)")
    print("=" * len(header))
    print(header)
    print(rule)
    for s in summaries:
        print(
            f"{s.batch_size:<12,}{s.num_calls:>10,}{s.throughput_eps:>20,.0f}"
            f"{s.mean_us:>12.2f}{s.p50_us:>12.2f}{s.p90_us:>12.2f}{s.p99_us:>12.2f}{s.p99_99_us:>14.2f}"
        )
    print(rule)

    notes = [f"  {s.batch_size:,}: {s.p99_99_sample_note}" for s in summaries if s.p99_99_sample_note]
    if notes:
        print("\np99.99 sample-size caveats (statistical resolution, not a defect):")
        for note in notes:
            print(note)

    print(
        "\nThroughput is derived from summed call latencies alone (total_events / "
        "sum of per-call times), not sweep wall-clock time -- it isolates the "
        "native batch API's own throughput from Python-side event-list "
        "construction, which happens before each call's timer starts and is "
        "deliberately excluded from every number in this table."
    )


def main() -> None:
    summaries = [run_sweep_isolated(bs, TOTAL_EVENTS) for bs in BATCH_SIZES]
    print_summary(summaries)


if __name__ == "__main__":
    if len(sys.argv) >= 3 and sys.argv[1] == "--run-sweep":
        result = run_sweep(int(sys.argv[2]), TOTAL_EVENTS)
        print("SWEEP_RESULT " + json.dumps(result))
    else:
        main()
