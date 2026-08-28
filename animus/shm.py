"""OS-level shared-memory IPC for cross-process telemetry exchange.

AnimusBindings.record_event() pushes into AnimusCore_v1.dll's in-process
LockFreeRingBuffer (animus.hpp) -- fast, but invisible outside the process
that loaded the DLL, since ctypes gives each process its own copy of the
DLL's static state. SharedTelemetryRing complements that with a ring buffer
that lives in an OS-level shared memory segment (multiprocessing.shared_memory,
stdlib-only per CLAUDE.md's zero-dependency SDK constraint), so a separate
producer process and consumer process can exchange TelemetryPayload-shaped
records with no serialization step: both sides pack/unpack fixed-offset
struct fields directly into the same physical memory.

This is a single-producer/single-consumer (SPSC) ring, not the native
engine's lock-free MPMC: head is written only by the producer and tail only
by the consumer, so plain 8-byte-aligned loads/stores (atomic on x86/x64
hardware, and every offset here is a multiple of 8) are sufficient without
a CAS loop. Do not share one SharedTelemetryRing across multiple producer
or multiple consumer processes -- use AnimusBindings for that.

Header and record fields are read/written via struct.pack_into/unpack_from
directly against the segment's memoryview rather than ctypes.from_buffer:
a ctypes object built with from_buffer holds a live export on the
underlying buffer for as long as that object exists, and multiprocessing.
shared_memory.SharedMemory.close() raises BufferError while any such
export is outstanding -- struct's pack_into/unpack_from touch the buffer
only for the duration of the call, so nothing is left pinning it open.
"""
import struct
import time
from multiprocessing import shared_memory
from typing import NamedTuple, Optional

# capacity, head, tail -- all uint64, little-endian, unpadded.
_HEADER_FORMAT = "<QQQ"
_HEADER_SIZE = struct.calcsize(_HEADER_FORMAT)
_HEAD_OFFSET = 8
_TAIL_OFFSET = 16

# timestamp_cycles(u64), event_id(u32), trace_id(u32), metric_value(u64) --
# mirrors animus::TelemetryPayload's logical fields (animus.hpp), without
# TelemetryPayload's 64-byte cache-line padding: that padding exists to
# avoid false sharing between concurrent native producer *threads* on
# adjacent ring cells, which doesn't apply here since head/tail already
# give every slot exactly one writer.
_RECORD_FORMAT = "<QIIQ"
_RECORD_SIZE = struct.calcsize(_RECORD_FORMAT)


class TelemetryRecordView(NamedTuple):
    timestamp_cycles: int
    event_id: int
    trace_id: int
    metric_value: int


class SharedTelemetryRing:
    """A fixed-capacity SPSC telemetry ring backed by named shared memory.

    One process calls create() to allocate and own the segment; any other
    process on the same machine calls attach() with the same name to map
    the identical bytes. push()/pop() never block: push() returns False
    when the ring is full, pop() returns None when it's empty.
    """

    def __init__(self, shm: "shared_memory.SharedMemory", capacity: int, owner: bool) -> None:
        self._shm = shm
        self._owner = owner
        if owner:
            struct.pack_into(_HEADER_FORMAT, self._shm.buf, 0, capacity, 0, 0)
            self._capacity = capacity
        else:
            self._capacity, _, _ = struct.unpack_from(_HEADER_FORMAT, self._shm.buf, 0)

    @classmethod
    def create(cls, name: str, capacity: int) -> "SharedTelemetryRing":
        """Allocates a new named shared memory segment sized for `capacity`
        records and takes ownership of it (unlink() destroys the underlying
        OS object; only the owner should call it).
        """
        if capacity < 1:
            raise ValueError("capacity must be >= 1")
        size = _HEADER_SIZE + capacity * _RECORD_SIZE
        shm = shared_memory.SharedMemory(name=name, create=True, size=size)
        return cls(shm, capacity, owner=True)

    @classmethod
    def attach(cls, name: str) -> "SharedTelemetryRing":
        """Maps an existing segment created by another process via create()."""
        shm = shared_memory.SharedMemory(name=name, create=False)
        return cls(shm, capacity=0, owner=False)

    @property
    def capacity(self) -> int:
        return self._capacity

    @property
    def name(self) -> str:
        return self._shm.name

    def _head_tail(self) -> "tuple[int, int]":
        _, head, tail = struct.unpack_from(_HEADER_FORMAT, self._shm.buf, 0)
        return head, tail

    def push(
        self,
        event_id: int,
        trace_id: int,
        metric_value: int,
        timestamp_cycles: Optional[int] = None,
    ) -> bool:
        """Writes one record directly into shared memory. Producer-only."""
        head, tail = self._head_tail()
        if head - tail >= self._capacity:
            return False
        slot = head % self._capacity
        offset = _HEADER_SIZE + slot * _RECORD_SIZE
        ts = timestamp_cycles if timestamp_cycles is not None else time.perf_counter_ns()
        struct.pack_into(_RECORD_FORMAT, self._shm.buf, offset, ts, event_id, trace_id, metric_value)
        struct.pack_into("<Q", self._shm.buf, _HEAD_OFFSET, head + 1)
        return True

    def pop(self) -> Optional[TelemetryRecordView]:
        """Reads and removes the oldest pending record. Consumer-only."""
        head, tail = self._head_tail()
        if tail == head:
            return None
        slot = tail % self._capacity
        offset = _HEADER_SIZE + slot * _RECORD_SIZE
        ts, event_id, trace_id, metric_value = struct.unpack_from(_RECORD_FORMAT, self._shm.buf, offset)
        struct.pack_into("<Q", self._shm.buf, _TAIL_OFFSET, tail + 1)
        return TelemetryRecordView(ts, event_id, trace_id, metric_value)

    def __len__(self) -> int:
        head, tail = self._head_tail()
        return head - tail

    def close(self) -> None:
        """Releases this process's mapping. Safe to call from any side."""
        self._shm.close()

    def unlink(self) -> None:
        """Destroys the underlying OS shared memory object. Owner-only --
        call after every attached consumer has closed its mapping.
        """
        self._shm.unlink()
