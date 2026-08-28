"""Phase 6 demo/verification: cross-process shared-memory telemetry IPC.

Spawns a separate consumer process that attaches to a SharedTelemetryRing
by name and drains it, while this (producer) process streams a synthetic
ThreatAgent batch into the same ring -- proving the two OS processes are
exchanging records through shared memory (multiprocessing.shared_memory),
not through the native engine's in-process ring buffer or any pickling/
socket round trip.
"""
import multiprocessing
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from animus.shm import SharedTelemetryRing
from threat_agent import ThreatAgent

_SENTINEL_EVENT_ID = 0xFFFFFFFF
_RING_NAME = "animus_phase6_demo_ring"
_RING_CAPACITY = 4096


def _consumer_main(ring_name: str, result_queue: "multiprocessing.Queue") -> None:
    ring = SharedTelemetryRing.attach(ring_name)
    received = 0
    critical = 0
    try:
        while True:
            record = ring.pop()
            if record is None:
                time.sleep(0.0001)
                continue
            if record.event_id == _SENTINEL_EVENT_ID:
                break
            received += 1
            if record.event_id == 999:
                critical += 1
    finally:
        ring.close()
    result_queue.put((received, critical))


def main() -> None:
    event_count = 200_000
    print(f"[SHM DEMO] Creating shared telemetry ring '{_RING_NAME}' (capacity={_RING_CAPACITY})...")
    ring = SharedTelemetryRing.create(_RING_NAME, _RING_CAPACITY)

    result_queue: "multiprocessing.Queue" = multiprocessing.Queue()
    consumer = multiprocessing.Process(target=_consumer_main, args=(_RING_NAME, result_queue))
    consumer.start()

    print(f"[SHM DEMO] Streaming {event_count:,} synthetic events into shared memory...")
    stream = ThreatAgent.generate_telemetry_batch(event_count)

    start = time.perf_counter_ns()
    pushed = 0
    for event_id, trace_id, metric_value in stream:
        while not ring.push(event_id, trace_id, metric_value):
            time.sleep(0)  # ring momentarily full; yield to the consumer process
        pushed += 1
    while not ring.push(_SENTINEL_EVENT_ID, 0, 0):
        time.sleep(0)
    end = time.perf_counter_ns()

    received, critical = result_queue.get()
    consumer.join()
    ring.close()
    ring.unlink()

    expected_critical = sum(1 for event_id, _, _ in stream if event_id == 999)
    elapsed_ms = (end - start) / 1e6

    print("=" * 60)
    print(f"[SHM DEMO] Events pushed by producer:    {pushed:,}")
    print(f"[SHM DEMO] Events received by consumer:  {received:,}")
    print(f"[SHM DEMO] Critical events (expected):   {expected_critical:,}")
    print(f"[SHM DEMO] Critical events (received):   {critical:,}")
    print(f"[SHM DEMO] Producer wall time:            {elapsed_ms:.2f} ms")
    print("=" * 60)

    if received != pushed or critical != expected_critical:
        raise RuntimeError("SHM IPC verification failed: consumer did not receive every event intact")

    print("[SHM DEMO] Phase 6 Shared-Memory IPC Verified.")


if __name__ == "__main__":
    main()
