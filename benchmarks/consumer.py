"""Animus Engine -- Cross-Process Shared-Memory Consumer (Milestone 4).

Consumer half of harness_benchmark.cpp's evaluation harness. Attaches to
the exact same named OS shared-memory segment
(animus::sys::ipc::ShmRing<ExecutionEvent>, include/animus/shm_ipc.hpp)
that binary creates and pushes into, decodes records directly with
`struct` against a hardcoded byte layout mirroring that C++ header
byte-for-byte, and drains it with no serialization step and no dependency
beyond the standard library -- consistent with this project's
zero-dependency ctypes/stdlib SDK path (animus/shm.py takes the same
"mirror the C++ layout by hand" approach for its own, different, wire
format; see that module's docstring for the same rationale applied there).

Layout assumption: this hardcodes ANIMUS_CACHE_LINE_SIZE == 64
(thread_affinity.hpp's default on every target except ARM64, where it's
128 -- see that header). harness_benchmark.cpp and this script must both
run on a 64-byte-cache-line target for the header offsets below to agree;
that covers essentially all x86_64 evaluation hardware this harness is
meant for.

Usage (after running harness_benchmark --name NAME, without --unlink-when-done):

    python consumer.py --name animus_harness_shm --events 10000000
"""
from __future__ import annotations

import argparse
import ctypes
import ctypes.wintypes
import os
import struct
import sys
import time
from multiprocessing import shared_memory
from typing import NamedTuple, Optional

# --- Header layout: must match ShmRing<ExecutionEvent>::Header exactly ------
# (include/animus/shm_ipc.hpp). Three 64-byte cache lines: read-only
# metadata, the producer's own line (head + everything only it writes),
# the consumer's own line (tail + everything only it writes).
_HEADER_SIZE = 192
_CAPACITY_OFF = 0
_MASK_OFF = 8
_HEAD_OFF = 64
_DROPPED_COUNT_OFF = 72
_PRODUCER_PID_OFF = 80
_PRODUCER_HEARTBEAT_OFF = 88
_TAIL_OFF = 128
_CONSUMER_PID_OFF = 136
_CONSUMER_HEARTBEAT_OFF = 144

# --- Record layout: must match harness_benchmark.cpp's ExecutionEvent ------
# sequence(u64), dispatch_ts_raw(u64), price_ticks(i64), quantity(i64),
# instrument_id(u32), flags(u32) -- 40 bytes, no padding (that file
# static_asserts this on the C++ side).
_RECORD_FORMAT = "<QQqqII"
_RECORD_SIZE = struct.calcsize(_RECORD_FORMAT)
assert _RECORD_SIZE == 40


class ExecutionEvent(NamedTuple):
    sequence: int
    dispatch_ts_raw: int
    price_ticks: int
    quantity: int
    instrument_id: int
    flags: int


def _read_u64(buf, offset: int) -> int:
    return struct.unpack_from("<Q", buf, offset)[0]


def _write_u64(buf, offset: int, value: int) -> None:
    struct.pack_into("<Q", buf, offset, value)


def is_process_alive(pid: int) -> bool:
    """Zero-dependency cross-platform pid liveness check -- the Python-side
    twin of animus::sys::lifecycle::is_process_alive (shm_lifecycle.hpp),
    used the same way here: to tell "the producer is gone" apart from "the
    producer is just behind", without depending on any wait that could
    itself hang if the producer never comes back.
    """
    if pid == 0:
        return False
    if sys.platform == "win32":
        PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
        STILL_ACTIVE = 259
        handle = ctypes.windll.kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
        if not handle:
            return False
        try:
            exit_code = ctypes.wintypes.DWORD()
            ok = ctypes.windll.kernel32.GetExitCodeProcess(handle, ctypes.byref(exit_code))
            return bool(ok) and exit_code.value == STILL_ACTIVE
        finally:
            ctypes.windll.kernel32.CloseHandle(handle)
    else:
        try:
            os.kill(pid, 0)
            return True
        except ProcessLookupError:
            return False
        except PermissionError:
            return True  # exists, just not ours to signal -- still alive


class ShmExecutionConsumer:
    """SPSC consumer over a ShmRing<ExecutionEvent> segment created by
    harness_benchmark.cpp. Consumer-side only: mirrors that struct's
    try_pop() exactly (relaxed tail read, acquire-equivalent head read,
    release-equivalent tail publish) -- plain 8-byte-aligned loads/stores
    are atomic on x86/x64 hardware, the same reasoning animus/shm.py's own
    SharedTelemetryRing documents for its unrelated wire format.
    """

    def __init__(self, name: str):
        self._shm = shared_memory.SharedMemory(name=name, create=False)
        self.capacity = _read_u64(self._shm.buf, _CAPACITY_OFF)
        self.mask = _read_u64(self._shm.buf, _MASK_OFF)
        _write_u64(self._shm.buf, _CONSUMER_PID_OFF, os.getpid())
        _write_u64(self._shm.buf, _CONSUMER_HEARTBEAT_OFF, 1)

    def heartbeat(self) -> None:
        hb = _read_u64(self._shm.buf, _CONSUMER_HEARTBEAT_OFF)
        _write_u64(self._shm.buf, _CONSUMER_HEARTBEAT_OFF, hb + 1)

    def producer_pid(self) -> int:
        return _read_u64(self._shm.buf, _PRODUCER_PID_OFF)

    def producer_alive(self) -> bool:
        pid = self.producer_pid()
        return pid == 0 or is_process_alive(pid)

    def dropped_count(self) -> int:
        return _read_u64(self._shm.buf, _DROPPED_COUNT_OFF)

    def pop(self) -> Optional[ExecutionEvent]:
        tail = _read_u64(self._shm.buf, _TAIL_OFF)
        head = _read_u64(self._shm.buf, _HEAD_OFF)
        if tail == head:
            return None
        slot = tail & self.mask
        offset = _HEADER_SIZE + slot * _RECORD_SIZE
        rec = struct.unpack_from(_RECORD_FORMAT, self._shm.buf, offset)
        _write_u64(self._shm.buf, _TAIL_OFF, tail + 1)
        return ExecutionEvent(*rec)

    def close(self) -> None:
        self._shm.close()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--name", default="animus_harness_shm", help="segment name harness_benchmark created")
    parser.add_argument("--events", type=int, default=10_000_000, help="target event count to consume before stopping")
    parser.add_argument("--idle-timeout-s", type=float, default=2.0,
                         help="stop early if the ring stays empty this long AND the producer has exited")
    args = parser.parse_args()

    try:
        ring = ShmExecutionConsumer(args.name)
    except FileNotFoundError:
        print(f"error: no shared-memory segment named '{args.name}' -- "
              f"start harness_benchmark first (without --unlink-when-done)", file=sys.stderr)
        return 1

    print("Animus Engine -- Cross-Process SHM Consumer")
    print("============================================")
    print(f"Segment name:   {args.name}")
    print(f"Ring capacity:  {ring.capacity} slots")
    print(f"Target events:  {args.events}")
    print(f"Producer pid:   {ring.producer_pid() or '(not yet attached)'}\n")

    consumed = 0
    gaps = 0
    last_sequence: Optional[int] = None
    integrity_ok = True
    idle_since: Optional[float] = None
    t_start = time.perf_counter()

    while consumed < args.events:
        rec = ring.pop()
        if rec is None:
            if not ring.producer_alive():
                if idle_since is None:
                    idle_since = time.perf_counter()
                elif time.perf_counter() - idle_since > args.idle_timeout_s:
                    print(f"\nProducer has exited and the ring has been empty for "
                          f"{args.idle_timeout_s:.1f}s -- stopping at {consumed}/{args.events} "
                          f"(the remainder were lost to overwrite, if the producer ran in that mode).")
                    break
            time.sleep(0.0001)  # brief backoff -- this is a Python consumer, not a native busy-spin
            continue
        idle_since = None
        consumed += 1
        ring.heartbeat()

        if last_sequence is not None:
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

    print("Consumption summary:")
    print(f"  events consumed:     {consumed}")
    print(f"  sequence gaps seen:  {gaps} (expected to match the producer's dropped_count under overwrite mode)")
    print(f"  producer dropped_count: {ring.dropped_count()}")
    print(f"  wall time:           {wall_seconds:.3f} s")
    print(f"  throughput:          {throughput:,.0f} ticks/sec ({throughput / 1_000_000:.3f} M ticks/sec)")
    print(f"  data integrity:      {'OK (monotonic, no repeats/reversals)' if integrity_ok else 'FAILED -- see above'}")

    ring.close()
    return 0 if integrity_ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
