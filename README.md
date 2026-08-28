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

## Phase 6: SDK Packaging, Shared-Memory IPC & @trace

The SDK is now a proper installable wheel with cross-process shared-memory transport and one-line function instrumentation:

* **Wheel packaging:** `pyproject.toml` carries PEP 621 metadata; `setup.py`'s `build_py` override stages the compiled native library (`AnimusCore_v1.dll` / `libAnimusCore.so` / `.dylib`) into `animus/` before packaging, so `pip wheel .` produces a self-contained wheel -- no sibling source checkout required at install time.
* **Shared-memory IPC:** `animus.shm.SharedTelemetryRing` is a zero-copy single-producer/single-consumer ring living in an OS-level shared memory segment (`multiprocessing.shared_memory`, stdlib-only), letting two separate processes exchange telemetry records with no serialization step -- complementary to the native engine's in-process ring, which only one process can see.
* **`@animus.trace` decorator:** wraps any function so each call is recorded as one native telemetry event (duration in nanoseconds as `metric_value`), usable bare or parameterized (`@animus.trace(event_id=42)`), and degrades gracefully rather than breaking the wrapped function if the native engine can't load.

```bash
# Build and verify a self-contained wheel
pip wheel . --no-deps

# Run the cross-process shared-memory IPC demo (spawns a real consumer process)
python AnimusCore_v1/shm_ipc_demo.py
```

Verified: a built wheel installed into an isolated venv and imported/exercised from a directory with no sibling repo files; the shared-memory IPC demo moved 200,000 events across two OS processes at 1,177,592 events/sec with zero drops; `@animus.trace` adds ~908.5 ns/call on top of an already-warm native engine. See `AnimusCore_v1/BENCHMARKS.md` for the full Phase 6 benchmark breakdown.

## Phase 7: Header-Only Engine & Broker/Execution Interop

`AnimusCore_v1/animus.hpp` is now a genuine zero-dependency, single-header C++17 library, with direct execution-path wrappers for ultra-low-latency deployment:

* **Header-only engine:** `EngineImpl` and `Engine::Create()` are defined `inline` directly in `animus.hpp` -- any C++17 translation unit can `#include "animus.hpp"` and drive `animus::Engine` in-process, with no `AnimusCore_v1.dll` to build or link and no ctypes/C-ABI boundary to cross. The Python SDK's DLL build (`animus_engine.cpp`) is now just a thin C-ABI shim over this same header.
* **Broker/execution interop wrappers:** `animus::IBrokerGateway` is an adapter interface for a real broker/exchange connection (a FIX session, a REST-to-exchange bridge, ...); `animus::ExecutionClient` wraps a gateway and automatically records every order's round-trip latency as a telemetry event against the shared `Engine` -- so a latency-risk check (e.g. "flag any fill slower than N nanoseconds") is just an ordinary `add_rule`/`poll_signals` SOAR rule, not a separate pipeline.
* **`LoopbackBrokerGateway`:** a deterministic in-process fill simulator included for demos and testing `ExecutionClient` without a live broker connection.

```bash
# Build and run the standalone execution-interop demo (zero DLL, zero Python)
g++ -std=c++17 -O2 -pthread AnimusCore_v1/execution_interop_demo.cpp -o execution_interop_demo.exe
./execution_interop_demo.exe
```

Verified: the header-only refactor was rebuilt with real MSBuild passes for both the DLL (`Release|x64`) and the exe (`Debug|Win32`) configurations with no regressions, and cross-checked for ODR safety by linking two separate translation units that each `#include "animus.hpp"`; the execution-interop demo routed 500,000 simulated orders at 8,464,120 orders/sec with a 90.16 ns average / 100 ns p99 `submit()` latency. See `AnimusCore_v1/BENCHMARKS.md` for the full Phase 7 benchmark breakdown.

