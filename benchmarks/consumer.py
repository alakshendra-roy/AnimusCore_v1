"""Animus Engine -- Cross-Process Shared-Memory Consumer (Milestone 4).

Consumer half of harness_benchmark.cpp's `--mode backpressure` evaluation
harness. Attaches to the exact same named OS shared-memory segment
(animus::sys::ipc::SpscQueue<ExecutionEvent>, include/animus/spsc_queue.hpp)
that binary creates and pushes into, decodes records directly with
`struct` against a hardcoded byte layout mirroring that C++ header
byte-for-byte, and drains it with no serialization step and no dependency
beyond the standard library -- consistent with this project's
zero-dependency ctypes/stdlib SDK path (animus/shm.py takes the same
"mirror the C++ layout by hand" approach for its own, different, wire
format; see that module's docstring for the same rationale applied there).
It needs no compiled extension, which is what lets CI and the evaluation kit
run it anywhere a Python interpreter exists.

SpscQueue is lossless: the producer is refused when the queue is full and
waits for this reader, nothing is ever overwritten. So unlike the old
overwrite-capable ring, any sequence gap seen here is a real loss, and this
script exits non-zero on one -- as it does on a short count, a backwards or
repeated sequence number, or a segment that is not an SpscQueue of
ExecutionEvent records.

Layout assumption: this hardcodes ANIMUS_CACHE_LINE_SIZE == 64
(spsc_queue.hpp's default on every target except ARM64, where it's 128).
harness_benchmark.cpp and this script must both run on a 64-byte-cache-line
target for the header offsets below to agree; that covers essentially all
x86_64 evaluation hardware this harness is meant for. A 128-byte build is
refused at attach (the magic/size checks fail) rather than misread.

Only the SpscQueue<T> layout is understood. harness_benchmark's default
`--mode overwrite` publishes into a BroadcastRing<ExecutionEvent>
(include/animus/broadcast_ring.hpp), whose header is a different shape; this
script refuses such a segment with a ValueError rather than misreading it --
read those with eval_kit/scripts/verify_stream.py (the nanobind
BroadcastRing binding) instead.

SpscQueue's header carries no producer pid or heartbeat, so this reader cannot
tell "the producer exited" from "the producer is momentarily behind"; it
stops when the queue has stayed empty for --idle-timeout-s, and treats
having consumed fewer than --events by then as a failure.

Usage (after starting harness_benchmark --mode backpressure --name NAME;
this script retries the attach for --attach-timeout-s, so start order is not
critical):

    python consumer.py --name animus_harness_shm --events 10000000
"""
from __future__ import annotations

import argparse
import struct
import sys
import time
from multiprocessing import shared_memory
from typing import NamedTuple, Optional

# --- Header layout: must match SpscQueueHeader exactly ----------------------
# (include/animus/spsc_queue.hpp). Three 64-byte cache lines: a read-only
# descriptor, then the producer-owned `head` line, then the consumer-owned
# `tail` line; slots follow immediately, back to back. This reader is the
# sole writer of `tail`.
_CACHE_LINE = 64
_MAGIC_OFF = 0
_CAPACITY_OFF = 8
_MASK_OFF = 16
_PAYLOAD_SIZE_OFF = 24
_HEAD_OFF = _CACHE_LINE
_TAIL_OFF = 2 * _CACHE_LINE
_HEADER_SIZE = 3 * _CACHE_LINE
_MAGIC = 0x5350534351554555  # SpscQueue<T>::kMagic, "SPSCQUEU"

# --- Record layout: must match animus::ExecutionEvent -----------------------
# (include/animus/execution_event.hpp)
# sequence(u64), dispatch_ts_raw(u64), price_ticks(i64), quantity(i64),
# instrument_id(u32), flags(u32) -- 40 bytes, no padding (that header
# static_asserts this on the C++ side).
_RECORD_FORMAT = "<QQqqII"
_RECORD_SIZE = struct.calcsize(_RECORD_FORMAT)
assert _RECORD_SIZE == 40

_U64 = struct.Struct("<Q")
_RECORD = struct.Struct(_RECORD_FORMAT)


class ExecutionEvent(NamedTuple):
    sequence: int
    dispatch_ts_raw: int
    price_ticks: int
    quantity: int
    instrument_id: int
    flags: int


class SegmentNotReady(Exception):
    """The segment exists but its header is not initialised yet (the producer
    creates the mapping before it writes the magic): worth retrying."""


class SpscQueueConsumer:
    """Consumer view over a SpscQueue<ExecutionEvent> segment created by
    harness_benchmark.cpp. Consumer-side only: mirrors that class's try_pop()
    exactly -- `tail` is read from a process-local copy (this side is its only
    writer), `head` is re-read from shared memory only when the cached value
    says the queue is empty, and `tail` is published after the slot has been
    copied out so the producer can reuse it. Plain 8-byte-aligned loads and
    stores are atomic on x86/x64 hardware with the ordering the C++ side's
    acquire/release pair needs, the same reasoning animus/shm.py's own
    SharedTelemetryRing documents for its unrelated wire format.
    """

    def __init__(self, name: str):
        shm = _attach(name)
        try:
            buf = shm.buf
            if len(buf) < _HEADER_SIZE:
                raise SegmentNotReady(f"segment '{name}' is {len(buf)} bytes, smaller than the {_HEADER_SIZE}-byte header")
            magic = _U64.unpack_from(buf, _MAGIC_OFF)[0]
            if magic == 0:
                raise SegmentNotReady(f"segment '{name}' header not initialised yet")
            if magic != _MAGIC:
                raise ValueError(
                    f"segment '{name}' has magic {magic:#018x}, expected {_MAGIC:#018x} (SpscQueue) -- "
                    f"this script only understands the lossless SpscQueue<T> layout that "
                    f"harness_benchmark's --mode backpressure writes, not the BroadcastRing<T> "
                    f"from --mode overwrite (use eval_kit/scripts/verify_stream.py for that) "
                    f"or a legacy ShmRing<T> segment left by an older build")
            capacity, mask, payload_size = (_U64.unpack_from(buf, off)[0]
                                            for off in (_CAPACITY_OFF, _MASK_OFF, _PAYLOAD_SIZE_OFF))
            if payload_size != _RECORD_SIZE:
                raise ValueError(
                    f"segment '{name}' has payload_size={payload_size}, expected {_RECORD_SIZE} "
                    f"(this script only understands animus::ExecutionEvent)")
            if capacity < 2 or capacity & (capacity - 1) or mask != capacity - 1:
                raise ValueError(f"segment '{name}' has an invalid header: capacity={capacity} mask={mask}")
            if len(buf) < _HEADER_SIZE + capacity * _RECORD_SIZE:
                raise ValueError(
                    f"segment '{name}' is {len(buf)} bytes, too small for capacity={capacity} "
                    f"({_HEADER_SIZE + capacity * _RECORD_SIZE} bytes needed)")
        except BaseException:
            shm.close()
            raise
        self._shm = shm
        self._buf = shm.buf
        self.capacity = capacity
        self._mask = mask
        self._tail = _U64.unpack_from(self._buf, _TAIL_OFF)[0]
        self._cached_head = self._tail

    def try_pop(self) -> Optional[ExecutionEvent]:
        """Backpressure-side pop: returns None at once when the queue is empty."""
        tail = self._tail
        if tail == self._cached_head:
            self._cached_head = _U64.unpack_from(self._buf, _HEAD_OFF)[0]
            if tail == self._cached_head:
                return None
        rec = _RECORD.unpack_from(self._buf, _HEADER_SIZE + (tail & self._mask) * _RECORD_SIZE)
        self._tail = tail + 1
        _U64.pack_into(self._buf, _TAIL_OFF, tail + 1)  # retires the slot for reuse
        return ExecutionEvent(*rec)

    def close(self) -> None:
        self._buf = None  # release the exported memoryview or close() raises BufferError
        self._shm.close()


def _attach(name: str) -> shared_memory.SharedMemory:
    """Open an existing segment without taking ownership of it. On POSIX
    before Python 3.13, SharedMemory registers every attach with the resource
    tracker, which would shm_unlink the producer's segment when this process
    exits -- undo that, the producer owns the name."""
    try:
        return shared_memory.SharedMemory(name=name, create=False, track=False)  # 3.13+
    except TypeError:
        pass
    shm = shared_memory.SharedMemory(name=name, create=False)
    if sys.platform != "win32":
        try:
            from multiprocessing import resource_tracker
            resource_tracker.unregister(shm._name, "shared_memory")  # type: ignore[attr-defined]
        except Exception:
            pass  # best effort: worst case is the pre-3.13 behaviour described above
    return shm


def _attach_with_retry(name: str, timeout_s: float) -> SpscQueueConsumer:
    """Waits for the producer to create and initialise the segment, so start
    order doesn't matter. A wrong-kind segment fails immediately (ValueError)."""
    deadline = time.monotonic() + timeout_s
    while True:
        try:
            return SpscQueueConsumer(name)
        except (FileNotFoundError, SegmentNotReady):
            if time.monotonic() >= deadline:
                raise
            time.sleep(0.01)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--name", default="animus_harness_shm", help="segment name harness_benchmark created")
    parser.add_argument("--events", type=int, default=10_000_000, help="event count to consume before stopping")
    parser.add_argument("--idle-timeout-s", type=float, default=2.0,
                         help="stop (and fail if short of --events) once the queue has stayed empty this long")
    parser.add_argument("--attach-timeout-s", type=float, default=10.0,
                         help="how long to wait for the producer to create the segment")
    args = parser.parse_args()

    try:
        ring = _attach_with_retry(args.name, args.attach_timeout_s)
    except (FileNotFoundError, SegmentNotReady):
        print(f"error: no usable shared-memory segment named '{args.name}' after {args.attach_timeout_s:.1f}s -- "
              f"start harness_benchmark --mode backpressure first (without --unlink-when-done)", file=sys.stderr)
        return 1
    except ValueError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    print("Animus Engine -- Cross-Process SHM Consumer")
    print("============================================")
    print(f"Segment name:   {args.name}")
    print(f"Queue capacity: {ring.capacity} slots (SpscQueue, lossless)")
    print(f"Target events:  {args.events}\n")

    consumed = 0
    gaps = 0
    # -1, not None: the producer numbers events from 0, so treating the first
    # record as following a virtual sequence -1 makes anything lost before this
    # reader's first record count as a gap too, instead of going unnoticed.
    last_sequence = -1
    integrity_ok = True
    idle_since = time.perf_counter()
    t_start = idle_since

    try_pop = ring.try_pop
    while consumed < args.events:
        rec = try_pop()
        if rec is None:
            if time.perf_counter() - idle_since > args.idle_timeout_s:
                print(f"\nQueue has been empty for {args.idle_timeout_s:.1f}s with the producer silent -- "
                      f"stopping at {consumed}/{args.events}.")
                break
            time.sleep(0.0001)  # brief backoff -- this is a Python consumer, not a native busy-spin
            continue
        idle_since = time.perf_counter()
        consumed += 1

        if rec.sequence <= last_sequence:
            print(f"INTEGRITY FAILURE: sequence went backwards or repeated "
                  f"({rec.sequence} after {last_sequence})", file=sys.stderr)
            integrity_ok = False
        else:
            gaps += rec.sequence - last_sequence - 1
        last_sequence = rec.sequence

    t_end = time.perf_counter()
    wall_seconds = t_end - t_start
    throughput = consumed / wall_seconds if wall_seconds > 0 else 0.0
    complete = consumed >= args.events

    print("Consumption summary:")
    print(f"  events consumed:     {consumed}")
    print(f"  sequence gaps seen:  {gaps} (lossless queue: must be 0)")
    print(f"  wall time:           {wall_seconds:.3f} s")
    print(f"  throughput:          {throughput:,.0f} ticks/sec ({throughput / 1_000_000:.3f} M ticks/sec)")
    print(f"  data integrity:      {'OK (monotonic, no repeats/reversals)' if integrity_ok else 'FAILED -- see above'}")
    if not complete:
        print(f"FAILURE: consumed {consumed} of {args.events} events", file=sys.stderr)

    ring.close()
    return 0 if (integrity_ok and gaps == 0 and complete) else 1


if __name__ == "__main__":
    raise SystemExit(main())
