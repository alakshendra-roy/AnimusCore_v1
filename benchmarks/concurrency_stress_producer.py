"""Phase 22: single-tenant producer process for concurrency_stress_test.py.

One of N genuinely separate OS processes launched concurrently by
concurrency_stress_test.py, one per tenant -- pushes synthetic RawEvent
telemetry into that tenant's own ShmRingChannel (opened by name, created
by concurrency_stress_consumer.cpp), then exits. Mirrors
AnimusCore_v1/execution_orchestration_demo.py's producer shape exactly
(same retry-with-backoff-on-partial-push discipline), for RawEvent instead
of OrderRequest.

Usage:
  python concurrency_stress_producer.py <tenant_id> <count>
"""
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from animus import ShmRingChannel  # noqa: E402

EVENT_ID = 101
BATCH_SIZE = 1024


def main() -> int:
    if len(sys.argv) < 3:
        print("Usage: python concurrency_stress_producer.py <tenant_id> <count>", file=sys.stderr)
        return 2
    tenant_id = int(sys.argv[1])
    count = int(sys.argv[2])
    ring_name = f"AnimusConcurrencyStressT{tenant_id}"

    ring = None
    deadline = time.time() + 5.0
    while time.time() < deadline:
        try:
            ring = ShmRingChannel.open(ring_name)
            break
        except OSError:
            time.sleep(0.01)
    if ring is None:
        print(f"[producer {tenant_id}] could not open ring {ring_name!r} within 5s -- start the consumer first",
              file=sys.stderr)
        return 1

    sent = 0
    while sent < count:
        batch_len = min(BATCH_SIZE, count - sent)
        batch = [(EVENT_ID, sent + i, 9999) for i in range(batch_len)]
        pushed = ring.push_batch(batch)
        sent += pushed
        if pushed == 0:
            # Ring momentarily full -- the consumer's retry-on-full discipline
            # (see concurrency_stress_consumer.cpp's flush()) guarantees this
            # only ever costs latency, never data, as long as THIS side also
            # retries rather than giving up on a partial push.
            time.sleep(0.0005)

    ring.close()
    print(f"[producer {tenant_id}] done: sent {sent} events", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
