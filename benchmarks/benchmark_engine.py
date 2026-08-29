"""Benchmarks AnimusNative (the compiled C++ engine, via animus.bindings)
against an equivalent pure-Python dictionary-loop implementation.

Four scenarios are measured:

  1. AnimusNative, full pipeline: record_event() pushes onto the real
     LockFreeRingBuffer; a background worker thread (started via
     start_logging) drains it in batches, evaluates the registered
     threshold rule, and flushes each batch to disk. stop_logging() blocks
     until that drain is fully complete, so the measured window covers the
     entire ingest -> evaluate -> persist pipeline.
  2. Pure-Python dict loop: a plain `for` loop builds an event dict, looks
     its rule up in a `dict` keyed by event_id, evaluates the threshold
     inline, and appends to a `list` on a match -- no ctypes, no native
     calls, no disk I/O.
  3. AnimusNative, ring-buffer ingestion only: the same record_event() loop
     as (1), but with no persistence worker running -- no rule evaluation,
     no disk I/O. Isolates the raw cost of the ctypes call boundary plus
     one atomic ring-buffer push per event, i.e. the "zero-copy hot path"
     a real caller with its own evaluation/persistence would actually hit.
  4. AnimusNative, batched ingestion: the same work as (3), but through
     animus_record_events_batch -- one ctypes call carrying every event,
     rather than one call per event. Isolates per-call ctypes marshalling
     overhead from the ring-buffer push cost itself: comparing (3) and (4)
     shows how much of (3)'s time is FFI-boundary tax versus actual native
     work.

(1) and (2) do the same logical work per event, so that pair isolates
native's batched-disk-I/O-and-evaluation cost against an in-memory Python
equivalent. (3) isolates native's per-event ingestion primitive alone,
with the disk I/O that dominates (1) removed. (4) then removes the
remaining per-call ctypes overhead that (3) still pays 100,000 times over.

Run with:
    python benchmarks/benchmark_engine.py
"""
import os
import sys
import tempfile
import time
from dataclasses import dataclass
from typing import List, Optional

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from animus.bindings import AnimusBindings, RuleComparator  # noqa: E402

TOTAL_EVENTS = 100_000
RULE_EVENT_ID = 1
RULE_THRESHOLD = 90  # metric_value = i % 100, so ~9% of events match (91-99)
RULE_SEVERITY = 5


@dataclass
class BenchResult:
    name: str
    total_events: int
    elapsed_s: float
    matches: Optional[int]  # None where no rule evaluation occurred (see run_native_ingest_only)

    @property
    def latency_us(self) -> float:
        return (self.elapsed_s * 1_000_000) / self.total_events

    @property
    def throughput_eps(self) -> float:
        return self.total_events / self.elapsed_s


def run_native_engine(total_events: int) -> BenchResult:
    bindings = AnimusBindings()
    if not bindings.using_native_engine:
        print(
            "WARNING: no compiled AnimusNative/AnimusCore_v1 binary found -- "
            "this run is actually exercising the pure-Python fallback engine, "
            "not the native C++ core. Build CMakeLists.txt or "
            "AnimusCore_v1.slnx first for a real comparison.\n"
        )

    bindings.init(buffer_capacity=total_events)
    bindings.add_rule(
        rule_id=1,
        event_id=RULE_EVENT_ID,
        threshold=RULE_THRESHOLD,
        comparator=RuleComparator.GREATER_THAN,
        severity=RULE_SEVERITY,
    )

    with tempfile.TemporaryDirectory() as tmp:
        log_path = os.path.join(tmp, "benchmark_telemetry.log")

        t0 = time.perf_counter()
        bindings.start_logging(log_path)
        for i in range(total_events):
            bindings.record_event(event_id=RULE_EVENT_ID, trace_id=i, metric_value=i % 100)
        bindings.stop_logging()  # blocks until the ring buffer is fully drained
        elapsed = time.perf_counter() - t0

        matches = 0
        while True:
            batch = bindings.poll_signals(max_count=4096)
            if not batch:
                break
            matches += len(batch)

    return BenchResult(name="AnimusNative (C++)", total_events=total_events, elapsed_s=elapsed, matches=matches)


def run_native_ingest_only(total_events: int) -> BenchResult:
    """Same record_event() loop as run_native_engine(), but never starts the
    persistence worker: no rule evaluation, no disk I/O. animus_init() is
    idempotent on the native singleton (see animus_engine.cpp), and the
    ring buffer left empty by run_native_engine()'s stop_logging() drain is
    reused here, so this measures nothing but the ctypes call boundary plus
    one atomic ring-buffer push per event.
    """
    bindings = AnimusBindings()
    bindings.init(buffer_capacity=total_events)
    # add_rule() is harmless to repeat (native side just appends another
    # threshold entry); it's never evaluated since no worker is running.
    bindings.add_rule(
        rule_id=1,
        event_id=RULE_EVENT_ID,
        threshold=RULE_THRESHOLD,
        comparator=RuleComparator.GREATER_THAN,
        severity=RULE_SEVERITY,
    )

    t0 = time.perf_counter()
    for i in range(total_events):
        bindings.record_event(event_id=RULE_EVENT_ID, trace_id=i, metric_value=i % 100)
    elapsed = time.perf_counter() - t0

    # Untimed: drain the ring buffer so the next scenario, which reuses the
    # same native singleton, starts from empty rather than immediately full.
    with tempfile.TemporaryDirectory() as tmp:
        bindings.start_logging(os.path.join(tmp, "drain.log"))
        bindings.stop_logging()

    return BenchResult(
        name="AnimusNative (ring buffer only)",
        total_events=total_events,
        elapsed_s=elapsed,
        matches=None,
    )


def run_native_batch_ingest(total_events: int) -> BenchResult:
    """Same raw ingestion as run_native_ingest_only(), but through the
    batched animus_record_events_batch entry point: the whole event list
    is built in Python first, then handed to native in a single ctypes
    call, so the per-call marshalling cost is paid once instead of
    total_events times.
    """
    bindings = AnimusBindings()
    bindings.init(buffer_capacity=total_events)

    events = [(RULE_EVENT_ID, i, i % 100) for i in range(total_events)]

    t0 = time.perf_counter()
    pushed = bindings.record_events_batch(events)
    elapsed = time.perf_counter() - t0
    assert pushed == total_events, f"batch push short by {total_events - pushed} events"

    return BenchResult(
        name="AnimusNative (batched ingestion)",
        total_events=total_events,
        elapsed_s=elapsed,
        matches=None,
    )


def run_python_dict_loop(total_events: int) -> BenchResult:
    rules = {
        RULE_EVENT_ID: {
            "rule_id": 1,
            "threshold": RULE_THRESHOLD,
            "severity": RULE_SEVERITY,
        }
    }
    processed_log: List[dict] = []
    signals: List[dict] = []

    t0 = time.perf_counter()
    for i in range(total_events):
        event = {
            "event_id": RULE_EVENT_ID,
            "trace_id": i,
            "metric_value": i % 100,
            "timestamp_ns": time.perf_counter_ns(),
        }
        rule = rules.get(event["event_id"])
        if rule is not None and event["metric_value"] > rule["threshold"]:
            signals.append({
                "timestamp_cycles": event["timestamp_ns"],
                "event_id": event["event_id"],
                "trace_id": event["trace_id"],
                "metric_value": event["metric_value"],
                "rule_id": rule["rule_id"],
                "severity": rule["severity"],
            })
        processed_log.append(event)
    elapsed = time.perf_counter() - t0

    return BenchResult(name="Pure-Python dict loop", total_events=total_events, elapsed_s=elapsed, matches=len(signals))


def print_summary(
    native_full: BenchResult,
    python: BenchResult,
    native_ingest: BenchResult,
    native_batch: BenchResult,
) -> None:
    def speedup(a: BenchResult, b: BenchResult) -> float:
        return b.elapsed_s / a.elapsed_s if a.elapsed_s > 0 else float("inf")

    header = f"{'Engine':<34}{'Time (ms)':>12}{'Latency (us/ev)':>18}{'Throughput (ev/s)':>20}{'Matches':>10}"
    rule = "-" * len(header)

    print("=" * len(header))
    print("  ANIMUS ENGINE BENCHMARK: Native (C++) vs. Pure-Python Dict Loop")
    print(f"  Events per run: {TOTAL_EVENTS:,}")
    print("=" * len(header))
    print(header)
    print(rule)
    for r in (native_full, python, native_ingest, native_batch):
        matches_str = str(r.matches) if r.matches is not None else "n/a"
        print(
            f"{r.name:<34}"
            f"{r.elapsed_s * 1000:>12.2f}"
            f"{r.latency_us:>18.3f}"
            f"{r.throughput_eps:>20,.0f}"
            f"{matches_str:>10}"
        )
    print(rule)
    print(f"Native (full pipeline) speedup over pure-Python:        {speedup(native_full, python):,.2f}x")
    print(f"Native (ring buffer only) speedup over pure-Python:     {speedup(native_ingest, python):,.2f}x")
    print(f"Native (batched ingestion) speedup over pure-Python:    {speedup(native_batch, python):,.2f}x")
    print(f"Native (batched) speedup over native (ring buffer only):{speedup(native_batch, native_ingest):>7,.2f}x")
    print("=" * len(header))
    print(
        "\nNote: 'ring buffer only' and 'batched ingestion' never start the\n"
        "persistence worker, so no rule evaluation or disk I/O happens on\n"
        "those runs -- 'ring buffer only' still pays the ctypes call cost once\n"
        "per event (100,000 calls), while 'batched ingestion' pays it once for\n"
        "the whole run via animus_record_events_batch, isolating per-call FFI\n"
        "overhead from the batched-disk-flush cost that dominates the full\n"
        "pipeline above."
    )


def main() -> None:
    native_full_result = run_native_engine(TOTAL_EVENTS)
    python_result = run_python_dict_loop(TOTAL_EVENTS)
    native_ingest_result = run_native_ingest_only(TOTAL_EVENTS)
    native_batch_result = run_native_batch_ingest(TOTAL_EVENTS)
    print_summary(native_full_result, python_result, native_ingest_result, native_batch_result)


if __name__ == "__main__":
    main()
