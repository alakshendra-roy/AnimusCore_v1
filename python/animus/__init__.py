"""Animus Engine -- nanobind zero-copy telemetry stream interop layer.

This is a separate, optional package (installed via `pip install
./bindings`, see bindings/pyproject.toml) from the root animus-engine-sdk
ctypes SDK -- both currently install as top-level `animus`, so install
only one of the two into a given environment until/unless they are
merged. See bindings/CMakeLists.txt's header comment for why this stayed
a standalone build rather than folding into the existing package.
"""

from .consumer import (
    TelemetryConsumer,
    TelemetryRecord,
    WIRE_FORMAT,
    WIRE_RECORD_SIZE,
    decode,
    decode_iter,
    to_numpy,
)

__all__ = [
    "TelemetryConsumer",
    "TelemetryRecord",
    "WIRE_FORMAT",
    "WIRE_RECORD_SIZE",
    "decode",
    "decode_iter",
    "to_numpy",
]

__version__ = "0.1.0"
