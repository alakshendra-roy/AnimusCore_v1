#!/usr/bin/env python3
"""Animus Evaluation Kit -- live stream verification (nanobind, zero-copy).

Attaches to the shared-memory ring a running `bin/harness_benchmark`
producer created, drains it through the compiled `_animus_shm_native`
extension (bindings/animus_shm_py.cpp -- animus::sys::ipc::ShmRing<
ExecutionEvent>, the same struct the C++ producer writes; see
include/animus/execution_event.hpp for the single shared wire-format
definition both sides build against), and reports throughput, latency
percentiles, and data-integrity validation.

This is the client-facing, nanobind-accelerated counterpart to this
project's benchmarks/consumer.py, which takes a pure-stdlib
(multiprocessing.shared_memory + struct) path with no compiled extension
required -- useful in CI, but it never releases the GIL during its
poll loop and decodes one record at a time in Python. This script's
poll() call runs its entire spin-wait for new records with the GIL
released (see animus_shm_py.cpp's file header for the exact discipline),
so this Python process's other threads are never blocked waiting on a
producer in a different OS process, and a batch of records is handed
back as one zero-copy buffer-protocol view rather than N individual
Python objects.

Latency methodology, stated precisely rather than left implicit: this
script measures *consumer-side inter-arrival latency* -- the wall-clock
gap, timestamped with this process's own CLOCK_MONOTONIC_RAW, between
successive records as they are decoded here. It is NOT the producer's
own enqueue latency (harness_benchmark.cpp measures and reports that
separately, from its own RDTSC-calibrated clock) and the two are not
directly comparable: the producer's dispatch_ts_raw field is a raw,
uncalibrated clock sample (RDTSC ticks on x86_64) written by a different
process with no shared calibration published between the two, so
reconstructing a true producer-to-consumer transport latency from it
here would silently be wrong. What this script reports instead --
"how smoothly is my Python consumption loop actually keeping up" -- is
the number a systems engineer integrating against this stream actually
needs.
"""
from __future__ import annotations

import argparse
import array
import struct
import sys
import time


def _percentile(sorted_values: array.array, p: float) -> float:
    n = len(sorted_values)
    if n == 0:
        return 0.0
    idx = int(p * (n - 1))
    if idx >= n:
        idx = n - 1
    return float(sorted_values[idx])


def _fmt_ns(value_ns: float) -> str:
    if value_ns >= 1_000_000:
        return f"{value_ns / 1_000_000:.3f} ms"
    if value_ns >= 1_000:
        return f"{value_ns / 1_000:.3f} us"
    return f"{value_ns:.1f} ns"


def _print_table(rows: "list[tuple[str, str]]", title: str) -> None:
    label_w = max(len(r[0]) for r in rows) + 2
    value_w = max(len(r[1]) for r in rows) + 2
    width = label_w + value_w + 3
    print(f"\n{title}")
    print("-" * width)
    for label, value in rows:
        print(f"| {label:<{label_w-1}}| {value:>{value_w-2}} |")
    print("-" * width)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--name", default="animus_harness_shm",
                         help="shared-memory segment name harness_benchmark created (default: animus_harness_shm)")
    parser.add_argument("--events", type=int, default=10_000_000,
                         help="target event count to consume before stopping (default: 10000000)")
    parser.add_argument("--drain-batch", type=int, default=8192,
                         help="max records fetched per poll() call (default: 8192)")
    parser.add_argument("--max-spins", type=int, default=200_000,
                         help="spin-wait attempts per poll() before returning whatever arrived (default: 200000)")
    parser.add_argument("--idle-timeout-s", type=float, default=3.0,
                         help="stop early if the ring stays empty this long AND the producer has exited (default: 3.0)")
    parser.add_argument("--progress-interval-s", type=float, default=1.0,
                         help="seconds between live throughput updates (default: 1.0)")
    args = parser.parse_args()

    try:
        from animus import _animus_shm_native as native
    except ImportError:
        print(
            "error: the compiled _animus_shm_native extension is not installed in this "
            "environment.\n"
            "Install both eval-kit wheels first (see README.md's Quickstart):\n"
            "  pip install wheels/animus_engine_sdk-*.whl\n"
            "  pip install wheels/animus_native_stream-*.whl",
            file=sys.stderr,
        )
        return 1

    try:
        channel = native.SharedExecutionChannel.open(args.name, drain_batch_capacity=args.drain_batch)
    except RuntimeError as exc:
        print(f"error: {exc}\n"
              f"Is harness_benchmark running with --name {args.name}? "
              f"(./bin/harness_benchmark --name {args.name} --events {args.events} --mode overwrite)",
              file=sys.stderr)
        return 1

    print("Animus Evaluation Kit -- Live Stream Verification")
    print("===================================================")
    print(f"Segment name:     {args.name}")
    print(f"Ring capacity:    {channel.capacity} slots")
    print(f"Target events:    {args.events}")
    print(f"Wire format:      {native.WIRE_FORMAT} ({native.WIRE_RECORD_SIZE} bytes/record)")
    print(f"Producer pid:     {'(not yet attached)' if not channel.is_producer_alive(0) else 'attached and alive'}\n")

    consumed = 0
    gaps = 0
    last_sequence: "int | None" = None
    integrity_ok = True
    interrupted = False
    idle_since: "float | None" = None

    inter_arrival_ns = array.array("q")
    last_record_time_ns: "int | None" = None

    unpack = struct.Struct(native.WIRE_FORMAT).iter_unpack
    monotonic_ns = time.monotonic_ns

    t_start = time.perf_counter()
    last_progress_t = t_start
    last_progress_count = 0

    try:
        while consumed < args.events:
            view = channel.poll(args.drain_batch, args.max_spins)
            n = view.shape[0]

            if n == 0:
                if not channel.is_producer_alive(0):
                    if idle_since is None:
                        idle_since = time.perf_counter()
                    elif time.perf_counter() - idle_since > args.idle_timeout_s:
                        print(f"\nProducer has exited and the ring has been empty for "
                              f"{args.idle_timeout_s:.1f}s -- stopping at {consumed}/{args.events} "
                              f"(the remainder were lost to overwrite, if the producer ran in that mode).")
                        break
                continue
            idle_since = None

            raw = bytes(view)  # one copy out of the zero-copy scratch view before the next poll() overwrites it
            channel.consumer_heartbeat()  # once per batch is ample liveness resolution -- no need to pay the FFI call per record
            for sequence, _dispatch_ts_raw, _price_ticks, _quantity, _instrument_id, _flags in unpack(raw):
                now_ns = monotonic_ns()
                if last_record_time_ns is not None:
                    inter_arrival_ns.append(now_ns - last_record_time_ns)
                last_record_time_ns = now_ns

                consumed += 1

                if last_sequence is not None:
                    if sequence <= last_sequence:
                        print(f"INTEGRITY FAILURE: sequence went backwards or repeated "
                              f"({sequence} after {last_sequence})", file=sys.stderr)
                        integrity_ok = False
                    else:
                        gaps += sequence - last_sequence - 1
                last_sequence = sequence

            now_t = time.perf_counter()
            if now_t - last_progress_t >= args.progress_interval_s:
                inst_rate = (consumed - last_progress_count) / (now_t - last_progress_t)
                print(f"\r  ... {consumed:>12,} events consumed "
                      f"({inst_rate / 1_000_000:6.3f} M/sec instantaneous)", end="", flush=True)
                last_progress_t = now_t
                last_progress_count = consumed
    except KeyboardInterrupt:
        interrupted = True
        print("\n\nInterrupted (Ctrl+C) -- detaching cleanly. Summary reflects events consumed so far.")

    print()  # newline after the last \r progress line

    t_end = time.perf_counter()
    wall_seconds = t_end - t_start
    throughput = consumed / wall_seconds if wall_seconds > 0 else 0.0

    sorted_deltas = array.array("q", sorted(inter_arrival_ns))
    latency_rows = [
        ("min", _fmt_ns(_percentile(sorted_deltas, 0.0))),
        ("p50", _fmt_ns(_percentile(sorted_deltas, 0.50))),
        ("p90", _fmt_ns(_percentile(sorted_deltas, 0.90))),
        ("p99", _fmt_ns(_percentile(sorted_deltas, 0.99))),
        ("p99.9", _fmt_ns(_percentile(sorted_deltas, 0.999))),
        ("max", _fmt_ns(_percentile(sorted_deltas, 1.0))),
    ]
    _print_table(latency_rows, "Consumer-side inter-arrival latency (this process's own clock, not producer transport latency)")

    summary_rows = [
        ("Events consumed", f"{consumed:,}"),
        ("Throughput", f"{throughput:,.0f} ticks/sec ({throughput / 1_000_000:.3f} M/sec)"),
        ("Wall time", f"{wall_seconds:.3f} s"),
        ("Sequence gaps seen", f"{gaps:,}"),
        ("Producer dropped_count", f"{channel.dropped_count:,}"),
        ("Gaps == dropped_count?", "yes" if gaps == channel.dropped_count else "NO -- investigate"),
        ("Data integrity", "OK" if integrity_ok else "FAILED -- see stderr above"),
        ("Run status", "interrupted (Ctrl+C)" if interrupted else "completed"),
    ]
    _print_table(summary_rows, "Summary")

    return 0 if (integrity_ok and not interrupted) else (130 if interrupted else 1)


if __name__ == "__main__":
    raise SystemExit(main())
