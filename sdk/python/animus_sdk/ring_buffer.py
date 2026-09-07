"""AnimusRingBuffer -- idiomatic Python wrapper over the nanobind native
ring (csrc/animus_ring_buffer.cpp).

This module only wraps lifecycle/producer-side operations (push, synthetic
load control, counters). Consumer-side decoding lives in consumer.py's
AnimusConsumer -- keeping the two separate mirrors the producer/consumer
split the underlying SPSC ring itself enforces (exactly one of each), and
lets a caller that only ever produces (e.g. a real market-data adapter
feeding the ring from native code elsewhere) depend on this module alone.
"""

from __future__ import annotations


def _import_native():
    try:
        from . import _animus_sdk_native as native
        return native
    except ImportError as exc:
        import_error = exc

    # The `animus_sdk` package resolved to this file's own directory (e.g.
    # an uncompiled source checkout earlier on sys.path than an installed
    # wheel), which has no compiled extension. Search the rest of sys.path
    # and site-packages directly for an installed animus_sdk/ that does,
    # rather than giving up -- this makes the import work regardless of
    # which `animus_sdk` a plain `from . import` picked.
    import importlib.util
    import site
    import sys
    from pathlib import Path

    this_pkg_dir = Path(__file__).resolve().parent
    search_dirs = [Path(p) for p in sys.path if p and Path(p).resolve() != this_pkg_dir]
    for getter in (site.getsitepackages, lambda: [site.getusersitepackages()]):
        try:
            search_dirs.extend(Path(p) for p in getter())
        except AttributeError:
            pass

    seen = set()
    for d in search_dirs:
        d = d.resolve() if d.exists() else d
        if d in seen:
            continue
        seen.add(d)
        pkg_dir = d / "animus_sdk"
        if pkg_dir == this_pkg_dir or not pkg_dir.is_dir():
            continue
        for so_path in sorted(pkg_dir.glob("_animus_sdk_native*")):
            spec = importlib.util.spec_from_file_location("animus_sdk._animus_sdk_native", so_path)
            if spec is None or spec.loader is None:
                continue
            native = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(native)
            sys.modules.setdefault("animus_sdk._animus_sdk_native", native)
            return native

    raise ImportError(
        "animus_sdk requires the compiled _animus_sdk_native extension. "
        "Build it from a full source checkout: `pip install ./sdk/python` "
        "(see sdk/python/CMakeLists.txt), or for fast local iteration, the "
        "direct-CMake steps documented at the bottom of that file."
    ) from import_error


class AnimusRingBuffer:
    """A fixed-capacity, single-producer/single-consumer ring of
    TelemetryFrame records, backed by animus::eval::SpscRingBuffer<T>
    (animus-eval-kit/include/spsc_ring_buffer.hpp) -- the exact same C++20
    ring animus_bench measures.

    Not thread-safe beyond the SPSC contract the native ring itself
    enforces and does not check at runtime: exactly one thread may call a
    consumer-side method (poll()/drain(), via AnimusConsumer) at a time,
    and it must not be the same thread driving push() or the background
    producer started by start_synthetic_load().
    """

    def __init__(self, capacity: int, drain_batch_capacity: int = 8192):
        self._native = _import_native().RingBuffer(capacity, drain_batch_capacity)

    def __enter__(self) -> "AnimusRingBuffer":
        return self

    def __exit__(self, *exc_info) -> None:
        self.stop_synthetic_load()

    def push(self, sequence_id: int, symbol: str, price: float, volume: int, flags: int = 0) -> bool:
        """Push one real frame from Python. Never blocks; False means the
        ring was full. For a hot real-time feed, prefer wiring the native
        producer directly into the ring from C++ rather than pushing
        one-by-one through Python -- this call pays normal Python/ctypes-
        free-but-still-interpreted call overhead per frame."""
        return self._native.push(sequence_id, symbol, price, volume, flags)

    def start_synthetic_load(self, frame_count: int = 0, target_frames_per_sec: float = 0.0) -> None:
        """Start a background native producer thread generating synthetic
        TelemetryFrame load, for exercising Python-side consumption
        throughput without wiring up a real second producer.
        frame_count=0 runs until stop_synthetic_load();
        target_frames_per_sec<=0 is unthrottled (push as fast as the ring
        accepts -- what bench_python_throughput.py uses)."""
        self._native.start_producer(frame_count, target_frames_per_sec)

    def stop_synthetic_load(self) -> None:
        if self._native.producer_running():
            self._native.stop_producer()

    @property
    def producer_running(self) -> bool:
        return self._native.producer_running()

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
