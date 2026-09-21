"""
Animus Core sandbox -- zero-copy Python bridge benchmark.

Measures the real, amortized per-event cost of TelemetryStream.drain():
a background native producer thread feeds the MPMC ring unthrottled while
this script drains it in a tight loop, timing only drain() itself with
time.perf_counter_ns(). No per-event Python object is constructed on this
path -- drain() returns a zero-copy ndarray view over a scratch buffer the
C++ side owns and reuses across calls (see nanobind_bridge.cpp); the only
allocation in that function's entire call graph happened once, at
TelemetryStream construction, not per drain().

Reference machines in this class have measured roughly ~30-65 ns/event on
this benchmark; your own number, printed below, is what to cite for your
hardware -- treat any single figure elsewhere as a ballpark, not a promise
for your box.

Build first (see ../README.md):
    cmake -B build && cmake --build build --config Release

Then run this script either from the build's output directory (where
animus_sandbox_bridge*.pyd/.so was placed), or after copying that file
next to this script:

    python python_bridge_test.py
"""
import os
import statistics
import sys
import time

if sys.platform == "win32":
    # Python 3.8+ stopped searching PATH for a native extension's own DLL
    # dependencies (PEP 578 / bpo-36085): a MinGW-built .pyd still needs its
    # runtime (libstdc++-*.dll, libgcc_s_*.dll) found explicitly. MSYS2's
    # default install path covers the common case; harmless if it doesn't
    # exist or isn't needed (a statically-linked or MSVC-built .pyd).
    for _dll_dir in (r"C:\msys64\ucrt64\bin", r"C:\msys64\mingw64\bin"):
        if os.path.isdir(_dll_dir):
            os.add_dll_directory(_dll_dir)

import animus_sandbox_bridge as bridge

EVENTS_PER_RUN = 2_000_000
# ring_buffer.hpp's MpmcBoundedQueue never drops events on overflow --
# enqueue() retries (spin + yield) until a slot is free, so an undersized
# ring cannot lose data the way a drop-on-full design could. It CAN still
# make the producer block waiting on this (much faster) consumer to make
# room, which would understate real throughput by coupling the two sides
# together; sizing the ring well above one run's event count keeps the
# producer unthrottled for the whole run, matching the "concurrent,
# independent producer/consumer" scenario this benchmark exists to measure.
RING_CAPACITY = 1 << 22  # 4,194,304
DRAIN_BATCH = 8192
NUM_RUNS = 5

# A dedicated, uncontended producer thread can finish enqueueing an entire
# run (tens of microseconds for a few million lock-free pushes) faster
# than this process's own first drain() call dispatches -- so seeing
# drain() return 0 events on an early iteration, even with
# producer_running already False, is a normal, harmless race, not data
# loss (nothing was ever dropped -- see above). Only a drain() that keeps
# returning 0 for longer than this indicates an actual stall/deadlock.
STALL_TIMEOUT_SECONDS = 2.0


def drain_only_ns_per_event(events: int, verify_zero_copy: bool = False) -> float:
    """One full run: background producer feeds `events` events unthrottled;
    this thread drains in a tight loop, timing only drain() itself."""
    stream = bridge.TelemetryStream(RING_CAPACITY, DRAIN_BATCH)
    stream.start_producer(events)
    try:
        drained = 0
        total_ns = 0
        checked_ownership = not verify_zero_copy
        last_progress = time.perf_counter()
        while drained < events:
            t0 = time.perf_counter_ns()
            view = stream.drain(DRAIN_BATCH)
            t1 = time.perf_counter_ns()
            if not checked_ownership and view.shape[0] > 0:
                # Proves this is a real zero-copy view over the C++ scratch
                # buffer, not numpy silently copying on the way out: an
                # owned array would report OWNDATA True here.
                assert view.flags["OWNDATA"] is False, (
                    "drain() returned an array that owns its data -- this is "
                    "no longer a zero-copy view, investigate before citing "
                    "any latency number from this run"
                )
                checked_ownership = True
            n = view.shape[0] // bridge.EVENT_SIZE_BYTES
            total_ns += (t1 - t0)
            drained += n
            now = time.perf_counter()
            if n > 0:
                last_progress = now
            elif now - last_progress > STALL_TIMEOUT_SECONDS:
                raise RuntimeError(
                    f"drain() returned 0 events for over {STALL_TIMEOUT_SECONDS}s "
                    f"({drained}/{events} drained so far, producer_running="
                    f"{stream.producer_running}) -- this queue design never drops "
                    "events, so this indicates a real stall, not data loss."
                )
        return total_ns / drained
    finally:
        stream.stop_producer()


def main() -> None:
    print("=" * 74)
    print("  ANIMUS CORE SANDBOX: zero-copy Python drain latency")
    print(f"  {EVENTS_PER_RUN:,} events/run, {NUM_RUNS} runs, batch={DRAIN_BATCH:,}, "
          f"ring capacity={RING_CAPACITY:,}")
    print("  Concurrent unthrottled native producer + Python-side draining, "
          "timed per batch (perf_counter_ns)")
    print("=" * 74)

    results = []
    for run in range(1, NUM_RUNS + 1):
        ns_per_event = drain_only_ns_per_event(EVENTS_PER_RUN, verify_zero_copy=(run == 1))
        results.append(ns_per_event)
        suffix = "  [OWNDATA=False verified]" if run == 1 else ""
        print(f"  run {run}/{NUM_RUNS}: {ns_per_event:.2f} ns/event{suffix}")

    print()
    print(f"  median: {statistics.median(results):.2f} ns/event "
          f"({1e9 / statistics.median(results):,.0f} events/sec)")
    print(f"  range:  {min(results):.2f} - {max(results):.2f} ns/event "
          f"(mean {statistics.mean(results):.2f} ns/event)")


if __name__ == "__main__":
    main()
