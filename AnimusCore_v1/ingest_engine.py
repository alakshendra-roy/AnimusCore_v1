import ctypes
import time

dll_path = r"C:\Users\Alaks\source\repos\AnimusCore_v1\x64\Release\AnimusCore_v1.dll"
animus = ctypes.CDLL(dll_path)

animus.animus_init.argtypes = [ctypes.c_size_t]
animus.animus_init.restype = ctypes.c_bool

animus.animus_record_event.argtypes = [ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint64]
animus.animus_record_event.restype = ctypes.c_bool

animus.animus_start_logging.argtypes = [ctypes.c_char_p]
animus.animus_start_logging.restype = None

animus.animus_stop_logging.restype = None

if __name__ == "__main__":
    print("[Python] Initializing Animus Engine C++ Core...")
    if animus.animus_init(65536):
        print("[Python] C++ Core initialized with 64k ring buffer.")
        animus.animus_start_logging(b"telemetry_stream.bin")
        start_time = time.perf_counter_ns()
        for i in range(600000):
            animus.animus_record_event(101, i, 9999)
        end_time = time.perf_counter_ns()
        total_ms = (end_time - start_time) / 1e6
        avg_ns = (end_time - start_time) / 600000
        print("[Python] Ingested 600,000 events successfully")
        print("[Python] Execution Time ms:", round(total_ms, 2))
        print("[Python] Latency per op ns:", round(avg_ns, 2))
        animus.animus_stop_logging()
        print("[Python] Phase 4 C-ABI Interop Test Complete.")