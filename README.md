# Animus Core v1.0: High-Performance Event Processing Engine

[![Build](https://github.com/alakshendra-roy/AnimusCore_v1/actions/workflows/build.yml/badge.svg)](https://github.com/alakshendra-roy/AnimusCore_v1/actions/workflows/build.yml)
![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)
![Python: 3.8+](https://img.shields.io/badge/python-3.8+-blue.svg)
![C++: 17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)

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

See `AnimusCore_v1/QUICKSTART.md` for four client proof-of-concept guides
(Python SDK, C++ single-header embedding, secure multi-tenant + mTLS, and
distributed Raft-lite cluster) covering every way to consume Animus Core.

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

## Phase 8: Enterprise Security & Multi-Tenancy

Two independent, composable layers add RBAC, tenant isolation, and encrypted transport on top of the header-only engine, without touching its hot path:

* **RBAC + multi-tenant isolation (`AnimusCore_v1/animus_security.hpp`):** `TenantRegistry` gives every tenant its own isolated `Engine` -- separate ring buffer, rule set, and persistence file -- so isolation is structural, not a filter applied after the fact. `SecureTelemetryGateway` is the only entry point: every call carries an `AccessToken` (tenant + role), is checked against a `Role`/`Permission` lattice (`Viewer` / `Operator` / `Admin`), routed only to that token's own tenant, and logged -- allowed or denied -- to an independent audit trail. Portable C++17, no platform dependency.
* **mTLS / TLS 1.3 transport (`AnimusCore_v1/animus_transport.hpp`):** a Windows-native Schannel (SSPI) transport with mandatory mutual authentication in both directions and manual certificate-chain verification against a private, in-memory-only CA trust anchor. A verified client certificate's subject CN is mapped to an `AccessToken` *only after* its chain is confirmed trusted -- so tenant/RBAC routing downstream is keyed to a cryptographically proven identity, never a value the client merely asserts. Built on the OS-native TLS provider rather than a third-party library, keeping the "zero external dependency" property intact on Windows.
* **Demo certificates (`AnimusCore_v1/generate_demo_certs.ps1`):** generates a self-signed demo CA plus server/client leaf certificates using only native Windows PKI cmdlets -- no OpenSSL. Certificates and keys are written to `AnimusCore_v1/demo_certs/` (gitignored; never commit private key material).

```bash
# Generate demo certs, then build and run the RBAC/tenancy and mTLS demos
# (PowerShell, then an "x64 Native Tools Command Prompt for VS"):
powershell -File AnimusCore_v1/generate_demo_certs.ps1
cl /std:c++17 /EHsc /O2 AnimusCore_v1/secure_multitenancy_demo.cpp
cl /std:c++17 /EHsc /O2 AnimusCore_v1/secure_transport_demo.cpp
secure_multitenancy_demo.exe
secure_transport_demo.exe
```

Verified: the RBAC/tenancy demo confirmed tenant isolation (a second tenant's viewer sees 0 of another tenant's signals), fail-closed behavior against an unknown tenant id, and correct denial of unentitled actions, all captured in an independent audit trail. The mTLS demo negotiated real TLS 1.3 with mutual certificate authentication over loopback TCP and delivered 20,000/20,000 frames at 144,748 frames/sec through the RBAC/tenancy layer above; a negative-path test confirmed a same-CN certificate signed by an untrusted CA is rejected before any frame is processed. See `AnimusCore_v1/BENCHMARKS.md` for the full Phase 8 benchmark breakdown, including two real defects found and fixed during verification.

## Phase 9: Distributed Cloud Orchestration & Clustering

A Raft-lite consensus layer clusters multiple engine nodes over the same mTLS transport Phase 8 established, without pulling in gRPC/Protobuf:

* **Inter-node sync over mTLS, not gRPC (`AnimusCore_v1/animus_cluster.hpp`):** given the choice between real gRPC+Protobuf (this project's first external build dependency) and a custom binary RPC reusing Phase 8's Schannel transport, the latter was chosen explicitly to keep the "zero external dependency" property intact. `SecureChannel` gained generic length-prefixed message framing (`send_message`/`recv_message`) alongside its existing fixed-size telemetry `WireFrame` to carry Raft's variable-length `AppendEntries` calls.
* **Raft-lite leader election & replication:** `RaftNode` implements randomized-timeout election, log-consistency-checked replication with conflict truncation, and the "commit only current-term entries" safety rule -- real Raft, not a stub, just without durable log storage or snapshotting (see the header's documented limitations). Each node keeps its telemetry ingestion fully local and zero-copy; only control-plane `AddRule` commands go through consensus, so every node's rule set converges without touching the hot path.
* **High-availability failover:** cluster membership is static and every node dials every other node over its own mTLS connection. Killing the current leader is detected via election timeout by the survivors, who elect a new leader and keep replicating -- proven with a real node shutdown mid-run, not a simulated clock.

```bash
# Generate demo certs (now includes 3 cluster-node identities), then build and run
# (PowerShell, then an "x64 Native Tools Command Prompt for VS"):
powershell -File AnimusCore_v1/generate_demo_certs.ps1
cl /std:c++17 /EHsc /O2 AnimusCore_v1/cluster_demo.cpp
cluster_demo.exe
```

Verified across 25 consecutive runs (100% pass): a 3-node cluster elects exactly one leader (~500 ms average), a proposed rule replicates to a majority and is confirmed *functionally* on all three independently-running engines (not just as a log entry), a follower correctly redirects a write attempt to the real leader, and killing the leader triggers a real re-election (~716 ms average) after which the survivors keep replicating correctly. See `AnimusCore_v1/BENCHMARKS.md` for the full Phase 9 benchmark breakdown, including four real defects found and fixed during verification -- among them a genuine Raft correctness gap (a new leader unable to commit an inherited pre-term entry without a no-op anchor) that was live-reproduced in roughly 1 in 5 failover runs before being fixed.

## Phase 10: Final Commercial Packaging & Single-Header Release

Animus Core ships as a single distributable artifact per platform: one header for C++ integrators, one wheel for Python integrators, both regenerated from source rather than hand-maintained.

* **Single-header release (`AnimusCore_v1/animus_release.hpp`):** `AnimusCore_v1/amalgamate.py` concatenates all four source headers (`animus.hpp`, `animus_security.hpp`, `animus_transport.hpp`, `animus_cluster.hpp`) into one self-contained file -- a client vendors one `#include`, not four with an inter-file include order to get right. The Windows-only transport/cluster sections are gated on `defined(_WIN32) && defined(_MSC_VER)`, not just `_WIN32`, so the same single header still compiles the portable core engine + RBAC layer under MinGW `g++` (verified with a real build and run) while correctly reserving the Schannel-based sections for MSVC, where their certificate loading actually works.
* **Python SDK, PyPI-ready:** an MIT `LICENSE`, PyPI classifiers, and `Documentation`/`Issues` project URLs were added to `pyproject.toml`; a real `pip wheel . --no-deps` build was installed into a fresh isolated venv (no sibling source checkout) and imported successfully, with the native `AnimusCore_v1.dll` bundled inside the wheel.
* **Multi-node write-latency benchmark (`AnimusCore_v1/cluster_latency_bench.cpp`):** a new benchmark against the Phase 9 cluster, distinct from Phase 9's election/failover timing -- it measures the latency a client actually experiences on every write (`RaftNode::propose()`'s majority-commit latency) separately from full-cluster (not just majority) convergence latency, since conflating them would misrepresent both.
* **Client quickstart guides (`AnimusCore_v1/QUICKSTART.md`):** four proof-of-concept guides -- Python SDK, C++ single header, secure multi-tenant + mTLS, and distributed cluster -- each layering on the previous, with every code sample either compiled and run for real or checked against the current method signatures in the source headers.

```bash
# Regenerate the single header after touching any of the 4 source headers,
# then verify it (from an "x64 Native Tools Command Prompt for VS"):
python AnimusCore_v1/amalgamate.py
cl /std:c++17 /EHsc /O2 AnimusCore_v1/release_header_smoke_test.cpp
release_header_smoke_test.exe

# Build and run the multi-node latency benchmark:
cl /std:c++17 /EHsc /O2 AnimusCore_v1/cluster_latency_bench.cpp
cluster_latency_bench.exe
```

Verified: the amalgamated header was real-compiled and run with both MSVC (all four layers) and MinGW `g++` (portable core + RBAC layer, correctly excluding the Windows-only sections); the wheel build/install/import cycle succeeded end-to-end in a fresh venv; five consecutive cluster-latency-benchmark runs completed with zero failed proposals, showing sub-millisecond p50 majority-commit write latency (0.120-0.377 ms across runs) and full-cluster convergence latency clustering tightly around the 30 ms heartbeat interval. See `AnimusCore_v1/BENCHMARKS.md` for the full Phase 10 benchmark breakdown, including two real defects found and fixed while building the header generator (an under-broad include-stripping regex that caused a genuine type-redefinition compile error, and a guard that checked only `_WIN32` when the code it protected actually required MSVC specifically).

