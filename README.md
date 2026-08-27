# ANIMUS Core Engine v1.0

An ultra-low latency, thread-safe asynchronous telemetry logging engine written in C++. Designed for high-frequency trading (HFT) and ultra-high-throughput systems.

## Features
- **Cache-Line Aligned Atomic Pointers (`alignas(64)`):** Prevents false sharing across core threads.
- **Lock-Free Ring Buffer Ingestion:** Achieves sub-20 ns hot-path recording.
- **Asynchronous Disk Persistence Worker:** Dedicated worker thread with batch-flushing mechanism (`BATCH_SIZE = 1024`) to eliminate file I/O thread bottlenecks.

---

## Environment & Build Configuration
- **Platform:** Windows 11 (x64) | HP Omen 15
- **Compiler:** MSVC (Visual Studio 2022)
- **Configuration:** Release | x64 (`/O2`)

---

## Benchmark Progression (600,000 Events)

### **Phase 2: In-Memory Ingestion Stress Test**
- **Total Ingested Events:** 600,000 / 600,000 (100% Success)
- **Total Test Duration:** 6.897 ms
- **Sustained Throughput:** 86.9943 Million ops/sec
- **Average Hot-Path Latency:** 11.495 ns / op
- **p99 Tail Latency:** 100 ns

### **Phase 3: Asynchronous Binary Disk Persistence**
- **Total Ingested Events:** 600,000 / 600,000 (100% Success)
- **Total Test Duration:** 23.1294 ms
- **Sustained Throughput:** 25.941 Million ops/sec
- **Average Hot-Path Latency:** 19.963 ns / op
- **p99 Tail Latency:** 100 ns
- **Persistence Output:** `telemetry_data.bin` (Direct binary stream)
