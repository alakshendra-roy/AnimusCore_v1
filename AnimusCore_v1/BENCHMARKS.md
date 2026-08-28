# ANIMUS Core Engine v1.0 - Performance Benchmarks

## Environment
- **Platform:** Windows 11 (x64) | HP Omen 15
- **Compiler:** MSVC (Visual Studio 2022)
- **Configuration:** Release | x64 (/O2)

## Benchmark Results (600,000 Sequential Telemetry Events)
- **Total Ingested Events:** 600,000 / 600,000 (100% Success)
- **Total Test Duration:** 6.897 ms
- **Sustained Throughput:** 86.9943 Million ops/sec
- **Average Hot-Path Latency:** 11.495 ns / op
- **p99 Tail Latency Ceiling:** 100 ns
- ## Phase 3 Benchmark Results (Asynchronous Binary Persistence)
- **Total Ingested Events:** 600,000 / 600,000 (100% Success)
- **Total Test Duration:** 23.1294 ms
- **Sustained Throughput:** 25.941 Million ops/sec
- **Average Hot-Path Latency:** 19.963 ns / op
- **p99 Tail Latency:** 100 ns
- **Persistence Target:** `telemetry_data.bin` (Direct binary stream)
- ## Phase 4: C-ABI Python Interop Benchmarks

* **Target Architecture:** C++ DLL (`AnimusCore_v1.dll`) via Python `ctypes`
* **Batch Size:** 600,000 events
* **Ring Buffer Allocation:** 64 KiB
* **Total Execution Time:** 545.95 ms
* **Average Latency per Op:** 909.92 ns (~0.91 µs)
* **Status:** Phase 4 Interop Verified

## Phase 4 (Validated Re-run): C-ABI Python Interop Benchmarks

* **Target Architecture:** C++ DLL (`AnimusCore_v1.dll`) via Python `ctypes`, single producer thread
* **Batch Size:** 600,000 sequential `animus_record_event` calls
* **Ring Buffer Allocation:** 64 KiB
* **Sustained Throughput:** 1,009,680 events/sec
* **Average Latency per Op:** 0.990 µs (990 ns)
* **Measurement Method:** Python-side wall clock (`time.perf_counter_ns`) spanning the full call loop; includes `ctypes` call-marshalling overhead, not just native ring-buffer push time
* **Status:** Phase 4 Interop Verified (re-run against current build)
## Phase 5: SOAR Real-Time Orchestration Benchmarks

* **Target System:** Real-Time Threat Evaluation & Automated Response Pipeline
* **Evaluated Stream:** 100,000 telemetry events
* **Threat Vectors Identified & Mitigated:** 200 high-priority vectors
* **Pipeline Execution Time:** 13.71 ms
* **Status:** Phase 5 Real-Time Orchestration Verified