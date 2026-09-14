# Animus Core v1.1.0: Technical Architecture & Low-Latency Event Processing Engine

## Executive Summary
Animus Core is an enterprise-grade telemetry ingestion and automated response engine engineered in C++ with native Python SDK bindings.

## Key Architectural Principles
* **Direct C-ABI Shared Library Interop:** Bypasses IPC overhead by loading native compiled binaries directly (.dll / .so).
* **Deterministic Execution:** Engineered for high-frequency telemetry parsing and automated mitigation.
* **Zero-Dependency SDK Integration:** Packaged as an installable Python SDK for seamless pilots.

## Performance Metrics
* **Multi-Producer Ring Buffer Throughput (producer-only push rate):** 16.5M+ pushes/sec target (8 concurrent producer threads, ring pre-sized so nothing drains during the timed window; observed range 13.5M–19.7M pushes/sec across runs, hardware-dependent)
* **Multi-Producer Ring Buffer Throughput (simultaneous 8-producer/8-consumer, sustained):** ~7.28M pushes/sec (7.26M–7.29M across a 40-second sustained run, <1% spread) — this is the number that reflects a real deployed workload with a consumer actually draining the ring; the producer-only figure above measures push cost in isolation and should not be read as the throughput under real load
* **Tick-to-Trade Latency (P50):** ~100 ns
* **Zero-Copy Python Bridge (nanobind):** ~34.5 ns/event reference figure; measured range ~22–66 ns/event

Reproduce these figures yourself via the standalone `animus_sandbox/` package (`README.md` — two-command build/run) or see `docs/AnimusCore_Technical_WhitePaper.md` for full benchmark methodology.