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

from animus.bindings import AnimusBindings, RuleComparator, ThreatSignal

# Matches the hardcoded event_id used by _producer below, so the registered
# rules below actually see traffic during the stress test.
RULE_EVENT_ID = 101


def _producer(bindings: AnimusBindings, event_count: int, trace_offset: int, results: List[Tuple[int, int]], slot: int) -> None:
    accepted = 0
    rejected = 0
    for i in range(event_count):
        if bindings.record_event(event_id=101, trace_id=(trace_offset + i) & 0xFFFFFFFF, metric_value=9999):
            accepted += 1
        else:
            rejected += 1
    results[slot] = (accepted, rejected)


def _drain_signals(bindings: AnimusBindings, stop_event: threading.Event, collected: List[ThreatSignal]) -> None:
    """Continuously polls the non-blocking signal ring while producers run,
    rather than polling once at the end -- this is the pattern a real
    consumer should use, and avoids needing a signal ring sized to hold
    every possible match for the whole run.
    """
    while not stop_event.is_set():
        batch = bindings.poll_signals(max_count=4096)
        if batch:
            collected.extend(batch)
        else:
            time.sleep(0.001)


def run_stress_test(total_events: int, ring_capacity: int, log_path: str, thread_count: int, exercise_rules: bool = True) -> None:
    bindings = AnimusBindings()

    print(f"[ingest] Initializing native engine (ring buffer capacity={ring_capacity})...")
    if not bindings.init(ring_capacity):
        raise RuntimeError("animus_init failed")

    signals_collected: List[ThreatSignal] = []
    poller_thread = None
    stop_poll = threading.Event()

    if exercise_rules:
        # Rule 1 matches every event from _producer (metric_value is always
        # 9999, which is > 5000). Rule 2 never matches (9999 is never < 10).
        # Registering one of each exercises both the match and no-match
        # paths through EngineImpl::evaluate_rules.
        if not bindings.add_rule(1, RULE_EVENT_ID, 5000, RuleComparator.GREATER_THAN, severity=3):
            raise RuntimeError("add_rule (rule 1) failed")
        if not bindings.add_rule(2, RULE_EVENT_ID, 10, RuleComparator.LESS_THAN, severity=1):
            raise RuntimeError("add_rule (rule 2) failed")

        poller_thread = threading.Thread(target=_drain_signals, args=(bindings, stop_poll, signals_collected), daemon=True)
        poller_thread.start()

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
    # ring buffer, so persisted_bytes below reflects every accepted event,
    # and every rule evaluation for this run has already happened by the
    # time it returns.
    bindings.stop_logging()

    if exercise_rules:
        stop_poll.set()
        poller_thread.join()
        # Final drain: catches any signals pushed in the last batch before
        # the poller thread observed stop_poll.
        trailing = bindings.poll_signals(max_count=100_000)
        while trailing:
            signals_collected.extend(trailing)
            trailing = bindings.poll_signals(max_count=100_000)

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
    if exercise_rules:
        rule1_hits = sum(1 for s in signals_collected if s.rule_id == 1)
        rule2_hits = sum(1 for s in signals_collected if s.rule_id == 2)
        print(f"[ingest] Rule signals detected: {len(signals_collected):,} (rule 1 matches: {rule1_hits:,}, rule 2 matches: {rule2_hits:,})")
    print("=" * 60)

    if accepted + rejected != total_events:
        raise RuntimeError(
            f"Accounting mismatch: {accepted} accepted + {rejected} rejected != {total_events} submitted"
        )
    if rejected:
        print(f"[ingest] WARNING: {rejected:,} events rejected (ring buffer saturation under load).")
    if accepted and persisted_bytes == 0:
        raise RuntimeError("Persistence verification failed: no bytes written to disk despite accepted events")

    if exercise_rules:
        if rule1_hits != accepted:
            raise RuntimeError(
                f"Rule engine verification failed: expected {accepted} matches for rule 1 (matches every accepted event), got {rule1_hits}"
            )
        if rule2_hits != 0:
            raise RuntimeError(f"Rule engine verification failed: rule 2 should never match, got {rule2_hits} hits")
        print("[ingest] Rule engine verification passed: signal counts match expected rule matches.")

    print("[ingest] End-to-end verification passed: submit/accept/reject accounted for, persistence confirmed.")


def main() -> None:
    parser = argparse.ArgumentParser(description="AnimusCore_v1 end-to-end telemetry stress test")
    parser.add_argument("--events", type=int, default=600_000, help="Total events to submit")
    parser.add_argument("--ring-capacity", type=int, default=65536, help="Native ring buffer capacity")
    parser.add_argument("--log-file", type=str, default="telemetry_stream.bin", help="Persistence output path")
    parser.add_argument("--threads", type=int, default=1, help="Concurrent producer threads (exercises the MPMC ring buffer)")
    parser.add_argument("--no-rules", action="store_true", help="Skip registering/exercising the in-memory rule engine")
    args = parser.parse_args()

    run_stress_test(args.events, args.ring_capacity, args.log_file, args.threads, exercise_rules=not args.no_rules)


if __name__ == "__main__":
    main()
