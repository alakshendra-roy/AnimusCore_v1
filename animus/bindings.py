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

from .shm import TelemetryRecordView

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


class WindowType(IntEnum):
    """Mirrors animus::WindowType (animus.hpp) -- values must stay in sync.
    COUNT: window_size is an event count (the last N matching events).
    TIME: window_size is milliseconds (events within the last N ms).
    """
    COUNT = 0
    TIME = 1


class AggregationFunction(IntEnum):
    """Mirrors animus::AggregationFunction (animus.hpp) -- values must stay in sync."""
    SUM = 0
    AVG = 1
    MIN = 2
    MAX = 3


class BookSide(IntEnum):
    """Mirrors animus::BookSide (animus.hpp) -- values must stay in sync."""
    BID = 0
    ASK = 1


class BookUpdateAction(IntEnum):
    """Mirrors animus::BookUpdateAction (animus.hpp) -- values must stay in sync.
    NEW: a fresh price level entered the book at `level`. UPDATE: that
    level's quantity changed. DELETE: the level was removed (quantity is
    not meaningful and should be ignored).
    """
    NEW = 0
    UPDATE = 1
    DELETE = 2


class TradeAggressor(IntEnum):
    """Mirrors animus::TradeAggressor (animus.hpp) -- values must stay in sync."""
    BUYER = 0
    SELLER = 1
    UNKNOWN = 2


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


class NativeEvent(ctypes.Structure):
    """Mirrors animus::RawEvent (animus.hpp) byte-for-byte: the input record
    for animus_record_events_batch. 16 bytes, naturally aligned (no padding
    needed -- two uint32 at offsets 0/4, one uint64 at offset 8).
    """
    _fields_ = [
        ("event_id", ctypes.c_uint32),
        ("trace_id", ctypes.c_uint32),
        ("metric_value", ctypes.c_uint64),
    ]


# struct.pack format mirroring NativeEvent's layout field-for-field, used by
# AnimusBindings.record_events_batch to fill a (NativeEvent * N) array via a
# single struct.pack()+memmove() per batch rather than constructing N
# NativeEvent Python objects: allocating a ctypes.Structure instance per
# event dominates batch-call cost (measured ~6x the packed-bytes approach
# at 100k events), since it pays Python object-creation overhead N times
# for what is, on the wire, a fixed-size binary record.
_NATIVE_EVENT_FORMAT = "<IIQ"
assert struct.calcsize(_NATIVE_EVENT_FORMAT) == ctypes.sizeof(NativeEvent)


class SpscTelemetryRecord(ctypes.Structure):
    """Mirrors animus::TelemetryPayload (animus.hpp) byte-for-byte, INCLUDING
    its `alignas(64)` cache-line padding -- unlike _TelemetryRecord below
    (which mirrors only the logical fields, for the pure-Python fallback's
    in-memory use), this one crosses the real C-ABI via animus_spsc_drain's
    caller-supplied buffer, so its size must match the native array's actual
    per-element stride exactly. Getting this wrong doesn't raise an error --
    it silently reads every record at the wrong offset, which is exactly
    what happened (visibly garbled field values) before this padding was
    added and verified against a real push/drain round trip.
    """
    _fields_ = [
        ("timestamp_cycles", ctypes.c_uint64),
        ("event_id", ctypes.c_uint32),
        ("trace_id", ctypes.c_uint32),
        ("metric_value", ctypes.c_uint64),
        ("_reserved", ctypes.c_uint8 * (64 - 24)),  # pad 24 real bytes out to alignas(64)
    ]


assert ctypes.sizeof(SpscTelemetryRecord) == 64


class SharedRecord(ctypes.Structure):
    """Mirrors animus::SharedTelemetryRecord (animus.hpp) byte-for-byte --
    24 bytes, no padding (unlike SpscTelemetryRecord above: the shared-
    memory wire format is deliberately NOT alignas(64), so it stays
    byte-for-byte identical to animus.shm's own on-disk `_RECORD_FORMAT`
    ("<QIIQ") and the two can interoperate on the same segment). Crosses
    the real C-ABI via animus_shm_pop's caller-supplied buffer.
    """
    _fields_ = [
        ("timestamp_cycles", ctypes.c_uint64),
        ("event_id", ctypes.c_uint32),
        ("trace_id", ctypes.c_uint32),
        ("metric_value", ctypes.c_uint64),
    ]


assert ctypes.sizeof(SharedRecord) == 24


class L2Update(ctypes.Structure):
    """Mirrors animus::L2Update (animus.hpp) byte-for-byte, INCLUDING its 6
    bytes of trailing alignment padding -- crosses the real C-ABI via
    animus_feed_poll_l2_updates's caller-supplied buffer, so this layout
    must match the native array's actual per-element stride exactly (see
    SpscTelemetryRecord's identical rationale above). side/action are
    raw ctypes.c_uint8, not BookSide/BookUpdateAction directly -- wrap
    with BookSide(rec.side) / BookUpdateAction(rec.action) if you want the
    enum, same convention ThreatSignal.rule_id etc. already follow.
    """
    _fields_ = [
        ("timestamp_cycles", ctypes.c_uint64),
        ("exchange_timestamp_ns", ctypes.c_uint64),
        ("sequence_number", ctypes.c_uint64),
        ("price_ticks", ctypes.c_uint64),
        ("quantity", ctypes.c_uint64),
        ("instrument_id", ctypes.c_uint32),
        ("level", ctypes.c_uint32),
        ("side", ctypes.c_uint8),
        ("action", ctypes.c_uint8),
        ("_reserved", ctypes.c_uint8 * 6),
    ]


assert ctypes.sizeof(L2Update) == 56


class TradeTick(ctypes.Structure):
    """Mirrors animus::TradeTick (animus.hpp) byte-for-byte, INCLUDING its 3
    bytes of trailing alignment padding -- same rationale as L2Update above.
    """
    _fields_ = [
        ("timestamp_cycles", ctypes.c_uint64),
        ("exchange_timestamp_ns", ctypes.c_uint64),
        ("sequence_number", ctypes.c_uint64),
        ("trade_id", ctypes.c_uint64),
        ("price_ticks", ctypes.c_uint64),
        ("quantity", ctypes.c_uint64),
        ("instrument_id", ctypes.c_uint32),
        ("aggressor_side", ctypes.c_uint8),
        ("_reserved", ctypes.c_uint8 * 3),
    ]


assert ctypes.sizeof(TradeTick) == 56


class OrderSide(IntEnum):
    """Mirrors animus::OrderSide (animus.hpp) -- values must stay in sync."""
    BUY = 0
    SELL = 1


class OrderType(IntEnum):
    """Mirrors animus::OrderType (animus.hpp) -- values must stay in sync."""
    MARKET = 0
    LIMIT = 1


class ExecStatus(IntEnum):
    """Mirrors animus::ExecStatus (animus.hpp) -- values must stay in sync."""
    ACCEPTED = 0
    REJECTED = 1
    FILLED = 2
    PARTIALLY_FILLED = 3


class Role(IntEnum):
    """Mirrors animus::security::Role (animus_security.hpp) -- values must
    stay in sync. Viewer: read-only (poll signals). Operator: Viewer plus
    ingest telemetry and submit orders (e.g. a trading/agent process).
    Admin: everything, including tenant/execution-tenant management.
    """
    VIEWER = 0
    OPERATOR = 1
    ADMIN = 2


class Permission(IntEnum):
    """Mirrors animus::security::Permission (animus_security.hpp) -- values
    must stay in sync. Exposed for reading AuditEvent.permission back;
    RBAC itself is enforced natively, not re-implemented here.
    """
    RECORD_EVENT = 0
    POLL_SIGNALS = 1
    ADD_RULE = 2
    MANAGE_PERSISTENCE = 3
    MANAGE_TENANTS = 4
    SUBMIT_ORDER = 5


class AuditOutcome(IntEnum):
    """Mirrors animus::security::AuditOutcome (animus_security.hpp)."""
    ALLOWED = 0
    DENIED = 1


class OrderRequest(ctypes.Structure):
    """Mirrors animus::OrderRequest (animus.hpp) byte-for-byte, INCLUDING its
    2 bytes of internal alignment padding between `type` and `price_ticks`
    -- crosses the real C-ABI via animus_security_submit_order and
    animus_shm_ring_order_*'s caller-supplied buffers, so this layout must
    match the native struct's actual 32-byte size and field offsets exactly
    (verified against a real sizeof/offsetof build before being relied on
    here, same bar as every other wire struct in this module). side/type
    are raw ctypes.c_uint8, not OrderSide/OrderType directly -- wrap with
    OrderSide(order.side) / OrderType(order.type) if you want the enum,
    same convention L2Update.side/action already follow.
    """
    _fields_ = [
        ("client_order_id", ctypes.c_uint64),
        ("instrument_id", ctypes.c_uint32),
        ("side", ctypes.c_uint8),
        ("type", ctypes.c_uint8),
        ("_reserved", ctypes.c_uint8 * 2),
        ("price_ticks", ctypes.c_uint64),
        ("quantity", ctypes.c_uint64),
    ]


assert ctypes.sizeof(OrderRequest) == 32


class ExecutionReport(ctypes.Structure):
    """Mirrors animus::ExecutionReport (animus.hpp) byte-for-byte, INCLUDING
    its 4 bytes of internal padding (after `instrument_id`) and 7 bytes of
    trailing padding (after `status`) -- same rationale and same
    real-build verification as OrderRequest above. status is raw
    ctypes.c_uint8 -- wrap with ExecStatus(report.status) for the enum.
    """
    _fields_ = [
        ("client_order_id", ctypes.c_uint64),
        ("instrument_id", ctypes.c_uint32),
        ("_reserved0", ctypes.c_uint8 * 4),
        ("filled_quantity", ctypes.c_uint64),
        ("avg_price_ticks", ctypes.c_uint64),
        ("status", ctypes.c_uint8),
        ("_reserved1", ctypes.c_uint8 * 7),
    ]


assert ctypes.sizeof(ExecutionReport) == 40


class AccessToken(ctypes.Structure):
    """Mirrors animus::security::AccessToken (animus_security.hpp)
    byte-for-byte, INCLUDING its 4 bytes of internal padding (after
    `tenant_id`) and 7 bytes of trailing padding (after `role`) -- verified
    against a real sizeof/offsetof build, same bar as every other wire
    struct in this module.

    Construct via AccessToken.make(...), not positionally
    (AccessToken(10, 1, Role.OPERATOR)) -- the padding sits BETWEEN
    tenant_id and principal_id, so positional construction would silently
    write into the reserved field instead of principal_id rather than
    raising anything.
    """
    _fields_ = [
        ("tenant_id", ctypes.c_uint32),
        ("_reserved0", ctypes.c_uint8 * 4),
        ("principal_id", ctypes.c_uint64),
        ("role", ctypes.c_uint8),
        ("_reserved1", ctypes.c_uint8 * 7),
    ]

    @classmethod
    def make(cls, tenant_id: int, principal_id: int, role: "Role | int") -> "AccessToken":
        token = cls()
        token.tenant_id = tenant_id
        token.principal_id = principal_id
        token.role = int(role)
        return token


assert ctypes.sizeof(AccessToken) == 24


class AuditEvent(ctypes.Structure):
    """Mirrors animus::security::AuditEvent (animus_security.hpp)
    byte-for-byte, INCLUDING its 4 bytes of internal padding (after
    `tenant_id`) and 6 bytes of trailing padding (after `outcome`) --
    verified against a real sizeof/offsetof build. permission/outcome are
    raw ctypes.c_uint8 -- wrap with Permission(ev.permission) /
    AuditOutcome(ev.outcome) for the enums.
    """
    _fields_ = [
        ("timestamp_cycles", ctypes.c_uint64),
        ("tenant_id", ctypes.c_uint32),
        ("_reserved0", ctypes.c_uint8 * 4),
        ("principal_id", ctypes.c_uint64),
        ("permission", ctypes.c_uint8),
        ("outcome", ctypes.c_uint8),
        ("_reserved1", ctypes.c_uint8 * 6),
    ]


assert ctypes.sizeof(AuditEvent) == 32


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


def _compare_value(lhs: int, rhs: int, comparator: int) -> bool:
    """Mirrors animus::compare_value (animus.hpp). Shared by
    _PurePythonEngine's plain-threshold and CEP rule evaluation, same as
    the native engine shares one compare_value between RuleThreshold and
    CepRuleState.
    """
    if comparator == RuleComparator.GREATER_THAN:
        return lhs > rhs
    if comparator == RuleComparator.LESS_THAN:
        return lhs < rhs
    return lhs == rhs


class _CepRuleState:
    """Pure-Python sliding-window aggregator, one per registered CEP rule.

    Recomputes the aggregate by scanning the whole window on every event
    rather than the native engine's O(1)-amortized running-sum/monotonic-
    deque approach (see animus::CepRuleState in animus.hpp) -- correctness,
    not throughput, is this fallback's goal (see _PurePythonEngine's own
    docstring). AVG's threshold check still cross-multiplies rather than
    dividing (`sum COMPARATOR threshold * count`), matching the native
    engine's exact-integer-arithmetic behavior so a rule doesn't fire
    differently depending on which engine evaluated it.
    """

    def __init__(self, rule_id: int, event_id: int, window_type: int, window_size: int,
                 aggregation: int, comparator: int, threshold: int, severity: int) -> None:
        self.rule_id = rule_id
        self.event_id = event_id
        self.window_type = window_type
        self.window_size = max(1, window_size)
        self.aggregation = aggregation
        self.comparator = comparator
        self.threshold = threshold
        self.severity = severity
        self._window: Deque = collections.deque()  # each item: (sequence, timestamp_ms, value)
        self._sequence = 0

    def on_event(self, value: int, now_ms: int) -> "tuple[bool, int]":
        self._sequence += 1
        self._window.append((self._sequence, now_ms, value))

        if self.window_type == WindowType.COUNT:
            while len(self._window) > self.window_size:
                self._window.popleft()
        else:
            while self._window and self._window[0][1] + self.window_size <= now_ms:
                self._window.popleft()

        values = [v for _, _, v in self._window]
        count = len(values)

        if self.aggregation == AggregationFunction.SUM:
            aggregated = sum(values)
            matched = _compare_value(aggregated, self.threshold, self.comparator)
        elif self.aggregation == AggregationFunction.AVG:
            total = sum(values)
            aggregated = total // count if count else 0
            matched = _compare_value(total, self.threshold * count, self.comparator)
        elif self.aggregation == AggregationFunction.MIN:
            aggregated = min(values)
            matched = _compare_value(aggregated, self.threshold, self.comparator)
        else:  # AggregationFunction.MAX
            aggregated = max(values)
            matched = _compare_value(aggregated, self.threshold, self.comparator)

        return matched, aggregated


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
        self._cep_rules: List[_CepRuleState] = []
        self._cep_rules_lock = threading.Lock()
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

    def record_events_batch(self, events) -> int:
        pushed = 0
        for event_id, trace_id, metric_value in events:
            if not self.record_event(event_id, trace_id, metric_value):
                break
            pushed += 1
        return pushed

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

    def add_cep_rule(self, rule_id: int, event_id: int, window_type: int, window_size: int,
                      aggregation: int, comparator: int, threshold: int, severity: int) -> bool:
        if window_type not in (WindowType.COUNT, WindowType.TIME):
            return False
        if aggregation not in (
            AggregationFunction.SUM, AggregationFunction.AVG,
            AggregationFunction.MIN, AggregationFunction.MAX,
        ):
            return False
        if comparator not in (
            RuleComparator.GREATER_THAN,
            RuleComparator.LESS_THAN,
            RuleComparator.EQUAL,
        ):
            return False
        with self._cep_rules_lock:
            self._cep_rules.append(_CepRuleState(
                rule_id, event_id, window_type, window_size, aggregation, comparator, threshold, severity,
            ))
        return True

    def _evaluate(self, record: _TelemetryRecord, now_ms: int) -> None:
        with self._rules_lock:
            rules_snapshot = list(self._rules)
        for rule in rules_snapshot:
            if rule.event_id != record.event_id:
                continue
            matched = _compare_value(record.metric_value, rule.threshold, rule.comparator)
            if matched:
                self._signals.push(ThreatSignal(
                    timestamp_cycles=record.timestamp_cycles,
                    event_id=record.event_id,
                    trace_id=record.trace_id,
                    metric_value=record.metric_value,
                    rule_id=rule.rule_id,
                    severity=rule.severity,
                ))

        with self._cep_rules_lock:
            cep_rules_snapshot = list(self._cep_rules)
        for cep_rule in cep_rules_snapshot:
            if cep_rule.event_id != record.event_id:
                continue
            matched, aggregated_value = cep_rule.on_event(record.metric_value, now_ms)
            if matched:
                self._signals.push(ThreatSignal(
                    timestamp_cycles=record.timestamp_cycles,
                    event_id=record.event_id,
                    trace_id=record.trace_id,
                    metric_value=aggregated_value,  # the window's aggregate, not the raw event's value
                    rule_id=cep_rule.rule_id,
                    severity=cep_rule.severity,
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
                # Read once per batch, not once per record, for the same reason
                # the native engine's process_persistence_queue does (see
                # animus.hpp): cheap but not free, and CEP time-window eviction
                # only needs millisecond resolution.
                now_ms = int(time.monotonic() * 1000)
                for record in batch:
                    self._evaluate(record, now_ms)
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
        self._spsc_initialized = False

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

        self._lib.animus_record_events_batch.argtypes = [
            ctypes.POINTER(NativeEvent),
            ctypes.c_size_t,
        ]
        self._lib.animus_record_events_batch.restype = ctypes.c_size_t

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

        self._lib.animus_add_cep_rule.argtypes = [
            ctypes.c_uint32,
            ctypes.c_uint32,
            ctypes.c_uint8,
            ctypes.c_uint64,
            ctypes.c_uint8,
            ctypes.c_uint8,
            ctypes.c_uint64,
            ctypes.c_uint32,
        ]
        self._lib.animus_add_cep_rule.restype = ctypes.c_bool

        self._lib.animus_poll_signals.argtypes = [
            ctypes.POINTER(ThreatSignal),
            ctypes.c_size_t,
        ]
        self._lib.animus_poll_signals.restype = ctypes.c_size_t

        self._lib.animus_spsc_init.argtypes = [ctypes.c_size_t]
        self._lib.animus_spsc_init.restype = ctypes.c_bool

        self._lib.animus_spsc_record_events_batch.argtypes = [
            ctypes.POINTER(NativeEvent),
            ctypes.c_size_t,
        ]
        self._lib.animus_spsc_record_events_batch.restype = ctypes.c_size_t

        self._lib.animus_spsc_drain.argtypes = [
            ctypes.POINTER(SpscTelemetryRecord),
            ctypes.c_size_t,
        ]
        self._lib.animus_spsc_drain.restype = ctypes.c_size_t

        self._lib.animus_pin_current_thread_to_core.argtypes = [ctypes.c_int]
        self._lib.animus_pin_current_thread_to_core.restype = ctypes.c_bool

        self._lib.animus_set_thread_high_priority.argtypes = []
        self._lib.animus_set_thread_high_priority.restype = None

        self._lib.animus_get_cpu_count.argtypes = []
        self._lib.animus_get_cpu_count.restype = ctypes.c_uint

        self._lib.animus_verify_license.argtypes = [ctypes.c_char_p]
        self._lib.animus_verify_license.restype = ctypes.c_bool

        self._lib.animus_is_licensed.argtypes = []
        self._lib.animus_is_licensed.restype = ctypes.c_bool

        self._lib.animus_licensed_max_cores.argtypes = []
        self._lib.animus_licensed_max_cores.restype = ctypes.c_uint32

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

    def record_events_batch(self, events: "list[tuple[int, int, int]]") -> int:
        """Pushes a batch of (event_id, trace_id, metric_value) tuples onto
        the ring buffer in a single native call, amortizing the per-call
        ctypes marshalling cost across the whole batch instead of paying it
        once per event (see record_event). Returns the number actually
        pushed -- fewer than len(events) if the ring buffer fills partway
        through (never blocks, same contract as record_event).

        The (NativeEvent * N) array handed to native is filled via one
        struct.pack() per event joined into a single bytes object, then one
        memmove() into the array's backing memory -- not by constructing N
        NativeEvent Python objects, which measured ~6x slower at 100k
        events (Python object-creation overhead per event, not marshalling
        cost, dominated that approach).
        """
        if not self._initialized:
            raise RuntimeError("AnimusBindings.init() must succeed before recording events")
        events = list(events)
        if not events:
            return 0
        if self.using_native_engine:
            count = len(events)
            packed = b"".join(
                struct.pack(_NATIVE_EVENT_FORMAT, event_id, trace_id, metric_value)
                for event_id, trace_id, metric_value in events
            )
            buf = (NativeEvent * count)()
            ctypes.memmove(buf, packed, len(packed))
            return int(self._lib.animus_record_events_batch(buf, ctypes.c_size_t(count)))
        return self._fallback.record_events_batch(events)

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

    def add_cep_rule(
        self,
        rule_id: int,
        event_id: int,
        window_type: "WindowType | int",
        window_size: int,
        aggregation: "AggregationFunction | int",
        comparator: "RuleComparator | int",
        threshold: int,
        severity: int,
    ) -> bool:
        """Registers a Complex Event Processing (CEP) sliding-window rule:
        aggregates matching events' metric_value over a count- or
        time-based window (see animus::CepRuleState, animus.hpp) and
        evaluates the aggregate against `threshold` on every matching
        event -- "SUM of the last 100 events exceeds X", "AVG over the
        last 5000ms is below Y", not just one event's raw value.

        window_type: WindowType.COUNT (window_size = number of events) or
        WindowType.TIME (window_size = milliseconds). aggregation:
        AggregationFunction.SUM/AVG/MIN/MAX. Matches are delivered through
        the same poll_signals() queue as add_rule() matches -- the
        ThreatSignal's metric_value field carries the window's aggregated
        value (floored to an integer for AVG), not the triggering event's
        raw metric_value; rule_id tells you which rule (plain or CEP)
        fired. Returns False for an unrecognized window_type, aggregation,
        or comparator value.
        """
        if not self._initialized:
            raise RuntimeError("AnimusBindings.init() must succeed before adding rules")
        if self.using_native_engine:
            return bool(self._lib.animus_add_cep_rule(
                ctypes.c_uint32(rule_id),
                ctypes.c_uint32(event_id),
                ctypes.c_uint8(int(window_type)),
                ctypes.c_uint64(window_size),
                ctypes.c_uint8(int(aggregation)),
                ctypes.c_uint8(int(comparator)),
                ctypes.c_uint64(threshold),
                ctypes.c_uint32(severity),
            ))
        return self._fallback.add_cep_rule(
            rule_id, event_id, int(window_type), window_size, int(aggregation), int(comparator), threshold, severity,
        )

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

    def _require_native(self, what: str) -> None:
        if not self.using_native_engine:
            raise RuntimeError(
                f"{what} has no pure-Python fallback -- it is a native "
                "performance primitive (lock-free SPSC ring / OS thread "
                "affinity), not something a Python-level reimplementation "
                "could meaningfully provide. Build CMakeLists.txt or "
                "AnimusCore_v1.slnx first."
            )

    def spsc_init(self, buffer_capacity: int = 65536) -> bool:
        """Initializes the standalone single-producer/single-consumer ring
        (animus::SpscRingBuffer, see animus.hpp), fully independent of
        init()'s Engine singleton -- its own buffer, its own lifetime.
        Idempotent, same as init(). Requires the native engine: there is no
        pure-Python fallback (see _require_native).
        """
        self._require_native("spsc_init")
        if self._spsc_initialized:
            return True
        self._spsc_initialized = bool(self._lib.animus_spsc_init(ctypes.c_size_t(buffer_capacity)))
        return self._spsc_initialized

    def spsc_record_events_batch(self, events: "list[tuple[int, int, int]]") -> int:
        """Producer-side push into the SPSC ring. Same batch semantics and
        wire format as record_events_batch() (struct.pack + memmove into a
        NativeEvent array, one native call per batch) -- the only
        difference is which ring it targets.

        Single-producer contract: never call this from more than one
        thread concurrently. Nothing in this binding enforces that (see
        animus::SpscRingBuffer's docstring for why) -- it is the caller's
        responsibility, same as it is in the C++ template underneath.
        """
        self._require_native("spsc_record_events_batch")
        if not self._spsc_initialized:
            raise RuntimeError("AnimusBindings.spsc_init() must succeed before recording events")
        events = list(events)
        if not events:
            return 0
        count = len(events)
        packed = b"".join(
            struct.pack(_NATIVE_EVENT_FORMAT, event_id, trace_id, metric_value)
            for event_id, trace_id, metric_value in events
        )
        buf = (NativeEvent * count)()
        ctypes.memmove(buf, packed, len(packed))
        return int(self._lib.animus_spsc_record_events_batch(buf, ctypes.c_size_t(count)))

    def spsc_drain(self, max_count: int = 1024) -> List[SpscTelemetryRecord]:
        """Consumer-side drain from the SPSC ring. Zero-copy, same pattern
        as poll_signals(). Single-consumer contract: never call this from
        more than one thread concurrently (a different thread than the
        producer is fine; more than one consumer thread is not).
        """
        self._require_native("spsc_drain")
        if not self._spsc_initialized:
            raise RuntimeError("AnimusBindings.spsc_init() must succeed before draining")
        buf = (SpscTelemetryRecord * max_count)()
        count = self._lib.animus_spsc_drain(buf, ctypes.c_size_t(max_count))
        return list(buf[:count])

    def pin_current_thread_to_core(self, core_id: int) -> bool:
        """Pins the calling OS thread to logical CPU `core_id` (0-based).
        Affects only the thread that calls it, not the whole process --
        call this at the start of whichever thread will run your hot
        ingestion loop. Returns False if core_id is out of range or the
        current platform has no supported hard-pinning API (see
        animus_pin_current_thread_to_core in animus_engine.cpp for exactly
        which platforms that covers). No pure-Python fallback: OS thread
        affinity is not something Python can provide portably on its own.
        """
        self._require_native("pin_current_thread_to_core")
        return bool(self._lib.animus_pin_current_thread_to_core(ctypes.c_int(core_id)))

    def set_thread_high_priority(self) -> None:
        """Raises the calling OS thread to the highest realtime/time-critical
        scheduling tier this host will grant (see
        animus::sys::set_thread_high_priority,
        include/animus/thread_affinity.hpp), falling back a tier if the host
        denies it. Same license gate as pin_current_thread_to_core: a no-op
        with no verified license. Best-effort by design -- never raises,
        call it right after pin_current_thread_to_core() on a thread about
        to enter its hot loop. No pure-Python fallback: OS scheduling
        priority is not something Python can provide portably on its own.
        """
        self._require_native("set_thread_high_priority")
        self._lib.animus_set_thread_high_priority()

    def get_cpu_count(self) -> int:
        """Logical CPU count on this machine, for sanity-checking a core_id
        before calling pin_current_thread_to_core.
        """
        self._require_native("get_cpu_count")
        return int(self._lib.animus_get_cpu_count())

    def verify_license(self, license_path: str) -> bool:
        """Verifies an RSA-signed offline license file (see
        animus::LicensePayload, animus.hpp) against the public key baked
        into this build and this machine's hardware fingerprint. Entirely
        offline -- no network call is made. On success, gates
        pin_current_thread_to_core()/spsc_init() to the license's entitled
        core count; both fail closed until this has succeeded once in this
        process. Returns False for a missing/wrong-size file, a bad magic
        number, a signature that doesn't verify (tampered file, or signed
        by a different key), a fingerprint for a different machine, or an
        expired license. Windows-only: returns False on other platforms
        rather than faking success (see animus_verify_license's definition
        in animus_engine.cpp for why). No pure-Python fallback -- this is
        a native security boundary, not something a Python reimplementation
        could meaningfully provide.
        """
        self._require_native("verify_license")
        return bool(self._lib.animus_verify_license(license_path.encode("utf-8")))

    def is_licensed(self) -> bool:
        """True once verify_license() has succeeded in this process."""
        self._require_native("is_licensed")
        return bool(self._lib.animus_is_licensed())

    def licensed_max_cores(self) -> int:
        """The verified license's entitled core count, or 0 if unlicensed."""
        self._require_native("licensed_max_cores")
        return int(self._lib.animus_licensed_max_cores())


def _configure_shm_signatures(lib: ctypes.CDLL) -> None:
    lib.animus_shm_create.argtypes = [ctypes.c_char_p, ctypes.c_uint64]
    lib.animus_shm_create.restype = ctypes.c_void_p
    lib.animus_shm_attach.argtypes = [ctypes.c_char_p]
    lib.animus_shm_attach.restype = ctypes.c_void_p
    lib.animus_shm_close.argtypes = [ctypes.c_void_p]
    lib.animus_shm_close.restype = None
    lib.animus_shm_unlink.argtypes = [ctypes.c_char_p]
    lib.animus_shm_unlink.restype = ctypes.c_bool
    lib.animus_shm_capacity.argtypes = [ctypes.c_void_p]
    lib.animus_shm_capacity.restype = ctypes.c_uint64
    lib.animus_shm_push.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint64]
    lib.animus_shm_push.restype = ctypes.c_bool
    lib.animus_shm_pop.argtypes = [ctypes.c_void_p, ctypes.POINTER(SharedRecord)]
    lib.animus_shm_pop.restype = ctypes.c_bool


class SharedTelemetryChannel:
    """ctypes-backed cross-process shared-memory telemetry channel --
    animus::SharedTelemetryChannel (animus.hpp) exposed via ctypes, the
    native counterpart to animus.shm.SharedTelemetryRing's pure-Python
    implementation.

    Wire-compatible with SharedTelemetryRing: both read/write the exact
    same byte layout (verified with a real cross-process round trip in
    each direction before this class was written -- a Python producer
    via SharedTelemetryRing feeding a native consumer here, and a native
    producer here feeding a Python consumer via SharedTelemetryRing, both
    checked, not assumed), so a producer using one implementation and a
    consumer using the other work together on the same named segment
    without either side knowing which the other is. The difference is
    where the per-call work happens: every push()/pop() here crosses into
    compiled C++ via one ctypes call, rather than doing
    struct.pack_into/unpack_from in Python.

    Single-producer/single-consumer, same as SharedTelemetryRing: do not
    share one channel across multiple producer or multiple consumer
    processes/threads -- use AnimusBindings.record_events_batch (a real
    MPMC ring) if you need that. Requires the native engine; there is no
    pure-Python fallback for this class specifically (that's what
    SharedTelemetryRing already is -- attach to the same segment with
    whichever of the two fits the process that doesn't have a compiled
    binary available).
    """

    def __init__(self, handle: int, lib: ctypes.CDLL) -> None:
        self._handle = handle
        self._lib = lib

    @classmethod
    def create(cls, name: str, capacity: int) -> "SharedTelemetryChannel":
        """Allocates a new named shared-memory segment sized for `capacity`
        records (any positive value -- not required to be a power of two;
        see animus::SharedTelemetryChannel::create) and takes ownership of
        it. Raises OSError if a segment with this name already exists.
        """
        if capacity < 1:
            raise ValueError("capacity must be >= 1")
        lib = load_native_library(required=True)
        _configure_shm_signatures(lib)
        handle = lib.animus_shm_create(name.encode("utf-8"), ctypes.c_uint64(capacity))
        if not handle:
            raise OSError(f"failed to create shared memory segment {name!r} (does one already exist?)")
        return cls(handle, lib)

    @classmethod
    def attach(cls, name: str) -> "SharedTelemetryChannel":
        """Maps an existing segment created by another process's create()
        call -- the native one above, or animus.shm.SharedTelemetryRing's,
        either one. Raises OSError if no such segment exists.
        """
        lib = load_native_library(required=True)
        _configure_shm_signatures(lib)
        handle = lib.animus_shm_attach(name.encode("utf-8"))
        if not handle:
            raise OSError(f"failed to attach to shared memory segment {name!r} (does it exist?)")
        return cls(handle, lib)

    @staticmethod
    def unlink(name: str) -> bool:
        """Destroys the underlying OS shared-memory object. POSIX: a real
        unlink -- call once every attached process has closed its mapping,
        same contract as SharedTelemetryRing.unlink(). Windows: a
        documented no-op (see animus::SharedMemorySegment::unlink) -- the
        object is destroyed automatically once every handle to it closes,
        so this always returns True there with nothing left to do.
        """
        lib = load_native_library(required=True)
        _configure_shm_signatures(lib)
        return bool(lib.animus_shm_unlink(name.encode("utf-8")))

    @property
    def capacity(self) -> int:
        return int(self._lib.animus_shm_capacity(self._handle))

    def push(self, event_id: int, trace_id: int, metric_value: int) -> bool:
        """Writes one record directly into shared memory. Producer-only.
        Never blocks; returns False if the ring is full. timestamp_cycles
        is stamped natively (read_cycle_counter()) -- unlike
        SharedTelemetryRing.push(), there is no caller-supplied override.
        """
        return bool(self._lib.animus_shm_push(
            self._handle, ctypes.c_uint32(event_id), ctypes.c_uint32(trace_id), ctypes.c_uint64(metric_value),
        ))

    def pop(self) -> Optional[TelemetryRecordView]:
        """Reads and removes the oldest pending record. Consumer-only.
        Never blocks; returns None if the ring is empty.
        """
        rec = SharedRecord()
        if not self._lib.animus_shm_pop(self._handle, ctypes.byref(rec)):
            return None
        return TelemetryRecordView(rec.timestamp_cycles, rec.event_id, rec.trace_id, rec.metric_value)

    def close(self) -> None:
        """Releases this process's mapping. Safe to call from any side,
        and safe to call more than once.
        """
        if self._handle:
            self._lib.animus_shm_close(self._handle)
            self._handle = None

    def __del__(self) -> None:
        # Narrow, deliberate except-Exception: this is a finalizer-safety
        # net against a leaked native handle if the caller never calls
        # close(), not a place to propagate errors -- module globals
        # (ctypes function objects) can already be torn down by the time
        # __del__ runs during interpreter shutdown, and raising from
        # __del__ is silently ignored by Python anyway (with a warning),
        # so catching here is strictly more informative than not.
        try:
            self.close()
        except Exception:
            pass


def _configure_feed_signatures(lib: ctypes.CDLL) -> None:
    lib.animus_feed_create.argtypes = [ctypes.c_size_t, ctypes.c_size_t]
    lib.animus_feed_create.restype = ctypes.c_void_p
    lib.animus_feed_close.argtypes = [ctypes.c_void_p]
    lib.animus_feed_close.restype = None
    lib.animus_feed_l2_capacity.argtypes = [ctypes.c_void_p]
    lib.animus_feed_l2_capacity.restype = ctypes.c_size_t
    lib.animus_feed_trade_capacity.argtypes = [ctypes.c_void_p]
    lib.animus_feed_trade_capacity.restype = ctypes.c_size_t
    lib.animus_feed_push_l2_update.argtypes = [
        ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint8, ctypes.c_uint8,
        ctypes.c_uint32, ctypes.c_uint64, ctypes.c_uint64, ctypes.c_uint64, ctypes.c_uint64,
    ]
    lib.animus_feed_push_l2_update.restype = ctypes.c_bool
    lib.animus_feed_push_trade.argtypes = [
        ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint64, ctypes.c_uint8,
        ctypes.c_uint64, ctypes.c_uint64, ctypes.c_uint64, ctypes.c_uint64,
    ]
    lib.animus_feed_push_trade.restype = ctypes.c_bool
    lib.animus_feed_poll_l2_updates.argtypes = [ctypes.c_void_p, ctypes.POINTER(L2Update), ctypes.c_size_t]
    lib.animus_feed_poll_l2_updates.restype = ctypes.c_size_t
    lib.animus_feed_poll_trades.argtypes = [ctypes.c_void_p, ctypes.POINTER(TradeTick), ctypes.c_size_t]
    lib.animus_feed_poll_trades.restype = ctypes.c_size_t


class MarketDataFeed:
    """ctypes-backed live market-data ingestion feed -- animus::MarketDataFeed
    (animus.hpp) exposed via ctypes: one lock-free ring for L2 order-book
    updates, one for trade execution ticks.

    Thread-safe for concurrent producers AND concurrent consumers (Vyukov
    MPMC ring underneath, the same design animus_record_events_batch's ring
    uses) -- unlike SpscTelemetryRecord/spsc_drain elsewhere in this module,
    which are deliberately single-producer/single-consumer for extra
    throughput. Multiple feed-handler threads (e.g. one per venue
    connection) may call push_l2_update()/push_trade() on the same
    MarketDataFeed instance concurrently, and multiple consumer threads may
    call poll_l2_updates()/poll_trades() concurrently, with no external
    locking required.

    Fully in-process, unlike SharedTelemetryChannel above: there is no OS
    shared-memory segment here, so a second process cannot attach to a feed
    created in this one. Requires the native engine; there is no
    pure-Python fallback (a native performance/concurrency primitive, same
    reasoning as SpscRingBuffer and SharedTelemetryChannel).
    """

    def __init__(self, handle: int, lib: ctypes.CDLL) -> None:
        self._handle = handle
        self._lib = lib

    @classmethod
    def create(cls, l2_capacity: int = 65536, trade_capacity: int = 65536) -> "MarketDataFeed":
        """Allocates a new feed with independently-sized L2-update and
        trade-tick rings. Each capacity is rounded up to the next power of
        two internally (animus::LockFreeRingBuffer's requirement), same as
        init()'s buffer_capacity. Raises OSError only on allocation failure
        (out of memory) -- unlike SharedTelemetryChannel.create(), there is
        no OS name to collide on, since this lives entirely in this
        process's heap.
        """
        lib = load_native_library(required=True)
        _configure_feed_signatures(lib)
        handle = lib.animus_feed_create(ctypes.c_size_t(l2_capacity), ctypes.c_size_t(trade_capacity))
        if not handle:
            raise OSError("failed to allocate MarketDataFeed (out of memory?)")
        return cls(handle, lib)

    @property
    def l2_capacity(self) -> int:
        return int(self._lib.animus_feed_l2_capacity(self._handle))

    @property
    def trade_capacity(self) -> int:
        return int(self._lib.animus_feed_trade_capacity(self._handle))

    def push_l2_update(
        self,
        instrument_id: int,
        side: int,
        action: int,
        level: int,
        price_ticks: int,
        quantity: int,
        sequence_number: int = 0,
        exchange_timestamp_ns: int = 0,
    ) -> bool:
        """Pushes one order-book price-level update. `side`/`action` accept
        either a raw int or a BookSide/BookUpdateAction member (IntEnum, so
        both work interchangeably). Never blocks; returns False if the L2
        ring is full. timestamp_cycles is stamped natively at the moment of
        this call -- exchange_timestamp_ns is whatever timestamp the feed
        itself carried, entirely caller-supplied.
        """
        return bool(self._lib.animus_feed_push_l2_update(
            self._handle,
            ctypes.c_uint32(instrument_id), ctypes.c_uint8(side), ctypes.c_uint8(action),
            ctypes.c_uint32(level), ctypes.c_uint64(price_ticks), ctypes.c_uint64(quantity),
            ctypes.c_uint64(sequence_number), ctypes.c_uint64(exchange_timestamp_ns),
        ))

    def push_trade(
        self,
        instrument_id: int,
        trade_id: int,
        aggressor_side: int,
        price_ticks: int,
        quantity: int,
        sequence_number: int = 0,
        exchange_timestamp_ns: int = 0,
    ) -> bool:
        """Pushes one executed-trade tick. `aggressor_side` accepts either a
        raw int or a TradeAggressor member. Never blocks; returns False if
        the trade ring is full.
        """
        return bool(self._lib.animus_feed_push_trade(
            self._handle,
            ctypes.c_uint32(instrument_id), ctypes.c_uint64(trade_id), ctypes.c_uint8(aggressor_side),
            ctypes.c_uint64(price_ticks), ctypes.c_uint64(quantity),
            ctypes.c_uint64(sequence_number), ctypes.c_uint64(exchange_timestamp_ns),
        ))

    def poll_l2_updates(self, max_count: int = 1024) -> List[L2Update]:
        """Drains up to max_count pending order-book updates. Never blocks;
        returns fewer than max_count (including zero/empty) if fewer are
        currently pending. Zero-copy: `buf` is a single contiguous ctypes
        array allocated once per call, written into directly by the native
        side, same pattern as AnimusBindings.poll_signals().
        """
        buf = (L2Update * max_count)()
        count = self._lib.animus_feed_poll_l2_updates(self._handle, buf, ctypes.c_size_t(max_count))
        return list(buf[:count])

    def poll_trades(self, max_count: int = 1024) -> List[TradeTick]:
        """Drains up to max_count pending trade ticks. Same contract as
        poll_l2_updates().
        """
        buf = (TradeTick * max_count)()
        count = self._lib.animus_feed_poll_trades(self._handle, buf, ctypes.c_size_t(max_count))
        return list(buf[:count])

    def close(self) -> None:
        """Releases this feed's native rings. Safe to call more than once."""
        if self._handle:
            self._lib.animus_feed_close(self._handle)
            self._handle = None

    def __del__(self) -> None:
        # Same finalizer-safety rationale as SharedTelemetryChannel.__del__ above.
        try:
            self.close()
        except Exception:
            pass


def _configure_shm_ring_signatures(lib: ctypes.CDLL) -> None:
    lib.animus_shm_ring_create.argtypes = [ctypes.c_char_p, ctypes.c_size_t]
    lib.animus_shm_ring_create.restype = ctypes.c_void_p
    lib.animus_shm_ring_open.argtypes = [ctypes.c_char_p]
    lib.animus_shm_ring_open.restype = ctypes.c_void_p
    lib.animus_shm_ring_close.argtypes = [ctypes.c_void_p]
    lib.animus_shm_ring_close.restype = None
    lib.animus_shm_ring_unlink.argtypes = [ctypes.c_char_p]
    lib.animus_shm_ring_unlink.restype = ctypes.c_bool
    lib.animus_shm_ring_capacity.argtypes = [ctypes.c_void_p]
    lib.animus_shm_ring_capacity.restype = ctypes.c_size_t
    lib.animus_shm_ring_try_push.argtypes = [ctypes.c_void_p, ctypes.POINTER(NativeEvent)]
    lib.animus_shm_ring_try_push.restype = ctypes.c_bool
    lib.animus_shm_ring_try_pop.argtypes = [ctypes.c_void_p, ctypes.POINTER(NativeEvent)]
    lib.animus_shm_ring_try_pop.restype = ctypes.c_bool
    lib.animus_shm_ring_push_batch.argtypes = [ctypes.c_void_p, ctypes.POINTER(NativeEvent), ctypes.c_size_t]
    lib.animus_shm_ring_push_batch.restype = ctypes.c_size_t
    lib.animus_shm_ring_pop_batch.argtypes = [ctypes.c_void_p, ctypes.POINTER(NativeEvent), ctypes.c_size_t]
    lib.animus_shm_ring_pop_batch.restype = ctypes.c_size_t


class ShmRingChannel:
    """ctypes-backed wrapper over animus::sys::ipc::ShmRing<animus::RawEvent>
    (include/animus/shm_ipc.hpp), instantiated for animus::RawEvent -- the
    same 16-byte record animus_record_events_batch itself takes, so a batch
    popped off this ring is exactly the array record_events_batch() wants,
    with no translation step.

    Distinct from SharedTelemetryChannel above, not a replacement for it:
    that class is deliberately wire-compatible with the pure-Python
    SharedTelemetryRing (identical byte layout, no cache-line padding, so
    either implementation can produce or consume the same segment).
    ShmRing<T> has no such interop constraint -- it pads its producer head
    and consumer tail cursors onto separate cache lines to eliminate false
    sharing between them, at the cost of that Python/C++ wire compatibility.
    See include/animus/shm_ipc.hpp's own module docstring and
    AnimusCore_v1/BENCHMARKS.md's Phase 20 section for the measured latency
    difference that buys.

    Single-producer/single-consumer, same contract as SpscRingBuffer/
    SharedTelemetryChannel elsewhere in this module -- don't share one
    channel across more than one producer or more than one consumer
    process/thread; it isn't enforced at runtime, for the same reason it
    isn't there either.

    No spin-blocking wait is exposed here, on purpose. ShmRing<T>'s own C++
    push_spin()/pop_spin() can legitimately block for low seconds waiting
    for a peer (bounded, not infinite, but still long for one call) -- a
    ctypes call that blocks that long releases the GIL for its entire
    duration and cannot be interrupted with Ctrl+C from Python. If you need
    "wait for the next item," poll try_pop()/pop_batch() in your own
    Python-level loop instead (optionally with a short time.sleep()) --
    interruptible at every iteration, same pattern
    AnimusCore_v1/ingest_engine.py's own signal-poller loop already uses.

    Requires the native engine; there is no pure-Python fallback (a native
    concurrency primitive, same reasoning as SpscRingBuffer/MarketDataFeed
    elsewhere in this module).
    """

    def __init__(self, handle: int, lib: ctypes.CDLL) -> None:
        self._handle = handle
        self._lib = lib

    @classmethod
    def create(cls, name: str, capacity: int) -> "ShmRingChannel":
        """Allocates a new named ring sized for at least `capacity` slots
        (rounded up to the next power of two internally, see
        ShmRing<T>::create). Raises OSError if a ring with this name
        already exists.
        """
        if capacity < 1:
            raise ValueError("capacity must be >= 1")
        lib = load_native_library(required=True)
        _configure_shm_ring_signatures(lib)
        handle = lib.animus_shm_ring_create(name.encode("utf-8"), ctypes.c_size_t(capacity))
        if not handle:
            raise OSError(f"failed to create shm ring {name!r} (does one already exist?)")
        return cls(handle, lib)

    @classmethod
    def open(cls, name: str) -> "ShmRingChannel":
        """Maps an existing ring created by another process's create()
        call. Raises OSError if no such ring exists.
        """
        lib = load_native_library(required=True)
        _configure_shm_ring_signatures(lib)
        handle = lib.animus_shm_ring_open(name.encode("utf-8"))
        if not handle:
            raise OSError(f"failed to open shm ring {name!r} (does it exist?)")
        return cls(handle, lib)

    @staticmethod
    def unlink(name: str) -> bool:
        """Destroys the underlying OS shared-memory object. POSIX: a real
        unlink -- call once every attached process has closed its mapping.
        Windows: a documented no-op (see SharedMemoryRegion::unlink) --
        always returns True there, with nothing left to do.
        """
        lib = load_native_library(required=True)
        _configure_shm_ring_signatures(lib)
        return bool(lib.animus_shm_ring_unlink(name.encode("utf-8")))

    @property
    def capacity(self) -> int:
        return int(self._lib.animus_shm_ring_capacity(self._handle))

    def try_push(self, event_id: int, trace_id: int, metric_value: int) -> bool:
        """Producer-side. Never blocks; returns False if the ring is full."""
        event = NativeEvent(event_id, trace_id, metric_value)
        return bool(self._lib.animus_shm_ring_try_push(self._handle, ctypes.byref(event)))

    def try_pop(self) -> Optional[NativeEvent]:
        """Consumer-side. Never blocks; returns None if the ring is empty."""
        event = NativeEvent()
        if not self._lib.animus_shm_ring_try_pop(self._handle, ctypes.byref(event)):
            return None
        return event

    def push_batch(self, events: "list[tuple[int, int, int]]") -> int:
        """Producer-side batch push. Amortizes the ctypes call boundary
        across the whole batch -- same reasoning, and the same
        struct.pack()+memmove() buffer-building, as
        AnimusBindings.record_events_batch(). Stops at the first push that
        fails (ring full); returns how many, in order, actually made it in
        (never blocks, same contract as try_push()).
        """
        events = list(events)
        if not events:
            return 0
        count = len(events)
        packed = b"".join(
            struct.pack(_NATIVE_EVENT_FORMAT, event_id, trace_id, metric_value)
            for event_id, trace_id, metric_value in events
        )
        buf = (NativeEvent * count)()
        ctypes.memmove(buf, packed, len(packed))
        return int(self._lib.animus_shm_ring_push_batch(self._handle, buf, ctypes.c_size_t(count)))

    def pop_batch(self, max_count: int = 1024) -> List[NativeEvent]:
        """Consumer-side batch drain. Never blocks; returns fewer than
        max_count (including zero/empty) if fewer are currently pending.
        Zero-copy: `buf` is a single contiguous ctypes array written into
        directly by the native side, same pattern as
        MarketDataFeed.poll_trades().
        """
        buf = (NativeEvent * max_count)()
        count = self._lib.animus_shm_ring_pop_batch(self._handle, buf, ctypes.c_size_t(max_count))
        return list(buf[:count])

    def close(self) -> None:
        """Releases this process's mapping. Safe to call more than once."""
        if self._handle:
            self._lib.animus_shm_ring_close(self._handle)
            self._handle = None

    def __del__(self) -> None:
        # Same finalizer-safety rationale as SharedTelemetryChannel.__del__ above.
        try:
            self.close()
        except Exception:
            pass


def _configure_shm_ring_order_signatures(lib: ctypes.CDLL) -> None:
    lib.animus_shm_ring_order_create.argtypes = [ctypes.c_char_p, ctypes.c_size_t]
    lib.animus_shm_ring_order_create.restype = ctypes.c_void_p
    lib.animus_shm_ring_order_open.argtypes = [ctypes.c_char_p]
    lib.animus_shm_ring_order_open.restype = ctypes.c_void_p
    lib.animus_shm_ring_order_close.argtypes = [ctypes.c_void_p]
    lib.animus_shm_ring_order_close.restype = None
    lib.animus_shm_ring_order_unlink.argtypes = [ctypes.c_char_p]
    lib.animus_shm_ring_order_unlink.restype = ctypes.c_bool
    lib.animus_shm_ring_order_capacity.argtypes = [ctypes.c_void_p]
    lib.animus_shm_ring_order_capacity.restype = ctypes.c_size_t
    lib.animus_shm_ring_order_try_push.argtypes = [ctypes.c_void_p, ctypes.POINTER(OrderRequest)]
    lib.animus_shm_ring_order_try_push.restype = ctypes.c_bool
    lib.animus_shm_ring_order_try_pop.argtypes = [ctypes.c_void_p, ctypes.POINTER(OrderRequest)]
    lib.animus_shm_ring_order_try_pop.restype = ctypes.c_bool
    lib.animus_shm_ring_order_push_batch.argtypes = [ctypes.c_void_p, ctypes.POINTER(OrderRequest), ctypes.c_size_t]
    lib.animus_shm_ring_order_push_batch.restype = ctypes.c_size_t
    lib.animus_shm_ring_order_pop_batch.argtypes = [ctypes.c_void_p, ctypes.POINTER(OrderRequest), ctypes.c_size_t]
    lib.animus_shm_ring_order_pop_batch.restype = ctypes.c_size_t


class ShmOrderRingChannel:
    """ctypes-backed wrapper over animus::sys::ipc::ShmRing<animus::OrderRequest>
    (include/animus/shm_ipc.hpp), instantiated for animus::OrderRequest --
    the order-routing counterpart to ShmRingChannel above (which carries
    animus::RawEvent, a telemetry shape). Same API shape, same contracts,
    same single-producer/single-consumer restriction, same "no spin-blocking
    wait exposed here" rationale as ShmRingChannel's own docstring -- see
    that class for the full explanation, not repeated here.

    Typically paired with SecurityContext below: a producer process pushes
    OrderRequest records for ONE tenant into one named ring; the consumer
    process (holding the SecurityContext) drains that tenant's own ring and
    calls SecurityContext.submit_order() with an AccessToken scoped to that
    same tenant -- one ring per tenant, not one shared ring carrying a
    tenant id on the wire, matching animus_security.hpp's own "isolation is
    structural" design (see SecureExecutionGateway's docstring).
    """

    def __init__(self, handle: int, lib: ctypes.CDLL) -> None:
        self._handle = handle
        self._lib = lib

    @classmethod
    def create(cls, name: str, capacity: int) -> "ShmOrderRingChannel":
        if capacity < 1:
            raise ValueError("capacity must be >= 1")
        lib = load_native_library(required=True)
        _configure_shm_ring_order_signatures(lib)
        handle = lib.animus_shm_ring_order_create(name.encode("utf-8"), ctypes.c_size_t(capacity))
        if not handle:
            raise OSError(f"failed to create shm order ring {name!r} (does one already exist?)")
        return cls(handle, lib)

    @classmethod
    def open(cls, name: str) -> "ShmOrderRingChannel":
        lib = load_native_library(required=True)
        _configure_shm_ring_order_signatures(lib)
        handle = lib.animus_shm_ring_order_open(name.encode("utf-8"))
        if not handle:
            raise OSError(f"failed to open shm order ring {name!r} (does it exist?)")
        return cls(handle, lib)

    @staticmethod
    def unlink(name: str) -> bool:
        lib = load_native_library(required=True)
        _configure_shm_ring_order_signatures(lib)
        return bool(lib.animus_shm_ring_order_unlink(name.encode("utf-8")))

    @property
    def capacity(self) -> int:
        return int(self._lib.animus_shm_ring_order_capacity(self._handle))

    def try_push(self, order: OrderRequest) -> bool:
        """Producer-side. Never blocks; returns False if the ring is full."""
        return bool(self._lib.animus_shm_ring_order_try_push(self._handle, ctypes.byref(order)))

    def try_pop(self) -> Optional[OrderRequest]:
        """Consumer-side. Never blocks; returns None if the ring is empty."""
        order = OrderRequest()
        if not self._lib.animus_shm_ring_order_try_pop(self._handle, ctypes.byref(order)):
            return None
        return order

    def push_batch(self, orders: "list[OrderRequest]") -> int:
        """Producer-side batch push. Stops at the first push that fails
        (ring full); returns how many, in order, actually made it in.
        """
        orders = list(orders)
        if not orders:
            return 0
        buf = (OrderRequest * len(orders))(*orders)
        return int(self._lib.animus_shm_ring_order_push_batch(self._handle, buf, ctypes.c_size_t(len(orders))))

    def pop_batch(self, max_count: int = 1024) -> List[OrderRequest]:
        """Consumer-side batch drain. Never blocks; returns fewer than
        max_count (including zero/empty) if fewer are currently pending.
        """
        buf = (OrderRequest * max_count)()
        count = self._lib.animus_shm_ring_order_pop_batch(self._handle, buf, ctypes.c_size_t(max_count))
        return list(buf[:count])

    def close(self) -> None:
        """Releases this process's mapping. Safe to call more than once."""
        if self._handle:
            self._lib.animus_shm_ring_order_close(self._handle)
            self._handle = None

    def __del__(self) -> None:
        try:
            self.close()
        except Exception:
            pass


def _configure_security_signatures(lib: ctypes.CDLL) -> None:
    lib.animus_security_create_context.argtypes = []
    lib.animus_security_create_context.restype = ctypes.c_void_p
    lib.animus_security_close_context.argtypes = [ctypes.c_void_p]
    lib.animus_security_close_context.restype = None
    lib.animus_security_create_tenant.argtypes = [ctypes.c_void_p, ctypes.POINTER(AccessToken), ctypes.c_uint32, ctypes.c_size_t]
    lib.animus_security_create_tenant.restype = ctypes.c_bool
    lib.animus_security_create_execution_tenant.argtypes = [ctypes.c_void_p, ctypes.POINTER(AccessToken), ctypes.c_uint32]
    lib.animus_security_create_execution_tenant.restype = ctypes.c_bool
    lib.animus_security_submit_order.argtypes = [
        ctypes.c_void_p, ctypes.POINTER(AccessToken), ctypes.POINTER(OrderRequest), ctypes.POINTER(ExecutionReport),
    ]
    lib.animus_security_submit_order.restype = ctypes.c_bool
    lib.animus_security_poll_execution_audit_log.argtypes = [ctypes.c_void_p, ctypes.POINTER(AuditEvent), ctypes.c_size_t]
    lib.animus_security_poll_execution_audit_log.restype = ctypes.c_size_t


class SecurityContext:
    """ctypes-backed wrapper over one RBAC-gated multi-tenant context
    bundling animus::security::TenantRegistry + SecureTelemetryGateway +
    SecureExecutionGateway (animus_security.hpp) behind a single handle.

    Every method takes an AccessToken (see AccessToken.make()) and is
    enforced natively against animus::security::RbacPolicy -- this class
    does not re-implement or pre-check RBAC in Python; a denied call simply
    returns False, exactly as the native SecureTelemetryGateway/
    SecureExecutionGateway methods it wraps do. Use
    poll_execution_audit_log() to see *why* a call was denied (or
    confirmed allowed), not the return value of the call itself.

    Deliberately narrow: only tenant creation (a prerequisite for
    create_execution_tenant), execution-tenant creation, order submission,
    and execution audit-log polling are exposed here -- record_event/
    add_rule/poll_signals/persistence are SecureTelemetryGateway's existing
    surface and are out of scope for this class, which exists for RBAC'd
    order routing specifically, not as a general Python RBAC'd telemetry
    API.

    Requires the native engine; there is no pure-Python fallback (a native
    security boundary, same reasoning as AnimusBindings.verify_license()).
    """

    def __init__(self, handle: int, lib: ctypes.CDLL) -> None:
        self._handle = handle
        self._lib = lib

    @classmethod
    def create(cls) -> "SecurityContext":
        lib = load_native_library(required=True)
        _configure_security_signatures(lib)
        handle = lib.animus_security_create_context()
        if not handle:
            raise OSError("failed to allocate SecurityContext (out of memory?)")
        return cls(handle, lib)

    def create_tenant(self, token: AccessToken, new_tenant_id: int, buffer_capacity: int = 65536) -> bool:
        """Requires Role.ADMIN (Permission.MANAGE_TENANTS). Allocates the new
        tenant's isolated telemetry Engine -- a prerequisite for
        create_execution_tenant() below, not itself an execution action.
        """
        return bool(self._lib.animus_security_create_tenant(
            self._handle, ctypes.byref(token), ctypes.c_uint32(new_tenant_id), ctypes.c_size_t(buffer_capacity),
        ))

    def create_execution_tenant(self, token: AccessToken, tenant_id: int) -> bool:
        """Requires Role.ADMIN (Permission.MANAGE_TENANTS) AND tenant_id's
        telemetry tenant to already exist via create_tenant() above. Wires
        one LoopbackBrokerGateway + one ExecutionClient bound to that
        tenant's Engine. Idempotent: calling this again for an
        already-set-up tenant is a no-op success.
        """
        return bool(self._lib.animus_security_create_execution_tenant(
            self._handle, ctypes.byref(token), ctypes.c_uint32(tenant_id),
        ))

    def submit_order(self, token: AccessToken, request: OrderRequest) -> Optional[ExecutionReport]:
        """Requires Role.OPERATOR or Role.ADMIN (Permission.SUBMIT_ORDER),
        routed to the token's own tenant_id -- never a caller-chosen tenant.
        Returns the ExecutionReport on success, or None for a denied
        token, a tenant with no execution path set up yet, or a
        broker-rejected order alike (poll_execution_audit_log()
        distinguishes why, not this return value -- see this class's own
        docstring).
        """
        report = ExecutionReport()
        ok = self._lib.animus_security_submit_order(
            self._handle, ctypes.byref(token), ctypes.byref(request), ctypes.byref(report),
        )
        return report if ok else None

    def poll_execution_audit_log(self, max_count: int = 1024) -> List[AuditEvent]:
        """Drains up to max_count pending execution RBAC decisions (allowed
        and denied alike). Never blocks; returns fewer than max_count
        (including zero/empty) if fewer are currently pending.
        """
        buf = (AuditEvent * max_count)()
        count = self._lib.animus_security_poll_execution_audit_log(self._handle, buf, ctypes.c_size_t(max_count))
        return list(buf[:count])

    def close(self) -> None:
        """Releases this context. Safe to call more than once."""
        if self._handle:
            self._lib.animus_security_close_context(self._handle)
            self._handle = None

    def __del__(self) -> None:
        try:
            self.close()
        except Exception:
            pass
