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

## Phase 4: In-Memory Signal & Threat Evaluation Engine

* **Target System:** Zero-copy threshold-rule evaluator running inline on each batch drained by the async persistence worker (`EngineImpl::evaluate_rules`, `animus_add_rule` / `animus_poll_signals`)
* **Harness:** Same ctypes / 4-producer-thread harness as `ingest_engine.py`, both runs in the same process for an apples-to-apples comparison
* **Batch Size:** 600,000 events per run
* **Ring Allocation:** 2 MiB telemetry ring + 2 MiB signal ring (sized above the run's total signal volume to avoid saturation-related drops)

| Run | Throughput | Avg Latency/op | Wall Time |
|---|---|---|---|
| Baseline (no rules registered) | 291,497 events/sec | 3,430.57 ns | 2,058.34 ms |
| 3 active rules (2 matching, 1 non-matching per event) | 191,734 events/sec | 5,215.57 ns | 3,129.34 ms |

* **Rule Evaluation Overhead:** +52.0% avg latency/op vs. the no-rules baseline, for 3 rules evaluated per event (linear in rule count; the loop is O(rules) per event)
* **Signal Detection Accuracy:** 1,200,000 / 1,200,000 expected signals captured (600,000 events × 2 matching rules), 0 false positives from the non-matching rule, 0 signals dropped
* **Persistence Correctness:** 38,400,000 bytes persisted in both runs (600,000 × 64 bytes/record) — rule evaluation adds no overhead to the disk-persistence path itself
* **Known Limit:** the signal output ring is sized equal to the telemetry ring by default; under high fan-out (multiple matching rules per event) with no concurrent `animus_poll_signals` consumer, it can saturate and silently drop signals past capacity, identical in behavior to the telemetry ring under producer/consumer imbalance
* **Status:** Phase 4 In-Memory Rule Engine Verified