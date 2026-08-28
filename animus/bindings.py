import os
import sys
import ctypes
from enum import IntEnum
from typing import List, Optional

def load_native_library():
    """Dynamically loads the compiled C++ dynamic library (.dll / .so)."""
    base_dir = os.path.dirname(os.path.abspath(__file__))
    
    if sys.platform.startswith("win32"):
        lib_name = "AnimusCore_v1.dll"
    elif sys.platform.startswith("linux"):
        lib_name = "libAnimusCore.so"
    elif sys.platform.startswith("darwin"):
        lib_name = "libAnimusCore.dylib"
    else:
        raise OSError(f"Unsupported platform: {sys.platform}")

    search_paths = [
        os.path.join(base_dir, lib_name),
        os.path.join(base_dir, "..", "x64", "Release", lib_name),
        os.path.join(base_dir, "..", "AnimusCore_v1", "x64", "Release", lib_name),
        os.path.join(base_dir, "..", lib_name),
    ]

    for path in search_paths:
        if os.path.exists(path):
            return ctypes.CDLL(os.path.abspath(path))
            
    raise FileNotFoundError(f"Could not locate native library {lib_name}. Ensure it is compiled in Release mode.")


class RuleComparator(IntEnum):
    """Mirrors animus::RuleComparator (animus.hpp) -- values must stay in sync."""
    GREATER_THAN = 0
    LESS_THAN = 1
    EQUAL = 2


class ThreatSignal(ctypes.Structure):
    """Mirrors animus::ThreatSignal (animus.hpp) byte-for-byte.

    Deliberately plain (no padding): ThreatSignal crosses the C-ABI via a
    caller-supplied buffer (animus_poll_signals), so this layout must match
    the native struct's natural 32-byte size exactly -- padding either side
    without mirroring it on the other would silently corrupt the buffer.
    """
    _fields_ = [
        ("timestamp_cycles", ctypes.c_uint64),
        ("event_id", ctypes.c_uint32),
        ("trace_id", ctypes.c_uint32),
        ("metric_value", ctypes.c_uint64),
        ("rule_id", ctypes.c_uint32),
        ("severity", ctypes.c_uint32),
    ]


class AnimusBindings:
    """Typed ctypes wrapper over the AnimusCore_v1 C-ABI.

    Calls go directly into the native LockFreeRingBuffer (see animus.hpp)
    with no intermediate serialization step, so per-event ingestion cost is
    the ctypes call-marshalling overhead plus one atomic ring-buffer push,
    not an IPC round trip.
    """

    def __init__(self, lib: Optional[ctypes.CDLL] = None) -> None:
        self._lib = lib if lib is not None else load_native_library()
        self._configure_signatures()
        self._initialized = False

    def _configure_signatures(self) -> None:
        self._lib.animus_init.argtypes = [ctypes.c_size_t]
        self._lib.animus_init.restype = ctypes.c_bool

        self._lib.animus_record_event.argtypes = [
            ctypes.c_uint32,
            ctypes.c_uint32,
            ctypes.c_uint64,
        ]
        self._lib.animus_record_event.restype = ctypes.c_bool

        self._lib.animus_start_logging.argtypes = [ctypes.c_char_p]
        self._lib.animus_start_logging.restype = None

        self._lib.animus_stop_logging.argtypes = []
        self._lib.animus_stop_logging.restype = None

        self._lib.animus_add_rule.argtypes = [
            ctypes.c_uint32,
            ctypes.c_uint32,
            ctypes.c_uint64,
            ctypes.c_uint8,
            ctypes.c_uint32,
        ]
        self._lib.animus_add_rule.restype = ctypes.c_bool

        self._lib.animus_poll_signals.argtypes = [
            ctypes.POINTER(ThreatSignal),
            ctypes.c_size_t,
        ]
        self._lib.animus_poll_signals.restype = ctypes.c_size_t

    def init(self, buffer_capacity: int = 65536) -> bool:
        """Initializes the native engine singleton. Idempotent."""
        if self._initialized:
            return True
        self._initialized = bool(self._lib.animus_init(ctypes.c_size_t(buffer_capacity)))
        return self._initialized

    def record_event(self, event_id: int, trace_id: int, metric_value: int) -> bool:
        """Pushes one telemetry event onto the native ring buffer.

        Returns False if the ring buffer is full (never blocks).
        """
        if not self._initialized:
            raise RuntimeError("AnimusBindings.init() must succeed before recording events")
        return bool(self._lib.animus_record_event(
            ctypes.c_uint32(event_id),
            ctypes.c_uint32(trace_id),
            ctypes.c_uint64(metric_value),
        ))

    def start_logging(self, filepath: str) -> None:
        """Starts the async background worker that drains the ring buffer to disk."""
        if not self._initialized:
            raise RuntimeError("AnimusBindings.init() must succeed before starting persistence")
        self._lib.animus_start_logging(filepath.encode("utf-8"))

    def stop_logging(self) -> None:
        """Stops the background worker after fully draining the ring buffer."""
        self._lib.animus_stop_logging()

    def add_rule(
        self,
        rule_id: int,
        event_id: int,
        threshold: int,
        comparator: "RuleComparator | int",
        severity: int,
    ) -> bool:
        """Registers an in-memory threshold rule evaluated against every
        ingested event carrying the given event_id (see
        EngineImpl::evaluate_rules in animus_engine.cpp). Returns False for
        an unrecognized comparator or if rule storage could not be grown.
        """
        if not self._initialized:
            raise RuntimeError("AnimusBindings.init() must succeed before adding rules")
        return bool(self._lib.animus_add_rule(
            ctypes.c_uint32(rule_id),
            ctypes.c_uint32(event_id),
            ctypes.c_uint64(threshold),
            ctypes.c_uint8(int(comparator)),
            ctypes.c_uint32(severity),
        ))

    def poll_signals(self, max_count: int = 1024) -> List[ThreatSignal]:
        """Drains up to max_count pending rule matches from the native
        signal ring. Never blocks; returns fewer than max_count (including
        zero) if fewer signals are currently pending.
        """
        if not self._initialized:
            raise RuntimeError("AnimusBindings.init() must succeed before polling signals")
        buf = (ThreatSignal * max_count)()
        count = self._lib.animus_poll_signals(buf, ctypes.c_size_t(max_count))
        return list(buf[:count])
