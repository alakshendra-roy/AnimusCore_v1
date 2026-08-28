# Animus Core v1.0: High-Performance Event Processing Engine

![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)
![Python: 3.8+](https://img.shields.io/badge/python-3.8+-blue.svg)
![C++: 17}hhttps://img.shields.io/badge/C+%-17-blue.svg)

## Overview
Animus Core is an enterprise-grade, low-latency telemetry ingestion and automated response engine engineered in C++ with native Python SDK bindings. It bridges C-ABI execution memory boundaries with high-level orchestrators to process high-throughput telemetry streams without zero-copy buffer degradation.

## Key Architectural Principles
* **Direct C-ABI Shared Library Interop:** Bypasses IPC overhead by loading native compiled binaries directly (.dll / .so).
* **Deterministic Execution:** Engineered for high-frequency telemetry parsing and automated mitigation.
* **Zero-Dependency SDK Integration:** Packaged as an installable Python SDK (`pip install -e .`) for seamless staging and production pilots.

## Quick Start

```bash
# Install sdk bindings in editable mode
pip install -e .

# Run SDK validation
python test_sdk.py

3 Run production benchmark suite
python benchmark_suite.py
 ```

## Benchmark Performance
* **Peak Throughput:** >238 Million ops/sec
* **Latency Profile:** Sub-millisecond batch ingestion

## Phase 5: Event-Driven SOAR Orchestration
`AnimusCore_v1/soar_orchestrator.py` closes the loop from raw telemetry to automated response, built directly on the Phase 4 in-memory rule engine rather than re-implementing signature matching in Python:

* **Declarative threat signatures:** `AnimusCore_v1/config/rules.json` defines each signature as an `event_id` / `comparator` / `threshold` triple plus a `severity` and `action` name -- no rebuild required to add or tune a detection.
* **Native rule registration:** on startup, every signature is registered with the engine via `animus_add_rule`, so matching runs zero-copy, in-process, on the same worker thread that persists telemetry to disk (see `EngineImpl::evaluate_rules`).
* **Continuous signal polling:** a background thread drains `animus_poll_signals` on a tight, non-blocking loop rather than polling once after the run completes, avoiding silent signal-ring saturation under sustained load.
* **Automated trigger actions:** each matched `ThreatSignal` is dispatched to a named action handler (e.g. `ISOLATE_HOST`, `TERMINATE_PROCESS`) resolved from the signature that produced it, ready to be swapped for a real integration per action without touching the polling/dispatch loop.

```bash
# Run the SOAR pipeline against a synthetic 600k-event threat stream
python AnimusCore_v1/soar_orchestrator.py --events 600000
```

Verified end-to-end at 600,000 events: 1,455,398 events/sec sustained ingestion, 15,834 threat signals correctly matched and dispatched to automated actions with zero drops. See `AnimusCore_v1/BENCHMARKS.md` for the full Phase 5 benchmark breakdown.

