"""AnimusConsumer -- idiomatic Python wrapper over the nanobind zero-copy
TelemetryFrame stream (csrc/animus_ring_buffer.cpp).

The compiled extension does the real work: drains the lock-free SPSC ring
into a scratch buffer it reuses across calls, and hands back a zero-copy
buffer-protocol view -- no per-frame Python object construction, no heap
allocation on that path, no serialization. Everything in this module is a
thin convenience layer on top of that view: struct decoding, an idiomatic
wrapper class, and an optional NumPy fast path. Only the stdlib `struct`
module is required; numpy is opt-in (see to_numpy()).
"""

from __future__ import annotations

import struct
from typing import Iterator, List, NamedTuple, Optional

from .ring_buffer import AnimusRingBuffer

# Matches animus::eval::TelemetryFrame (animus-eval-kit/include/telemetry_frame.hpp,
# static_assert'd there to 64 bytes) exactly, including its 27 bytes of
# trailing alignas(64) padding -- the "27x" pads the format's calcsize out
# to the full 64-byte stride between frames without producing a value in
# the unpacked tuple. Hardcoded here (matching kTelemetryFrameWireFormat)
# rather than read from the compiled extension so decode()/decode_iter()/
# to_numpy()/TelemetryFrame stay usable and testable without
# _animus_sdk_native being built at all.
WIRE_FORMAT = "<QQ8sdIB27x"
WIRE_RECORD_SIZE = 64
_UNPACKER = struct.Struct(WIRE_FORMAT)
assert _UNPACKER.size == WIRE_RECORD_SIZE  # catches a WIRE_FORMAT/WIRE_RECORD_SIZE edit going out of sync


class TelemetryFrame(NamedTuple):
    """One decoded telemetry frame. Field order/types match
    animus::eval::TelemetryFrame -- the same 64-byte layout animus_bench
    itself pushes and measures."""
    sequence_id: int
    timestamp_ns: int
    symbol: bytes
    price: float
    volume: int
    flags: int


def decode(view) -> List[TelemetryFrame]:
    """Unpack a drain()/poll() view into a list of TelemetryFrame.

    Makes one real copy (bytes(view)) to outlive the buffer's next
    drain()-triggered overwrite, then a further per-record unpack --
    convenient for small batches or debugging, not the hot path. For large
    batches prefer decode_iter() to avoid the list allocation, or
    to_numpy() for a genuinely zero-copy structured view.
    """
    return list(decode_iter(view))


def decode_iter(view) -> Iterator[TelemetryFrame]:
    """Yield TelemetryFrame one at a time from a drain()/poll() view.

    Makes one real copy up front (bytes(view)) -- necessary here because
    the source view aliases a scratch buffer the next drain()/poll() call
    overwrites, and this iterator may still be running when that happens.
    """
    raw = bytes(view)
    for record in _UNPACKER.iter_unpack(raw):
        yield TelemetryFrame(*record)


def to_numpy(view):
    """Zero-copy structured NumPy view over the exact same memory
    drain()/poll() returned -- no per-field decode, no copy, aliases the
    C++ scratch buffer directly (numpy.frombuffer over nanobind's
    buffer-protocol export). Subject to the same lifetime rule as the raw
    view: valid only until the next drain()/poll() call on the same
    AnimusConsumer.

    This is the path bench_python_throughput.py drives to hit multi-
    million-frame-per-second ingestion: everything downstream of drain()
    is a strided reinterpretation of already-resident memory, not a copy.

    Raises ImportError with a clear message if numpy isn't installed --
    this package's core dependency footprint stays at zero.
    """
    try:
        import numpy as np
    except ImportError as exc:
        raise ImportError(
            "to_numpy() requires numpy (pip install 'animus-py[numpy]', or "
            "just `pip install numpy`) -- use decode()/decode_iter() for a "
            "dependency-free path."
        ) from exc
    dtype = np.dtype({
        "names": ["sequence_id", "timestamp_ns", "symbol", "price", "volume", "flags"],
        "formats": ["<u8", "<u8", "S8", "<f8", "<u4", "u1"],
        "offsets": [0, 8, 16, 24, 32, 36],
        "itemsize": WIRE_RECORD_SIZE,
    })
    return np.frombuffer(view, dtype=dtype)


class AnimusConsumer:
    """Idiomatic zero-copy consumer over an AnimusRingBuffer.

    >>> ring = AnimusRingBuffer(capacity=1 << 20)
    >>> stream = AnimusConsumer(ring)
    >>> ring.start_synthetic_load(frame_count=1_000_000)
    >>> received = 0
    >>> while received < 1_000_000:
    ...     frames = stream.to_numpy(stream.drain(max_count=4096))
    ...     received += len(frames)
    >>> ring.stop_synthetic_load()

    Not thread-safe beyond the SPSC contract the native ring itself
    enforces: exactly one thread may call drain()/poll() at a time, and it
    must not be the same thread driving push() or the background producer.
    """

    def __init__(self, ring: AnimusRingBuffer):
        self._ring = ring
        self._native = ring._native  # the compiled RingBuffer -- drain()/poll() live here

    @property
    def ring(self) -> AnimusRingBuffer:
        return self._ring

    def poll(self, max_count: Optional[int] = None, max_spins: int = 200_000):
        """Spin-wait (native side, GIL released) up to max_spins times per
        frame for the producer to catch up, returning a raw zero-copy
        buffer-protocol view of up to max_count frames actually popped.
        Valid only until the next drain()/poll() call -- decode it
        (decode(), decode_iter(), or to_numpy()) before polling again if
        the data needs to outlive that call."""
        limit = self._ring.drain_batch_capacity if max_count is None else max_count
        return self._native.poll(limit, max_spins)

    def drain(self, max_count: Optional[int] = None):
        """Non-blocking: pop only what's immediately available, up to
        max_count. The hot ingestion-loop call -- see bench_python_throughput.py."""
        limit = self._ring.drain_batch_capacity if max_count is None else max_count
        return self._native.drain(limit)

    def to_numpy(self, view):
        """Convenience: module-level to_numpy(), bound here so a caller
        holding only an AnimusConsumer doesn't need a separate import."""
        return to_numpy(view)

    def read(self, max_count: Optional[int] = None) -> List[TelemetryFrame]:
        """Convenience: drain() + decode() in one call. On a hot path
        where the list allocation here matters, call drain() and
        decode_iter() (or to_numpy()) directly instead."""
        return decode(self.drain(max_count))
