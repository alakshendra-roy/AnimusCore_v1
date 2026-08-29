"""Tail-latency benchmark for the native batch ingestion API, comparing two
producer paths across three batch sizes (100 / 1,000 / 10,000 events),
each pushing exactly 1,000,000 events total, timing *only the call
itself* with time.perf_counter_ns() (nanosecond resolution, reported in
microseconds) -- batch construction happens before the timer starts, so
Python-side list-building never contaminates the measured call latency.

  - Baseline: animus_record_events_batch (AnimusBindings.record_events_batch),
    the general-purpose MPMC ring, unpinned. This is the same call and the
    same numbers as this script measured before the SPSC/pinning work
    below existed -- kept as-is so Phase 13's methodology and results
    stay valid as the "before" side of the comparison.
  - SPSC + pinned: animus_spsc_record_events_batch
    (AnimusBindings.spsc_record_events_batch) against the standalone
    lock-free single-producer/single-consumer ring (animus::SpscRingBuffer,
    animus.hpp), from a thread pinned to a single CPU core via
    animus_pin_current_thread_to_core before the timed loop starts. The
    SPSC ring trades MPMC's compare-exchange retry loop for a plain
    atomic load/store pair (no contention to retry against with only one
    producer, by construction).

What pinning actually buys here, measured, not assumed: with a good core
selected (see below), mean/p50/p90/p99 improve consistently and clearly
over the unpinned baseline. p99.99 does NOT reliably improve, and often
gets *worse* -- sometimes dramatically -- even with a good core, measured
across repeated runs. This is a real result, not a bug in this script:
SetThreadAffinityMask pins a thread to a core, it does not reserve that
core exclusively (that needs OS-level isolation -- Linux's isolcpus/
nohz_full, or Windows' CPU Sets reserved-exclusively mode, neither of
which this benchmark sets up, and neither of which a general-purpose
laptop's OS/background-service load is a realistic target for anyway). An
unpinned thread that hits contention can migrate to any idle core; a
pinned thread has nowhere to go until its one core frees up -- which
specifically inflates the rare, worst-case tail even as cache locality
improves the common case. Report both effects, not just the one that
matches what "pin the hot thread" is supposed to do.

Each (batch size, variant) pair runs in its own fresh subprocess, not
sequentially in this process. Two reasons, both specific to tail latency
(not just an average, where this would matter less):

  1. Each ring is sized to hold the whole 1,000,000-event sweep, so no
     call ever blocks on a full buffer or needs draining -- every call
     measures the same thing. Reusing one process across sweeps would mean
     either resizing a buffer that can't be resized after init, or
     draining between sweeps, either of which risks leaving the
     allocator/ring in a different state for sweep 2 than sweep 1 started
     in -- exactly the kind of hard-to-see contamination that quietly
     skews a p99.99 without ever showing up in the mean.
  2. A fresh process means a fresh page-fault/allocator warm-up curve each
     time (see benchmarks/stress_test_engine.py's Part 1 for why that
     curve exists at all) -- consistent conditions across every sweep, not
     "sweep 1 pays the warm-up cost, the rest don't."

Which core to pin to is *probed*, not guessed. The first version of this
script pinned to the highest-numbered logical core (a common "avoid core
0" convention) -- on a machine with a hybrid P-core/E-core CPU (the one
this was developed on: Intel i7-14650HX, 16 cores/24 threads), that
convention silently picked an Efficiency core, and SPSC + pinned came out
2-33x *worse* on p99.99 than the unpinned baseline, not better. There is
no portable, vendor-neutral way to ask the OS "which logical cores are
P-cores" from C++ -- animus_pin_current_thread_to_core only pins, it
can't tell you where it's a good idea to. find_best_core() below probes a
handful of candidate cores with a small, cheap workload before the real
sweeps run, and picks whichever one actually measured the lowest p99, so
the "SPSC + pinned" numbers this script reports are never resting on that
same wrong assumption.

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
from typing import List, Optional

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from animus.bindings import AnimusBindings  # noqa: E402

TOTAL_EVENTS = 1_000_000
BATCH_SIZES = [100, 1_000, 10_000]
EVENT_ID = 1
SWEEP_TIMEOUT_S = 60.0
BASELINE = "baseline"
SPSC_PINNED = "spsc_pinned"


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


def _summarize(batch_size: int, num_calls: int, total_events: int, latencies_us: List[float]) -> dict:
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


def run_sweep(batch_size: int, total_events: int) -> dict:
    """Baseline: animus_record_events_batch against the general-purpose
    MPMC ring, unpinned. Only ever called from the --run-sweep child
    process path (see __main__ below) -- the parent process never calls
    this directly, to keep sweeps isolated (see the module docstring for
    why that matters for tail latency). Unchanged from before the SPSC/
    pinning work below existed, so it remains the "before" side of that
    comparison.
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

    return _summarize(batch_size, num_calls, total_events, latencies_us)


def run_sweep_spsc_pinned(batch_size: int, total_events: int, core_id: int) -> dict:
    """SPSC + pinned: pins this thread to `core_id`, then pushes through
    animus_spsc_record_events_batch against the standalone SPSC ring
    (AnimusBindings.spsc_record_events_batch) instead of the MPMC Engine
    ring run_sweep() above uses. Same timing discipline as run_sweep():
    batch construction happens before each call's timer starts. Only ever
    called from the --run-sweep-spsc-pinned child process path.
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

    pinned = bindings.pin_current_thread_to_core(core_id)
    if not pinned:
        raise RuntimeError(
            f"failed to pin the producer thread to core {core_id} -- refusing to "
            "report SPSC numbers as \"pinned\" when they aren't"
        )

    bindings.spsc_init(buffer_capacity=total_events)

    latencies_us: List[float] = []
    processed = 0

    for call_idx in range(num_calls):
        batch = [
            (EVENT_ID, processed + i, (processed + i) % 100)
            for i in range(batch_size)
        ]
        processed += batch_size

        t0 = time.perf_counter_ns()
        pushed = bindings.spsc_record_events_batch(batch)
        t1 = time.perf_counter_ns()

        if pushed != batch_size:
            raise RuntimeError(
                f"short push at call {call_idx}: pushed {pushed}/{batch_size} "
                "-- ring buffer should never fill given buffer_capacity=total_events"
            )

        latencies_us.append((t1 - t0) / 1000.0)

    result = _summarize(batch_size, num_calls, total_events, latencies_us)
    result["core_id"] = core_id
    return result


@dataclass
class SweepSummary:
    variant: str  # BASELINE or SPSC_PINNED
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


def _probe_core_p99(bindings: AnimusBindings, core_id: int, num_calls: int, batch_size: int) -> float:
    """Quick in-process probe: pins this thread to core_id, times num_calls
    small batch pushes, drains the ring afterward so the next candidate
    core's probe starts from an empty ring (the SPSC ring is a
    process-wide singleton -- see spsc_init's idempotent contract; without
    draining, probe 2 would inherit probe 1's leftover events and start
    seeing short pushes). Returns p99 latency in microseconds. This is a
    calibration step, not a reported benchmark result -- kept deliberately
    cheap (a few thousand events, not a million) since it runs once per
    candidate core before the real, isolated sweeps.
    """
    bindings.pin_current_thread_to_core(core_id)
    latencies_us: List[float] = []
    for i in range(num_calls):
        batch = [(EVENT_ID, i * batch_size + j, j) for j in range(batch_size)]
        t0 = time.perf_counter_ns()
        bindings.spsc_record_events_batch(batch)
        t1 = time.perf_counter_ns()
        latencies_us.append((t1 - t0) / 1000.0)
    while bindings.spsc_drain(max_count=100_000):
        pass  # empty the ring so the next candidate core's probe starts clean
    latencies_us.sort()
    return percentile(latencies_us, 99)


def find_best_core(cpu_count: int, num_calls: int = 50, batch_size: int = 200) -> "tuple[int, List[tuple]]":
    """Probes a handful of candidate cores spread across the logical core
    range and returns the one with the lowest measured p99 latency, plus
    every candidate's result for the caller to report. See the module
    docstring for why this exists instead of a fixed heuristic.
    """
    candidates = sorted({
        0, 1,
        max(1, cpu_count // 4),
        max(1, cpu_count // 2),
        max(1, (3 * cpu_count) // 4),
        max(0, cpu_count - 1),
    })
    candidates = [c for c in candidates if 0 <= c < cpu_count]

    bindings = AnimusBindings()
    bindings.spsc_init(buffer_capacity=1 << 16)

    results = [(core_id, _probe_core_p99(bindings, core_id, num_calls, batch_size)) for core_id in candidates]
    best_core_id, _ = min(results, key=lambda r: r[1])
    return best_core_id, results


def run_sweep_isolated(variant: str, batch_size: int, total_events: int, core_id: Optional[int] = None) -> SweepSummary:
    if variant == BASELINE:
        cmd = [sys.executable, __file__, "--run-sweep", str(batch_size)]
    elif variant == SPSC_PINNED:
        cmd = [sys.executable, __file__, "--run-sweep-spsc-pinned", str(batch_size), str(core_id)]
    else:
        raise ValueError(f"unknown variant {variant!r}")

    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=SWEEP_TIMEOUT_S)
    if proc.returncode != 0:
        raise RuntimeError(
            f"sweep variant={variant} batch_size={batch_size} failed (exit {proc.returncode}):\n{proc.stderr}"
        )
    result_line = next(
        (line for line in proc.stdout.splitlines() if line.startswith("SWEEP_RESULT ")), None,
    )
    if result_line is None:
        raise RuntimeError(
            f"sweep variant={variant} batch_size={batch_size} produced no result:\n{proc.stdout}\n{proc.stderr}"
        )
    data = json.loads(result_line[len("SWEEP_RESULT "):])

    throughput_eps = data["total_events"] / data["total_call_time_s"]
    return SweepSummary(
        variant=variant,
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


_VARIANT_LABELS = {BASELINE: "Baseline (MPMC, unpinned)", SPSC_PINNED: "SPSC + pinned"}


def print_summary(summaries: List[SweepSummary], core_id: int) -> None:
    header = (
        f"{'Batch Size':<12}{'Variant':<26}{'Calls':>10}{'Throughput (ev/s)':>20}"
        f"{'Mean (us)':>12}{'p50 (us)':>12}{'p90 (us)':>12}{'p99 (us)':>12}{'p99.99 (us)':>14}"
    )
    rule = "-" * len(header)

    print("=" * len(header))
    print("  ANIMUS FINTECH TAIL LATENCY: baseline vs. SPSC ring + CPU pinning")
    print(f"  {TOTAL_EVENTS:,} events per (batch size, variant), timed call-by-call (perf_counter_ns)")
    print(f"  SPSC + pinned producer thread pinned to logical core {core_id}")
    print("=" * len(header))
    print(header)
    print(rule)
    for s in summaries:
        print(
            f"{s.batch_size:<12,}{_VARIANT_LABELS[s.variant]:<26}{s.num_calls:>10,}{s.throughput_eps:>20,.0f}"
            f"{s.mean_us:>12.2f}{s.p50_us:>12.2f}{s.p90_us:>12.2f}{s.p99_us:>12.2f}{s.p99_99_us:>14.2f}"
        )
    print(rule)

    notes = [
        f"  {_VARIANT_LABELS[s.variant]}, batch={s.batch_size:,}: {s.p99_99_sample_note}"
        for s in summaries if s.p99_99_sample_note
    ]
    if notes:
        print("\np99.99 sample-size caveats (statistical resolution, not a defect):")
        for note in notes:
            print(note)

    def _delta(base_val: float, pinned_val: float) -> str:
        pct = (pinned_val - base_val) / base_val * 100.0 if base_val else float("nan")
        return f"{'-' if pct < 0 else '+'}{abs(pct):.1f}%"

    print("\nBaseline -> SPSC + pinned, by percentile (negative = improved):")
    by_key = {(s.variant, s.batch_size): s for s in summaries}
    for bs in BATCH_SIZES:
        base = by_key.get((BASELINE, bs))
        pinned = by_key.get((SPSC_PINNED, bs))
        if base is None or pinned is None:
            continue
        print(
            f"  batch={bs:,}:  "
            f"p50 {base.p50_us:.1f}->{pinned.p50_us:.1f}us ({_delta(base.p50_us, pinned.p50_us)})  "
            f"p90 {base.p90_us:.1f}->{pinned.p90_us:.1f}us ({_delta(base.p90_us, pinned.p90_us)})  "
            f"p99 {base.p99_us:.1f}->{pinned.p99_us:.1f}us ({_delta(base.p99_us, pinned.p99_us)})  "
            f"p99.99 {base.p99_99_us:.1f}->{pinned.p99_99_us:.1f}us ({_delta(base.p99_99_us, pinned.p99_99_us)})"
        )

    print(
        "\nExpect p50/p90/p99 to improve fairly consistently with a well-chosen\n"
        "core (cache locality, no per-call migration) -- p99.99 often does NOT\n"
        "improve, and can get markedly worse, because pinning alone (no OS-level\n"
        "core isolation) removes the scheduler's ability to move this thread off\n"
        "its one core when something else needs it, which specifically hurts the\n"
        "rare worst case even as it helps the common one. See this script's\n"
        "module docstring and BENCHMARKS.md's Phase 14 section."
    )

    print(
        "\nThroughput is derived from summed call latencies alone (total_events / "
        "sum of per-call times), not sweep wall-clock time -- it isolates each "
        "path's own throughput from Python-side event-list construction, which "
        "happens before each call's timer starts and is deliberately excluded "
        "from every number in this table."
    )


def main() -> None:
    probe = AnimusBindings()
    if not probe.using_native_engine:
        raise RuntimeError(
            "no compiled native engine found -- this benchmark requires the real "
            "AnimusNative/AnimusCore_v1 binary, not the pure-Python fallback"
        )
    cpu_count = probe.get_cpu_count()
    print(f"Detected {cpu_count} logical CPU(s). Probing candidate cores (small workload, p99 latency)...")
    core_id, probe_results = find_best_core(cpu_count)
    for cid, p99 in probe_results:
        marker = "  <- selected" if cid == core_id else ""
        print(f"  core {cid:>3}: p99 = {p99:8.2f} us{marker}")
    print()

    summaries: List[SweepSummary] = []
    for bs in BATCH_SIZES:
        summaries.append(run_sweep_isolated(BASELINE, bs, TOTAL_EVENTS))
        summaries.append(run_sweep_isolated(SPSC_PINNED, bs, TOTAL_EVENTS, core_id=core_id))
    print_summary(summaries, core_id)


if __name__ == "__main__":
    if len(sys.argv) >= 3 and sys.argv[1] == "--run-sweep":
        result = run_sweep(int(sys.argv[2]), TOTAL_EVENTS)
        print("SWEEP_RESULT " + json.dumps(result))
    elif len(sys.argv) >= 4 and sys.argv[1] == "--run-sweep-spsc-pinned":
        result = run_sweep_spsc_pinned(int(sys.argv[2]), TOTAL_EVENTS, int(sys.argv[3]))
        print("SWEEP_RESULT " + json.dumps(result))
    else:
        main()
