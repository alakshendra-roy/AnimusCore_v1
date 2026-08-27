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