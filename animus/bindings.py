import os
import sys
import ctypes
from typing import Optional

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
