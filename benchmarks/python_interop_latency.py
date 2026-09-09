"""Amortized per-event latency of the Python zero-copy interop layer
(bindings/animus_py.cpp's nanobind TelemetryStream, wrapped by
animus/consumer.py's TelemetryConsumer) -- the source this repo's own
outreach material (docs/PILOT_PROGRAM.md SS4, docs/OUTREACH_TEMPLATES.md)
cites for its "~4.5 ns/event (amortized) drain() only" and "~560 ns/event
full decode" claims. Those two figures previously had no reproduction
command or benchmark artifact anywhere in this repo, unlike every other
number in this project's outreach material -- this script closes that gap
with a real, re-runnable measurement.

Two amortized costs, measured separately, matching the two rows those
docs already present:

  - "drain() only": TelemetryConsumer.drain() alone -- pops events out of
    the lock-free SPSC ring into a scratch buffer the C++ side owns, and
    returns a zero-copy buffer-protocol view over that same memory (see
    bindings/animus_py.cpp's TelemetryStream::drain doc comment). No
    per-event Python object is constructed on this path.
  - "full decode": drain() immediately followed by animus.consumer.decode()
    on the returned view -- one real `bytes(view)` copy (the view aliases
    a scratch buffer the next drain() call overwrites) plus a per-record
    struct.unpack, producing a real list[TelemetryRecord] a caller can
    hold onto past the next drain() call.

Methodology: a background native producer thread (TelemetryStream's own
start_producer, unthrottled) feeds the ring continuously while the main
Python thread drains it in a tight loop -- concurrent producer/consumer,
not a pre-filled-then-drained snapshot, so the numbers reflect steady-state
streaming the way a real caller would actually use this. Only the timed
region (drain(), or drain()+decode()) is measured with
time.perf_counter_ns() -- batch construction, producer startup, and
teardown are excluded, same discipline as benchmarks/fintech_tail_latency.py.
Timing happens once per batch (up to drain_batch_capacity events), not once
per event: at single-digit-to-low-hundreds-of-ns per event, a per-event
perf_counter_ns() pair (~50-100ns of overhead each on this interpreter)
would swamp the thing being measured -- "amortized" specifically means
"total batch time / events in that batch," matching how every other
per-event figure in this repo's benchmarks is derived.

Requires the compiled _animus_native nanobind extension (animus/consumer.py
raises a clear ImportError if it isn't built). Build it locally:
    pip install nanobind
    cmake -S bindings -B bindings/build -DPython_EXECUTABLE=<path to your python>
    cmake --build bindings/build --config Release
    # then copy the resulting _animus_native(.pyd|.so) into animus/, next
    # to consumer.py (see bindings/CMakeLists.txt's own build notes)

Run with:
    python benchmarks/python_interop_latency.py
"""
import statistics
import time
from typing import List

from animus.consumer import TelemetryConsumer, decode

EVENTS_PER_RUN = 2_000_000
# Must exceed EVENTS_PER_RUN, not just be "generous": the background
# producer is unthrottled and runs far ahead of the (much slower)
# full-decode consumer, so a ring sized close to EVENTS_PER_RUN fills
# completely before decode() catches up -- push() then silently drops the
# overflow (by design, see animus::SpscRingBuffer::push), which understates
# "events drained" against the run's target and looks like a hang/failure.
# Sizing the ring larger than the whole run's event count makes that
# structurally impossible: capacity can never be exhausted by a single
# bounded run, regardless of the producer/consumer speed ratio.
RING_CAPACITY = 1 << 22  # 4,194,304 -- > 2x EVENTS_PER_RUN
DRAIN_BATCH = 8192
NUM_RUNS = 5


def _drain_only_ns_per_event(events: int) -> float:
    """One full run: background producer feeds `events` events unthrottled;
    main thread drains in a tight loop, timing only drain() itself."""
    consumer = TelemetryConsumer(capacity=RING_CAPACITY, drain_batch_capacity=DRAIN_BATCH)
    consumer.start_synthetic_load(event_count=events, target_events_per_sec=0.0)
    try:
        drained = 0
        total_ns = 0
        while drained < events:
            t0 = time.perf_counter_ns()
            view = consumer.drain(DRAIN_BATCH)
            t1 = time.perf_counter_ns()
            n = view.shape[0]
            total_ns += (t1 - t0)
            drained += n
            if n == 0 and not consumer.producer_running and drained < events:
                raise RuntimeError(
                    f"producer stopped early: drained {drained}/{events} events, "
                    "ring empty and no producer running -- events were dropped "
                    "(ring too small for this producer/consumer speed ratio)"
                )
        return total_ns / drained
    finally:
        consumer.stop_synthetic_load()


def _full_decode_ns_per_event(events: int) -> float:
    """Same as above, but each drain() is immediately followed by decode()
    -- times drain()+decode() together, per batch."""
    consumer = TelemetryConsumer(capacity=RING_CAPACITY, drain_batch_capacity=DRAIN_BATCH)
    consumer.start_synthetic_load(event_count=events, target_events_per_sec=0.0)
    try:
        drained = 0
        total_ns = 0
        while drained < events:
            t0 = time.perf_counter_ns()
            view = consumer.drain(DRAIN_BATCH)
            records = decode(view)
            t1 = time.perf_counter_ns()
            n = len(records)
            total_ns += (t1 - t0)
            drained += n
            if n == 0 and not consumer.producer_running and drained < events:
                raise RuntimeError(
                    f"producer stopped early: drained {drained}/{events} events, "
                    "ring empty and no producer running -- events were dropped "
                    "(ring too small for this producer/consumer speed ratio)"
                )
        return total_ns / drained
    finally:
        consumer.stop_synthetic_load()


def _summarize(label: str, values_ns: List[float]) -> None:
    representative = values_ns[0]
    lo, hi = min(values_ns), max(values_ns)
    mean = statistics.mean(values_ns)
    print(f"{label}:")
    print(f"  representative run (run 1 of {len(values_ns)}): {representative:.2f} ns/event")
    print(f"  range across {len(values_ns)} runs: {lo:.2f} - {hi:.2f} ns/event (mean {mean:.2f} ns/event)")


def main() -> None:
    print("=" * 78)
    print("  ANIMUS PYTHON INTEROP LATENCY: drain() only vs. full decode")
    print(f"  {EVENTS_PER_RUN:,} events/run, {NUM_RUNS} runs, batch={DRAIN_BATCH:,}, "
          f"ring capacity={RING_CAPACITY:,}")
    print("  Concurrent unthrottled native producer + Python-side draining, "
          "timed per batch (perf_counter_ns)")
    print("=" * 78)

    drain_only_results = []
    for run in range(1, NUM_RUNS + 1):
        ns_per_event = _drain_only_ns_per_event(EVENTS_PER_RUN)
        drain_only_results.append(ns_per_event)
        print(f"  [drain-only]   run {run}/{NUM_RUNS}: {ns_per_event:.2f} ns/event")

    full_decode_results = []
    for run in range(1, NUM_RUNS + 1):
        ns_per_event = _full_decode_ns_per_event(EVENTS_PER_RUN)
        full_decode_results.append(ns_per_event)
        print(f"  [full-decode]  run {run}/{NUM_RUNS}: {ns_per_event:.2f} ns/event")

    print()
    _summarize("drain() only (zero-copy view, no per-event object)", drain_only_results)
    print()
    _summarize("full decode (drain() + animus.consumer.decode())", full_decode_results)
    print()
    print(
        "Note: 'full decode' includes drain() itself, not just the incremental decode\n"
        "cost -- this matches how docs/PILOT_PROGRAM.md SS4 and docs/OUTREACH_TEMPLATES.md\n"
        "present the two figures (two alternative end-to-end paths, not a base + delta)."
    )


if __name__ == "__main__":
    main()
