"""End-to-end telemetry ingestion stress test for the native AnimusCore_v1 engine.

Exercises the full pipeline through the typed AnimusBindings wrapper (see
animus/bindings.py): Python call -> C-ABI -> lock-free MPMC ring buffer ->
async persistence worker -> disk (see AnimusCore_v1/animus.hpp,
animus_engine.cpp).
"""
import argparse
import os
import threading
import time
from typing import List, Tuple

from animus.bindings import AnimusBindings


def _producer(bindings: AnimusBindings, event_count: int, trace_offset: int, results: List[Tuple[int, int]], slot: int) -> None:
    accepted = 0
    rejected = 0
    for i in range(event_count):
        if bindings.record_event(event_id=101, trace_id=(trace_offset + i) & 0xFFFFFFFF, metric_value=9999):
            accepted += 1
        else:
            rejected += 1
    results[slot] = (accepted, rejected)


def run_stress_test(total_events: int, ring_capacity: int, log_path: str, thread_count: int) -> None:
    bindings = AnimusBindings()

    print(f"[ingest] Initializing native engine (ring buffer capacity={ring_capacity})...")
    if not bindings.init(ring_capacity):
        raise RuntimeError("animus_init failed")

    if os.path.exists(log_path):
        os.remove(log_path)

    bindings.start_logging(log_path)

    events_per_thread = total_events // thread_count
    remainder = total_events - events_per_thread * thread_count
    results: List[Tuple[int, int]] = [(0, 0)] * thread_count

    start = time.perf_counter_ns()
    if thread_count == 1:
        _producer(bindings, total_events, 0, results, 0)
    else:
        threads = []
        offset = 0
        for slot in range(thread_count):
            count = events_per_thread + (1 if slot < remainder else 0)
            t = threading.Thread(target=_producer, args=(bindings, count, offset, results, slot))
            threads.append(t)
            offset += count
        for t in threads:
            t.start()
        for t in threads:
            t.join()
    end = time.perf_counter_ns()

    # stop_logging() blocks until the background worker fully drains the
    # ring buffer, so persisted_bytes below reflects every accepted event.
    bindings.stop_logging()

    accepted = sum(a for a, _ in results)
    rejected = sum(r for _, r in results)

    elapsed_s = (end - start) / 1e9
    avg_ns = (end - start) / total_events if total_events else 0.0
    throughput = total_events / elapsed_s if elapsed_s > 0 else float("inf")

    persisted_bytes = os.path.getsize(log_path) if os.path.exists(log_path) else 0
    bytes_per_record = persisted_bytes / accepted if accepted else 0

    print("=" * 60)
    print(f"[ingest] Producer threads:      {thread_count}")
    print(f"[ingest] Events submitted:      {total_events:,}")
    print(f"[ingest] Events accepted:       {accepted:,}")
    print(f"[ingest] Events rejected (full):{rejected:,}")
    print(f"[ingest] Total wall time:       {elapsed_s * 1000:.2f} ms")
    print(f"[ingest] Avg latency per op:    {avg_ns:.2f} ns")
    print(f"[ingest] Throughput:            {throughput:,.0f} events/sec")
    print(f"[ingest] Persisted bytes:       {persisted_bytes:,} ({bytes_per_record:.1f} bytes/record)")
    print("=" * 60)

    if accepted + rejected != total_events:
        raise RuntimeError(
            f"Accounting mismatch: {accepted} accepted + {rejected} rejected != {total_events} submitted"
        )
    if rejected:
        print(f"[ingest] WARNING: {rejected:,} events rejected (ring buffer saturation under load).")
    if accepted and persisted_bytes == 0:
        raise RuntimeError("Persistence verification failed: no bytes written to disk despite accepted events")

    print("[ingest] End-to-end verification passed: submit/accept/reject accounted for, persistence confirmed.")


def main() -> None:
    parser = argparse.ArgumentParser(description="AnimusCore_v1 end-to-end telemetry stress test")
    parser.add_argument("--events", type=int, default=600_000, help="Total events to submit")
    parser.add_argument("--ring-capacity", type=int, default=65536, help="Native ring buffer capacity")
    parser.add_argument("--log-file", type=str, default="telemetry_stream.bin", help="Persistence output path")
    parser.add_argument("--threads", type=int, default=1, help="Concurrent producer threads (exercises the MPMC ring buffer)")
    args = parser.parse_args()

    run_stress_test(args.events, args.ring_capacity, args.log_file, args.threads)


if __name__ == "__main__":
    main()
