"""animus-py -- Zero-copy Python SDK for Animus Core's C++20 SPSC ring buffer.

Institutional client-facing package: pulls TelemetryFrame records (the same
64-byte, cache-line-aligned wire format animus_bench measures -- see
animus-eval-kit/include/telemetry_frame.hpp) straight out of the native
lock-free ring and into NumPy, with no per-frame Python object construction
and no heap allocation on the ingestion hot path.

    from animus_sdk import AnimusRingBuffer, AnimusConsumer

    ring = AnimusRingBuffer(capacity=1 << 20)
    consumer = AnimusConsumer(ring)
    ring.start_synthetic_load(target_frames_per_sec=5_000_000)
    while True:
        frames = consumer.to_numpy(consumer.drain())
        ...  # frames is a zero-copy structured ndarray

See bench_python_throughput.py for a full ingestion-loop benchmark, and
../../docs/INSTITUTIONAL_INTEGRATION_GUIDE.md for the architectural
background (memory ordering, cache-line isolation, kernel tuning).
"""

from .consumer import AnimusConsumer, TelemetryFrame, decode, decode_iter, to_numpy
from .ring_buffer import AnimusRingBuffer

__all__ = [
    "AnimusRingBuffer",
    "AnimusConsumer",
    "TelemetryFrame",
    "decode",
    "decode_iter",
    "to_numpy",
]

__version__ = "1.0.0"
