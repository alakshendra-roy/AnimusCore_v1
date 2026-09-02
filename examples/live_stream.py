#!/usr/bin/env python
"""Animus Engine -- nanobind zero-copy telemetry stream demo.

Starts a real background native producer thread (inside the compiled
extension -- see bindings/animus_py.cpp's TelemetryStream::start_producer)
pushing synthetic telemetry as fast as the SPSC ring accepts it, then
drains it from this Python process in a tight loop, timing two things
separately rather than conflating them (this project's benchmark culture,
see AnimusCore_v1/BENCHMARKS.md and BENCHMARK_DATASHEET.md's methodology
notes, treats that as a real methodology error, not a rounding choice):

  1. drain() alone -- acquiring the zero-copy view, no per-event decode.
     This is the number that answers "what does the zero-copy boundary
     itself cost."
  2. poll() (drain() + decode()) -- the realistic cost of actually
     materializing Python objects for every event, which is what most
     callers will do.

Defaults to a paced producer (500,000 events/sec) that this Python
consumer can sustain with zero drops, since an unthrottled native
producer measurably outruns single-threaded Python consumption on this
machine (~1.7M events/sec sustained here, see BENCHMARK_DATASHEET.md's
"Cross-core SPSC dispatch latency" section for the native-only numbers
without that ceiling) -- pass target_events_per_sec=0 for the
unthrottled flood instead, to see the ring's backpressure/drop behavior
when the consumer can't keep up. Either way, dropped_count is reported
honestly, not hidden.

Usage:
    python examples/live_stream.py [event_count] [batch_size] [target_events_per_sec]
"""

from __future__ import annotations

import sys
import time
from pathlib import Path

# Allow running directly from a source checkout without installing the
# package first -- explicit, not relying on cwd: importing plain `animus`
# from an arbitrary working directory is not guaranteed to resolve to
# this checkout's animus/ package over some other installed one.
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from animus.consumer import TelemetryConsumer, decode  # noqa: E402


def main() -> int:
    event_count = int(sys.argv[1]) if len(sys.argv) > 1 else 2_000_000
    batch_size = int(sys.argv[2]) if len(sys.argv) > 2 else 4096
    target_rate = float(sys.argv[3]) if len(sys.argv) > 3 else 500_000.0

    print("Animus Engine -- Zero-Copy Telemetry Stream Demo (nanobind)")
    print("=" * 61)
    print(f"Events to consume:  {event_count:,}")
    print(f"Drain batch size:   {batch_size:,}")
    print(f"Producer pacing:    {'unthrottled (flood)' if target_rate <= 0 else f'{target_rate:,.0f} events/sec'}")
    print()

    stream = TelemetryConsumer(capacity=65536, drain_batch_capacity=batch_size)

    drain_ns_total = 0
    decode_ns_total = 0
    received = 0
    batches = 0

    stream.start_synthetic_load(event_count=event_count, target_events_per_sec=target_rate)
    try:
        wall_start = time.perf_counter_ns()
        while True:
            t0 = time.perf_counter_ns()
            view = stream.drain(batch_size)
            t1 = time.perf_counter_ns()
            n = view.shape[0]
            if n == 0:
                # Ring momentarily empty. Only really done once the producer
                # has also finished -- otherwise it's just between pushes.
                # (received can never reach event_count on its own if any
                # events were dropped as full-ring backpressure, since
                # pushed_count + dropped_count == event_count always --
                # looping on "received < event_count" instead of this would
                # hang forever whenever the consumer can't keep up.)
                if not stream.producer_running:
                    break
                continue
            records = decode(view)
            t2 = time.perf_counter_ns()

            assert len(records) == n, "decode() must return exactly what drain() reported"

            drain_ns_total += (t1 - t0)
            decode_ns_total += (t2 - t1)
            received += n
            batches += 1
        wall_end = time.perf_counter_ns()
    finally:
        stream.stop_synthetic_load()

    wall_seconds = (wall_end - wall_start) / 1e9
    drain_ns_per_event = drain_ns_total / received
    poll_ns_per_event = (drain_ns_total + decode_ns_total) / received
    events_per_sec = received / wall_seconds

    print("Results")
    print("-" * 61)
    print(f"Batches drained:                 {batches:,}")
    print(f"Events received:                 {received:,}")
    print(f"Events pushed by producer:       {stream.pushed_count:,}")
    print(f"Events dropped (ring full):      {stream.dropped_count:,}")
    print(f"Wall time:                       {wall_seconds:.3f} s")
    print(f"Throughput:                      {events_per_sec / 1e6:.3f} M events/sec")
    print(f"drain() only, amortized/event:   {drain_ns_per_event:.1f} ns")
    print(f"drain()+decode(), amortized/event: {poll_ns_per_event:.1f} ns")
    print("-" * 61)
    if drain_ns_per_event < 1000.0:
        print(f"drain() alone: sub-microsecond per event, amortized over batches of {batch_size:,} "
              f"({drain_ns_per_event:.1f} ns/event measured on this run).")
    else:
        print(f"drain() alone measured {drain_ns_per_event:.1f} ns/event on this run -- "
              f"not sub-microsecond at this batch size/hardware; this is what was actually "
              f"measured, not an assumed number.")

    if received != stream.pushed_count:
        print()
        print(f"NOTE: received ({received:,}) != pushed ({stream.pushed_count:,}) -- "
              f"the producer may still be finishing when this ran, or events were dropped "
              f"({stream.dropped_count:,} reported). Not necessarily an error.")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
