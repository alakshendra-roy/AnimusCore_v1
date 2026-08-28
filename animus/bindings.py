"""ctypes bindings over the native Animus engine, with a pure-Python fallback.

Two native binaries can satisfy this module, checked in this priority order:
  1. AnimusNative.dll / libanimus_native.so / libanimus_native.dylib -- the
     portable CMake build (see CMakeLists.txt), buildable on any platform
     with a C++17 compiler.
  2. AnimusCore_v1.dll / libAnimusCore.so / libAnimusCore.dylib -- the
     original MSVC-only vcxproj build (AnimusCore_v1.vcxproj).
Both export the identical extern "C" surface declared at the bottom of
animus.hpp (animus_init, animus_record_event, ...), so AnimusBindings talks
to whichever one is found without caring which it is.

Dynamic loading uses ctypes exclusively, not cffi: ctypes is part of the
Python 3.8+ standard library, matching this SDK's zero-dependency
constraint (CLAUDE.md; see also pyproject.toml's `dependencies = []`),
whereas cffi is a third-party package that would need to be vendored or
installed. When neither binary can be found -- no C++ toolchain available,
or simply not built yet -- AnimusBindings transparently falls back to
_PurePythonEngine, a same-contract, pure-Python reimplementation, so
callers (animus.core.EventEngine, animus.decorators.trace) keep working
without a native binary at reduced throughput rather than failing outright.
"""
import collections
import os
import struct
import sys
import threading
import time
import ctypes
from enum import IntEnum
from typing import Deque, List, NamedTuple, Optional

_NATIVE_LIB_NAMES = {
    # (preferred: portable CMake build, legacy: MSVC-only vcxproj build)
    "win32": ("AnimusNative.dll", "AnimusCore_v1.dll"),
    "linux": ("libanimus_native.so", "libAnimusCore.so"),
    "darwin": ("libanimus_native.dylib", "libAnimusCore.dylib"),
}


def _platform_key() -> str:
    if sys.platform.startswith("win32"):
        return "win32"
    if sys.platform.startswith("linux"):
        return "linux"
    if sys.platform.startswith("darwin"):
        return "darwin"
    raise OSError(f"Unsupported platform: {sys.platform}")


def find_native_library() -> Optional[str]:
    """Searches known build-output locations for a compiled native engine.

    Checks AnimusNative/libanimus_native (CMakeLists.txt's build/ and
    build/Release/) before the legacy AnimusCore_v1 vcxproj output
    locations, and the animus/ package directory itself first of all (an
    installed wheel bundles the binary there -- see setup.py). Returns the
    first match's absolute path, or None if nothing is found anywhere
    searched.
    """
    base_dir = os.path.dirname(os.path.abspath(__file__))
    repo_root = os.path.join(base_dir, "..")
    lib_names = _NATIVE_LIB_NAMES[_platform_key()]

    search_dirs = [
        base_dir,
        os.path.join(repo_root, "build"),
        os.path.join(repo_root, "build", "Release"),
        os.path.join(repo_root, "x64", "Release"),
        os.path.join(repo_root, "AnimusCore_v1", "x64", "Release"),
        repo_root,
    ]

    for lib_name in lib_names:
        for directory in search_dirs:
            candidate = os.path.join(directory, lib_name)
            if os.path.exists(candidate):
                return os.path.abspath(candidate)
    return None


def load_native_library(required: bool = True) -> Optional[ctypes.CDLL]:
    """Dynamically loads the compiled native engine via ctypes.CDLL.

    If `required` is True (the default, preserving this function's original
    contract for any caller that wants a hard failure), raises
    FileNotFoundError when no compiled binary can be found. If False,
    returns None instead -- this is what AnimusBindings uses internally to
    decide whether to fall back to the pure-Python engine.
    """
    path = find_native_library()
    if path is None:
        if required:
            raise FileNotFoundError(
                "Could not locate a compiled native engine (AnimusNative.* or "
                "AnimusCore_v1.*). Build either CMakeLists.txt "
                "(cmake -S . -B build && cmake --build build) or "
                "AnimusCore_v1.slnx (MSVC) first, or construct "
                "AnimusBindings() directly to use the automatic "
                "pure-Python fallback instead of raising."
            )
        return None
    return ctypes.CDLL(path)


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
    _PurePythonEngine also constructs these (via keyword args, not across
    any C-ABI boundary) so poll_signals() returns the same type regardless
    of which engine is backing it.
    """
    _fields_ = [
        ("timestamp_cycles", ctypes.c_uint64),
        ("event_id", ctypes.c_uint32),
        ("trace_id", ctypes.c_uint32),
        ("metric_value", ctypes.c_uint64),
        ("rule_id", ctypes.c_uint32),
        ("severity", ctypes.c_uint32),
    ]


class _TelemetryRecord(NamedTuple):
    """Mirrors animus::TelemetryPayload's logical fields (animus.hpp),
    without its 64-byte cache-line padding -- see shm.py's identical
    rationale for _RECORD_FORMAT, which this reuses for on-disk layout.
    """
    timestamp_cycles: int
    event_id: int
    trace_id: int
    metric_value: int


class _RuleThreshold(NamedTuple):
    rule_id: int
    event_id: int
    threshold: int
    comparator: int
    severity: int


class _BoundedQueue:
    """Lock-guarded, fixed-capacity FIFO used by _PurePythonEngine in place
    of animus::LockFreeRingBuffer. push() never blocks and returns False
    once `capacity` items are pending (matching the native ring's
    never-blocks/bounded-capacity contract) rather than silently evicting
    the oldest entry the way a deque(maxlen=...) would.
    """

    def __init__(self, capacity: int) -> None:
        self._capacity = capacity
        self._items: Deque = collections.deque()
        self._lock = threading.Lock()

    def push(self, item) -> bool:
        with self._lock:
            if len(self._items) >= self._capacity:
                return False
            self._items.append(item)
            return True

    def pop_batch(self, max_count: int) -> list:
        with self._lock:
            out = []
            while self._items and len(out) < max_count:
                out.append(self._items.popleft())
            return out

    def __len__(self) -> int:
        with self._lock:
            return len(self._items)


class _PurePythonEngine:
    """Pure-Python stand-in for the native engine's C-ABI surface, used by
    AnimusBindings when no compiled AnimusNative/AnimusCore_v1 binary is
    available.

    Reproduces EngineImpl's (animus_engine.cpp) observable contract --
    never-blocking bounded ingestion, a background worker that drains
    telemetry to disk while evaluating threshold rules, a separate signal
    queue for matches -- with a lock-guarded deque standing in for the
    lock-free ring buffer. The goal is correctness and API parity so
    animus.trace/EventEngine keep working on a machine with no C++
    toolchain, not to match the native engine's throughput.
    """

    def __init__(self) -> None:
        self._ring: Optional[_BoundedQueue] = None
        self._signals: Optional[_BoundedQueue] = None
        self._rules: List[_RuleThreshold] = []
        self._rules_lock = threading.Lock()
        self._running = False
        self._worker: Optional[threading.Thread] = None
        self._log_path: Optional[str] = None

    def init(self, buffer_capacity: int) -> bool:
        if self._ring is None:
            self._ring = _BoundedQueue(buffer_capacity)
            self._signals = _BoundedQueue(buffer_capacity)
        return True

    def record_event(self, event_id: int, trace_id: int, metric_value: int) -> bool:
        record = _TelemetryRecord(time.perf_counter_ns(), event_id, trace_id, metric_value)
        return self._ring.push(record)

    def add_rule(self, rule_id: int, event_id: int, threshold: int, comparator: int, severity: int) -> bool:
        if comparator not in (
            RuleComparator.GREATER_THAN,
            RuleComparator.LESS_THAN,
            RuleComparator.EQUAL,
        ):
            return False
        with self._rules_lock:
            self._rules.append(_RuleThreshold(rule_id, event_id, threshold, comparator, severity))
        return True

    def _evaluate(self, record: _TelemetryRecord) -> None:
        with self._rules_lock:
            rules_snapshot = list(self._rules)
        for rule in rules_snapshot:
            if rule.event_id != record.event_id:
                continue
            if rule.comparator == RuleComparator.GREATER_THAN:
                matched = record.metric_value > rule.threshold
            elif rule.comparator == RuleComparator.LESS_THAN:
                matched = record.metric_value < rule.threshold
            else:
                matched = record.metric_value == rule.threshold
            if matched:
                self._signals.push(ThreatSignal(
                    timestamp_cycles=record.timestamp_cycles,
                    event_id=record.event_id,
                    trace_id=record.trace_id,
                    metric_value=record.metric_value,
                    rule_id=rule.rule_id,
                    severity=rule.severity,
                ))

    def start_logging(self, filepath: str) -> None:
        if self._running:
            return
        self._log_path = filepath
        self._running = True
        self._worker = threading.Thread(target=self._drain_loop, daemon=True)
        self._worker.start()

    def _drain_loop(self) -> None:
        with open(self._log_path, "ab") as fh:
            while True:
                batch = self._ring.pop_batch(1024)
                for record in batch:
                    self._evaluate(record)
                    fh.write(struct.pack("<QIIQ", *record))
                if batch:
                    fh.flush()
                    continue
                if not self._running:
                    break
                time.sleep(0.0005)

    def stop_logging(self) -> None:
        if not self._running:
            return
        self._running = False
        if self._worker is not None:
            self._worker.join()
            self._worker = None

    def poll_signals(self, max_count: int) -> List[ThreatSignal]:
        return self._signals.pop_batch(max_count)


class AnimusBindings:
    """Typed ctypes wrapper over the AnimusNative / AnimusCore_v1 C-ABI.

    When a compiled native binary is available, calls go directly into the
    native LockFreeRingBuffer (see animus.hpp) with no intermediate
    serialization step, so per-event ingestion cost is the ctypes
    call-marshalling overhead plus one atomic ring-buffer push, not an IPC
    round trip. When no binary is found, falls back transparently to
    _PurePythonEngine (see module docstring) -- every method below behaves
    identically either way; use `using_native_engine` to inspect which
    backend is active.
    """

    def __init__(self, lib: Optional[ctypes.CDLL] = None) -> None:
        self._fallback: Optional[_PurePythonEngine] = None
        if lib is not None:
            self._lib = lib
        else:
            self._lib = load_native_library(required=False)

        if self._lib is not None:
            self._configure_signatures()
        else:
            print(
                "[animus.bindings] WARNING: no compiled native engine found "
                "(AnimusNative.* / AnimusCore_v1.*); falling back to the "
                "pure-Python engine (reduced throughput, no zero-copy "
                "poll_signals)."
            )
            self._fallback = _PurePythonEngine()
        self._initialized = False

    @property
    def using_native_engine(self) -> bool:
        """True if backed by the compiled C++ engine, False if running on
        the pure-Python fallback."""
        return self._lib is not None

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
        """Initializes the engine (native singleton, or pure-Python fallback). Idempotent."""
        if self._initialized:
            return True
        if self.using_native_engine:
            self._initialized = bool(self._lib.animus_init(ctypes.c_size_t(buffer_capacity)))
        else:
            self._initialized = self._fallback.init(buffer_capacity)
        return self._initialized

    def record_event(self, event_id: int, trace_id: int, metric_value: int) -> bool:
        """Pushes one telemetry event onto the ring buffer.

        Returns False if the ring buffer is full (never blocks).
        """
        if not self._initialized:
            raise RuntimeError("AnimusBindings.init() must succeed before recording events")
        if self.using_native_engine:
            return bool(self._lib.animus_record_event(
                ctypes.c_uint32(event_id),
                ctypes.c_uint32(trace_id),
                ctypes.c_uint64(metric_value),
            ))
        return self._fallback.record_event(event_id, trace_id, metric_value)

    def start_logging(self, filepath: str) -> None:
        """Starts the async background worker that drains the ring buffer to disk."""
        if not self._initialized:
            raise RuntimeError("AnimusBindings.init() must succeed before starting persistence")
        if self.using_native_engine:
            self._lib.animus_start_logging(filepath.encode("utf-8"))
        else:
            self._fallback.start_logging(filepath)

    def stop_logging(self) -> None:
        """Stops the background worker after fully draining the ring buffer."""
        if self.using_native_engine:
            self._lib.animus_stop_logging()
        elif self._fallback is not None:
            self._fallback.stop_logging()

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
        EngineImpl::evaluate_rules in animus_engine.cpp, or
        _PurePythonEngine._evaluate for the fallback equivalent). Returns
        False for an unrecognized comparator or if rule storage could not
        be grown.
        """
        if not self._initialized:
            raise RuntimeError("AnimusBindings.init() must succeed before adding rules")
        if self.using_native_engine:
            return bool(self._lib.animus_add_rule(
                ctypes.c_uint32(rule_id),
                ctypes.c_uint32(event_id),
                ctypes.c_uint64(threshold),
                ctypes.c_uint8(int(comparator)),
                ctypes.c_uint32(severity),
            ))
        return self._fallback.add_rule(rule_id, event_id, threshold, int(comparator), severity)

    def poll_signals(self, max_count: int = 1024) -> List[ThreatSignal]:
        """Drains up to max_count pending rule matches from the signal ring.
        Never blocks; returns fewer than max_count (including zero) if
        fewer signals are currently pending.

        On the native engine this is zero-copy: `buf` below is a single
        contiguous ctypes array allocated once in this process, and its
        pointer is handed directly to animus_poll_signals, which writes
        matched ThreatSignal records straight into that memory -- there is
        no intermediate serialization/deserialization step, and the list
        returned is built from slicing that same buffer.
        """
        if not self._initialized:
            raise RuntimeError("AnimusBindings.init() must succeed before polling signals")
        if self.using_native_engine:
            buf = (ThreatSignal * max_count)()
            count = self._lib.animus_poll_signals(buf, ctypes.c_size_t(max_count))
            return list(buf[:count])
        return self._fallback.poll_signals(max_count)
