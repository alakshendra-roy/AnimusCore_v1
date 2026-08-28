"""High-level @trace decorator: automatic per-call telemetry instrumentation.

Wraps a function so every call is recorded as one native telemetry event
(via the same AnimusBindings.record_event zero-copy ring-buffer push used
by ingest_engine.py) with metric_value set to the call's wall-clock
duration in nanoseconds -- turning ad hoc "call record_event manually
around this function" instrumentation into a single annotation.
"""
import functools
import itertools
import threading
import time
import zlib
from typing import Any, Callable, Optional, TypeVar

from .bindings import AnimusBindings

F = TypeVar("F", bound=Callable[..., Any])

_DEFAULT_RING_CAPACITY = 65536

_bindings: Optional[AnimusBindings] = None
_bindings_lock = threading.Lock()
_init_failed = False
_trace_id_counter = itertools.count(1)


def _get_bindings(ring_capacity: int) -> Optional[AnimusBindings]:
    """Lazily creates and initializes the process-wide AnimusBindings used
    by @trace. Returns None (rather than raising) if the native library
    can't be loaded or initialized, so instrumentation failures never take
    down the function being traced.
    """
    global _bindings, _init_failed
    if _bindings is not None:
        return _bindings
    if _init_failed:
        return None
    with _bindings_lock:
        if _bindings is not None:
            return _bindings
        if _init_failed:
            return None
        try:
            candidate = AnimusBindings()
            if not candidate.init(ring_capacity):
                raise RuntimeError("animus_init failed")
        except Exception as exc:  # native lib missing/unbuildable on this platform
            _init_failed = True
            print(f"[animus.trace] WARNING: native engine unavailable, tracing disabled ({exc})")
            return None
        _bindings = candidate
        return _bindings


def _stable_event_id(qualname: str) -> int:
    """Deterministic uint32 event_id derived from a function's qualified
    name -- stable across runs/processes, unlike Python's salted hash().
    """
    return zlib.crc32(qualname.encode("utf-8")) & 0xFFFFFFFF


def trace(_func: Optional[F] = None, *, event_id: Optional[int] = None, ring_capacity: int = _DEFAULT_RING_CAPACITY) -> Any:
    """Records one telemetry event per call to the decorated function.

    Usable bare (`@animus.trace`) or parameterized (`@animus.trace(event_id=42)`).
    `metric_value` on the recorded event is the call's wall-clock duration
    in nanoseconds; `trace_id` is a process-local monotonically increasing
    call counter. The wrapped function's return value and exceptions pass
    through unchanged -- a failed or unavailable native engine only disables
    instrumentation, never the function itself.
    """

    def decorator(func: F) -> F:
        resolved_event_id = event_id if event_id is not None else _stable_event_id(func.__qualname__)

        @functools.wraps(func)
        def wrapper(*args: Any, **kwargs: Any) -> Any:
            start = time.perf_counter_ns()
            try:
                return func(*args, **kwargs)
            finally:
                elapsed_ns = time.perf_counter_ns() - start
                bindings = _get_bindings(ring_capacity)
                if bindings is not None:
                    trace_id = next(_trace_id_counter) & 0xFFFFFFFF
                    try:
                        bindings.record_event(
                            event_id=resolved_event_id,
                            trace_id=trace_id,
                            metric_value=elapsed_ns,
                        )
                    except Exception as exc:
                        print(f"[animus.trace] WARNING: failed to record event for {func.__qualname__}: {exc}")

        return wrapper  # type: ignore[return-value]

    if _func is not None:
        return decorator(_func)
    return decorator
