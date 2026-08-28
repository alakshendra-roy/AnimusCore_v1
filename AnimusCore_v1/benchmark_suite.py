"""
Animus Core v1.0 stress benchmark.

Drives the real compiled C-ABI (AnimusCore_v1.dll) through ctypes - this does
NOT go through animus.core.EventEngine, whose process_telemetry_batch() never
calls into the native engine and cannot produce a meaningful throughput number.
Every metric in the report below comes from an actual measured call into the
lock-free ring buffer + async persistence worker.
"""
import ctypes
import json
import statistics
import time
from pathlib import Path

SCRIPT_DIR = Path(__file__).parent
DLL_PATH = SCRIPT_DIR / "x64" / "Release" / "AnimusCore_v1.dll"

THROUGHPUT_TARGET_EVENTS_PER_SEC = 600_000
LATENCY_TARGET_US = 1000.0  # sub-millisecond average per-event latency
RING_BUFFER_CAPACITY = 1_048_576
BATCH_SIZES = [100_000, 300_000, 600_000, 1_000_000]
PAYLOAD_SIZE_BYTES = 64  # sizeof(TelemetryPayload), alignas(64)


def load_engine():
    if not DLL_PATH.exists():
        raise FileNotFoundError(
            f"Native library not found at {DLL_PATH}. Build the x64 Release "
            f"configuration first (MSBuild AnimusCore_v1.vcxproj /p:Configuration=Release /p:Platform=x64)."
        )
    lib = ctypes.CDLL(str(DLL_PATH))
    lib.animus_init.argtypes = [ctypes.c_size_t]
    lib.animus_init.restype = ctypes.c_bool
    lib.animus_record_event.argtypes = [ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint64]
    lib.animus_record_event.restype = ctypes.c_bool
    lib.animus_start_logging.argtypes = [ctypes.c_char_p]
    lib.animus_start_logging.restype = None
    lib.animus_stop_logging.argtypes = []
    lib.animus_stop_logging.restype = None
    return lib


def run_batch(lib, count: int) -> dict:
    record = lib.animus_record_event
    t0 = time.perf_counter_ns()
    for i in range(count):
        # Spin-retry on backpressure so timing reflects true sustained
        # throughput against the async persistence consumer, not just how
        # many events fit before the ring buffer filled up.
        while not record(101, i & 0xFFFFFFFF, i):
            pass
    t1 = time.perf_counter_ns()

    elapsed_us = (t1 - t0) / 1000.0
    ops_per_sec = count / (elapsed_us / 1_000_000.0) if elapsed_us > 0 else 0.0
    avg_latency_us = elapsed_us / count if count else 0.0

    return {
        "events": count,
        "elapsed_ms": round(elapsed_us / 1000.0, 3),
        "throughput_events_per_sec": round(ops_per_sec, 0),
        "avg_latency_us": round(avg_latency_us, 4),
    }


def run_production_benchmark():
    lib = load_engine()

    print("==================================================")
    print("      ANIMUS CORE v1.0 ENTERPRISE BENCHMARK       ")
    print("==================================================")

    log_path = SCRIPT_DIR / "benchmark_stream.bin"
    if log_path.exists():
        log_path.unlink()

    if not lib.animus_init(RING_BUFFER_CAPACITY):
        raise RuntimeError("animus_init failed")
    lib.animus_start_logging(str(log_path).encode("utf-8"))

    results = []
    for count in BATCH_SIZES:
        result = run_batch(lib, count)
        results.append(result)
        print(
            f"[BATCH TEST] Events: {count:<8} | Time: {result['elapsed_ms']:.2f} ms | "
            f"Throughput: {result['throughput_events_per_sec']:,.0f} events/sec | "
            f"Avg latency: {result['avg_latency_us']:.3f} us/op"
        )

    lib.animus_stop_logging()

    total_events = sum(r["events"] for r in results)
    written_bytes = log_path.stat().st_size if log_path.exists() else 0
    expected_bytes = total_events * PAYLOAD_SIZE_BYTES
    persistence_ok = written_bytes == expected_bytes

    throughputs = [r["throughput_events_per_sec"] for r in results]
    latencies_us = [r["avg_latency_us"] for r in results]

    min_throughput = min(throughputs)
    max_avg_latency = max(latencies_us)

    throughput_pass = min_throughput >= THROUGHPUT_TARGET_EVENTS_PER_SEC
    latency_pass = max_avg_latency < LATENCY_TARGET_US
    overall_pass = throughput_pass and latency_pass and persistence_ok

    metrics = {
        "engine_version": "1.0.0",
        "batches": results,
        "mean_throughput_events_per_sec": round(statistics.mean(throughputs), 0),
        "min_throughput_events_per_sec": round(min_throughput, 0),
        "mean_latency_us": round(statistics.mean(latencies_us), 4),
        "max_avg_latency_us": round(max_avg_latency, 4),
        "throughput_target_events_per_sec": THROUGHPUT_TARGET_EVENTS_PER_SEC,
        "latency_target_us": LATENCY_TARGET_US,
        "total_events_persisted": total_events,
        "bytes_written": written_bytes,
        "bytes_expected": expected_bytes,
        "persistence_integrity_ok": persistence_ok,
        "throughput_target_met": throughput_pass,
        "latency_target_met": latency_pass,
        "overall_pass": overall_pass,
    }

    report_path = SCRIPT_DIR / "BENCHMARK_REPORT.json"
    with open(report_path, "w") as f:
        json.dump(metrics, f, indent=4)

    print("--------------------------------------------------")
    print(f"Persistence integrity check : {'OK' if persistence_ok else 'MISMATCH'} "
          f"({written_bytes} / {expected_bytes} bytes)")
    print(f"Throughput target (>= {THROUGHPUT_TARGET_EVENTS_PER_SEC:,}/sec): "
          f"{'PASS' if throughput_pass else 'FAIL'} (min observed {min_throughput:,.0f}/sec)")
    print(f"Latency target (< {LATENCY_TARGET_US:.0f} us avg): "
          f"{'PASS' if latency_pass else 'FAIL'} (max observed {max_avg_latency:.3f} us)")
    print(f"OVERALL: {'PASS' if overall_pass else 'FAIL'}")
    print(f"Benchmark summary saved to {report_path}")

    return metrics


if __name__ == "__main__":
    run_production_benchmark()
