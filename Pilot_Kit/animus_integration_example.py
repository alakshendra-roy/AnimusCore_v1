#!/usr/bin/env python3
"""Animus Core Pilot Kit -- Python dynamic-library interop example.

A complete, runnable demonstration of the real Python <-> native-DLL
integration path a pilot evaluation would use: load the compiled engine,
optionally check an evaluation license, ingest a batch of telemetry events
through the real C-ABI (animus_record_events_batch, not a mock), register
a threshold rule, and drain the threat signals it produces -- then report
the *measured* per-event ingestion latency for this run, not an asserted
number. See AnimusCore_v1/BENCHMARKS.md Phase 11/13/26 for the same
measurement taken across many runs on the reference development machine
(consistently sub-microsecond per event for batched ingestion at this
batch size) -- this script prints what your machine actually measures,
since batch size, CPU, and background load all affect the real number.

Setup: see PILOT_README.md in this directory.

Usage:
    python Pilot_Kit/animus_integration_example.py [path/to/pilot_license.lic]

The license argument is optional -- core event ingestion works without
one. Passing it prints the license's verification status (VALID / EXPIRED
/ WRONG_MACHINE / ...); a subset of features unrelated to this demo (CPU
core pinning) additionally require a valid license to do anything at all
(fail closed, no unlicensed default -- see AnimusCore_v1/BENCHMARKS.md's
licensing phases).
"""
import os
import sys
import tempfile
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from animus.bindings import AnimusBindings, RuleComparator  # noqa: E402

RING_CAPACITY = 1 << 17  # 131,072
BATCH_SIZE = 10_000
NUM_BATCHES = 50  # 500,000 events total
RULE_EVENT_ID = 500
RULE_THRESHOLD = 100  # metric_value cycles 0-149, so ~1/3 of events match
SUB_MICROSECOND_NS = 1000.0


def check_license(bindings: AnimusBindings, license_path: str) -> None:
    if not license_path:
        print(
            "No evaluation license path given -- proceeding with core event ingestion "
            "only. (CPU core pinning and other licensed features are unavailable "
            "without a verified license; this demo doesn't use them.)"
        )
        return
    if not os.path.exists(license_path):
        print(f"WARNING: license file not found at {license_path!r} -- continuing unlicensed.")
        return

    status = bindings.check_license_status(license_path)
    print(f"License status ({license_path}): {status.name}")
    if status.name == "VALID":
        print(f"  Entitled cores: {bindings.licensed_max_cores()}")


def run_ingestion_demo(bindings: AnimusBindings) -> None:
    if not bindings.init(buffer_capacity=RING_CAPACITY):
        raise SystemExit("animus_init failed")

    bindings.add_rule(
        rule_id=1, event_id=RULE_EVENT_ID, threshold=RULE_THRESHOLD,
        comparator=RuleComparator.GREATER_THAN, severity=5,
    )

    total_events = NUM_BATCHES * BATCH_SIZE
    print(f"\nIngesting {total_events:,} events in {NUM_BATCHES} batches of "
          f"{BATCH_SIZE:,} via animus_record_events_batch()...")

    with tempfile.TemporaryDirectory() as tmp:
        log_path = os.path.join(tmp, "pilot_example_telemetry.log")
        # Persistence + rule evaluation run on a background worker thread --
        # starting it before the ingestion loop lets it drain concurrently,
        # so a batch call is never blocked waiting for disk I/O (same
        # pattern used throughout benchmarks/ in this repo).
        bindings.start_logging(log_path)

        total_pushed = 0
        total_call_time_ns = 0
        total_matches = 0

        for batch_idx in range(NUM_BATCHES):
            events = [
                (RULE_EVENT_ID, batch_idx * BATCH_SIZE + i, i % 150)
                for i in range(BATCH_SIZE)
            ]

            t0 = time.perf_counter_ns()
            pushed = bindings.record_events_batch(events)
            t1 = time.perf_counter_ns()

            if pushed < len(events):
                # Ring momentarily full: drain signals to make room, then
                # push the remainder. Handled rather than assumed away, in
                # case this runs on a slower or busier evaluation machine.
                total_matches += len(bindings.poll_signals(max_count=4096))
                remaining = events[pushed:]
                while remaining:
                    n = bindings.record_events_batch(remaining)
                    if n == 0:
                        total_matches += len(bindings.poll_signals(max_count=4096))
                        time.sleep(0.001)
                        continue
                    remaining = remaining[n:]

            total_pushed += BATCH_SIZE
            total_call_time_ns += (t1 - t0)

            if batch_idx % 10 == 9:
                total_matches += len(bindings.poll_signals(max_count=4096))

        time.sleep(0.1)  # let the persistence/rule-evaluation worker catch up
        total_matches += len(bindings.poll_signals(max_count=1_000_000))
        bindings.stop_logging()

    mean_ns_per_event = total_call_time_ns / total_pushed
    print(f"\nPushed {total_pushed:,} events.")
    print(f"Measured mean ingestion latency: {mean_ns_per_event:.1f} ns/event "
          f"({mean_ns_per_event / 1000:.4f} us/event)")
    if mean_ns_per_event < SUB_MICROSECOND_NS:
        print("-> Sub-microsecond per-event ingestion latency achieved on this run.")
    else:
        print(
            "-> Above 1 us/event on this run. This is batch-size, CPU, and "
            "background-load dependent, not a fixed guarantee -- see "
            "AnimusCore_v1/BENCHMARKS.md Phase 11/13 for reference numbers "
            "measured across many runs, and try a larger BATCH_SIZE."
        )

    print(f"\nThreat signals matched: {total_matches:,} "
          f"(rule: event_id={RULE_EVENT_ID}, metric_value > {RULE_THRESHOLD})")


def main() -> None:
    license_path = sys.argv[1] if len(sys.argv) > 1 else ""

    bindings = AnimusBindings()
    if not bindings.using_native_engine:
        raise SystemExit(
            "No compiled native engine found. Build AnimusCore_v1.slnx (MSVC) or "
            "CMakeLists.txt first, or install a wheel that bundles the native "
            "library for your platform -- see PILOT_README.md."
        )

    print("=" * 78)
    print("  ANIMUS CORE -- PILOT INTEGRATION EXAMPLE")
    print("=" * 78)

    check_license(bindings, license_path)
    run_ingestion_demo(bindings)

    print("\nDone. See PILOT_README.md for next steps.")


if __name__ == "__main__":
    main()
