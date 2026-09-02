"""Animus Engine -- idiomatic Python wrapper over the nanobind zero-copy
telemetry stream (bindings/animus_py.cpp).

The compiled extension (`_animus_native.TelemetryStream`) does the real
work: owns the lock-free SPSC ring, drains it into a scratch buffer it
reuses across calls, and hands back a zero-copy buffer-protocol view --
no per-event Python object construction, no heap allocation on that path,
no serialization. Everything in this module is a thin convenience layer
on top of that view: struct decoding, an idiomatic wrapper class, and an
optional numpy fast path. Only the stdlib `struct` module is required;
numpy is opt-in (see to_numpy()).
"""

from __future__ import annotations

import struct
from typing import Iterator, List, NamedTuple, Optional

# Matches animus::SharedTelemetryRecord (animus.hpp, static_assert'd there
# to 24 bytes) and animus.shm's own <QIIQ> wire record -- hardcoded here
# rather than read from the compiled extension so that decode()/
# decode_iter()/to_numpy()/TelemetryRecord stay usable (and testable)
# without _animus_native being built at all. Only TelemetryConsumer
# itself needs the extension, imported lazily in its __init__ -- see the
# comment there for why eagerly importing it at module level would make
# this whole module unimportable for anyone who hasn't built it yet.
WIRE_FORMAT = "<QIIQ"
WIRE_RECORD_SIZE = 24
_UNPACKER = struct.Struct(WIRE_FORMAT)
assert _UNPACKER.size == WIRE_RECORD_SIZE  # catches a WIRE_FORMAT/WIRE_RECORD_SIZE edit going out of sync


def _import_native():
    try:
        from . import _animus_native as native
    except ImportError as exc:
        raise ImportError(
            "animus.consumer.TelemetryConsumer requires the compiled "
            "_animus_native extension, which the base animus-engine-sdk "
            "install does not build (it stays on the zero-dependency "
            "ctypes path -- see animus/bindings.py). Build it locally: "
            "`pip install ./bindings` from a full source checkout (see "
            "bindings/CMakeLists.txt), or for fast iteration, the "
            "direct-CMake steps documented at the bottom of that file."
        ) from exc
    return native


class TelemetryRecord(NamedTuple):
    """One decoded telemetry event. Field order/types match
    animus::SharedTelemetryRecord (animus.hpp) and animus.shm's own wire
    format -- the same record shape used across every interop layer in
    this project, not a new one invented for this module."""
    timestamp_cycles: int
    event_id: int
    trace_id: int
    metric_value: int


def decode(view) -> List[TelemetryRecord]:
    """Unpack a drain() view into a list of TelemetryRecord.

    This makes one real copy (bytes(view)) to outlive the buffer's next
    drain()-triggered overwrite, then a further per-record unpack --
    convenient for small batches or debugging, not the hot path. For
    large batches prefer decode_iter() to avoid the list allocation, or
    to_numpy() for a genuinely zero-copy structured view.
    """
    return list(decode_iter(view))


def decode_iter(view) -> Iterator[TelemetryRecord]:
    """Yield TelemetryRecord one at a time from a drain() view.

    Makes one real copy up front (bytes(view)) -- necessary here because
    the source view aliases a scratch buffer the next drain() call
    overwrites, and this iterator may still be running when that happens.
    Each individual unpack thereafter operates on that owned copy, not
    the original C++ memory.
    """
    raw = bytes(view)
    for record in _UNPACKER.iter_unpack(raw):
        yield TelemetryRecord(*record)


def to_numpy(view):
    """Zero-copy structured numpy view over the exact same memory drain()
    returned -- no per-field decode, no copy, aliases the C++ scratch
    buffer directly (numpy.frombuffer over nanobind's buffer-protocol
    export). Subject to the same lifetime rule as the raw view: valid
    only until the next drain() call on the same TelemetryConsumer.

    Raises ImportError with a clear message if numpy isn't installed --
    this package does not depend on it.
    """
    try:
        import numpy as np
    except ImportError as exc:
        raise ImportError(
            "to_numpy() requires numpy, which this package does not "
            "depend on -- pip install numpy, or use decode()/decode_iter() "
            "for a dependency-free path."
        ) from exc
    dtype = np.dtype({
        "names": ["timestamp_cycles", "event_id", "trace_id", "metric_value"],
        "formats": ["<u8", "<u4", "<u4", "<u8"],
        "itemsize": WIRE_RECORD_SIZE,
    })
    return np.frombuffer(view, dtype=dtype)


class TelemetryConsumer:
    """Idiomatic wrapper over the native TelemetryStream.

    >>> stream = TelemetryConsumer(capacity=65536, drain_batch_capacity=4096)
    >>> stream.start_synthetic_load(event_count=1_000_000)
    >>> received = 0
    >>> while received < 1_000_000:
    ...     batch = stream.poll(max_count=4096)
    ...     received += len(batch)
    >>> stream.stop_synthetic_load()

    Not thread-safe beyond the SPSC contract the native ring itself
    enforces and does not check at runtime: exactly one thread may call
    drain()/poll() at a time, and it must not be the same thread driving
    push() or the background producer started by start_synthetic_load().
    """

    def __init__(self, capacity: int, drain_batch_capacity: int = 8192):
        self._native = _import_native().TelemetryStream(capacity, drain_batch_capacity)

    def __enter__(self) -> "TelemetryConsumer":
        return self

    def __exit__(self, *exc_info) -> None:
        self.stop_synthetic_load()

    def push(self, event_id: int, trace_id: int, metric_value: int) -> bool:
        """Push one event. Never blocks; False means the ring was full."""
        return self._native.push(event_id, trace_id, metric_value)

    def start_synthetic_load(self, event_count: int = 0, target_events_per_sec: float = 0.0) -> None:
        """Start a background native producer thread generating synthetic
        telemetry, for exercising Python-side consumption without wiring
        up a real second producer. event_count=0 runs until
        stop_synthetic_load(); target_events_per_sec<=0 is unthrottled."""
        self._native.start_producer(event_count, target_events_per_sec)

    def stop_synthetic_load(self) -> None:
        if self._native.producer_running():
            self._native.stop_producer()

    @property
    def producer_running(self) -> bool:
        return self._native.producer_running()

    def drain(self, max_count: Optional[int] = None):
        """Return a raw zero-copy buffer-protocol view of up to max_count
        drained events (defaults to drain_batch_capacity). Valid only
        until the next drain() call on this object -- decode it
        (decode(), decode_iter(), or to_numpy()) before draining again if
        the data needs to outlive that call."""
        limit = self.drain_batch_capacity if max_count is None else max_count
        return self._native.drain(limit)

    def poll(self, max_count: Optional[int] = None) -> List[TelemetryRecord]:
        """Convenience: drain() + decode() in one call. On a hot path
        where the list allocation here matters, call drain() and
        decode_iter() directly instead."""
        return decode(self.drain(max_count))

    @property
    def capacity(self) -> int:
        return self._native.capacity

    @property
    def drain_batch_capacity(self) -> int:
        return self._native.drain_batch_capacity

    @property
    def pushed_count(self) -> int:
        return self._native.pushed_count

    @property
    def dropped_count(self) -> int:
        return self._native.dropped_count
