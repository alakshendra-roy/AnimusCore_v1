# Animus Core v1.0: High-Performance Event Processing Engine

[![Build](https://github.com/alakshendra-roy/AnimusCore_v1/actions/workflows/build.yml/badge.svg)](https://github.com/alakshendra-roy/AnimusCore_v1/actions/workflows/build.yml)
![License: Proprietary](https://img.shields.io/badge/license-Proprietary-red.svg)
![Python: 3.8+](https://img.shields.io/badge/python-3.8+-blue.svg)
![C++: 17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)

> **`proprietary-edition` branch.** This branch adds an offline RSA-signed
> hardware licensing layer (Phase 17) and lock-free market data feed
> adapters (Phase 18 below) on top of the MIT `master` branch and is
> licensed differently -- see `LICENSE` in this branch, which is a
> placeholder pending real legal review, not the MIT text on `master`.

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

# Compare native vs. pure-Python ingestion, including batched ingestion
python benchmarks/benchmark_engine.py

# Stress-test sustained load (1.2M+ events), memory leaks, and C-ABI boundary safety
python benchmarks/stress_test_engine.py

# Tail latency (p50-p99.99) for batched ingestion, baseline vs. SPSC ring + CPU pinning
python benchmarks/fintech_tail_latency.py

# Tick-to-trade latency, 8-thread ring buffer throughput, and CPU cache
# locality -- compiles and runs a native C++ benchmark binary, then
# renders benchmarks/BENCHMARK_REPORT.md
python benchmarks/generate_benchmark_report.py
 ```

See `AnimusCore_v1/QUICKSTART.md` for six client proof-of-concept guides
(Python SDK, C++ single-header embedding, secure multi-tenant + mTLS,
distributed Raft-lite cluster, enterprise licensing, and market data feed
adapters) covering every way to consume Animus Core.

Running a pilot evaluation? See [`Pilot_Kit/`](Pilot_Kit/PILOT_README.md)
for a minimal customer-facing quickstart -- setup, a runnable
sub-microsecond ingestion example, and how 30-day evaluation licenses work.

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

## Phase 11: Batched Event Ingestion

`benchmarks/benchmark_engine.py` (comparing the native engine against an equivalent pure-Python dict-loop implementation) isolated a real bottleneck the earlier per-phase benchmarks never surfaced: at 100,000 events, driving `animus_record_event()` from Python one call at a time lost to a pure in-memory Python loop by more than 3x -- even with disk I/O and rule evaluation both removed. A follow-up measurement pinned the cause precisely: the native C-ABI call itself processes 100,000 events in ~2.5 ms; it was the ctypes call-marshalling overhead of 100,000 individual foreign-function calls, not the ring-buffer push or any native-side work, that dominated.

* **`animus_record_events_batch`** (`AnimusCore_v1/animus.hpp`, `animus_engine.cpp`) pushes a whole batch of events onto the ring buffer in one C-ABI call instead of one call per event, exposed from Python as `AnimusBindings.record_events_batch()`.
* **A second bottleneck found while fixing the first:** the initial implementation still built one `ctypes.Structure` object per event before the call, which only closed ~10% of the gap -- Python object-construction cost, not the FFI call, was still dominant. Fixed by packing the batch with `struct.pack()` and a single `ctypes.memmove()` into the array's backing memory instead, roughly 6x faster to build at 100,000 events.

```python
from animus.bindings import AnimusBindings

bindings = AnimusBindings()
bindings.init(buffer_capacity=100_000)
events = [(event_id, trace_id, metric_value), ...]
pushed = bindings.record_events_batch(events)  # one native call, not len(events)
```

```bash
python benchmarks/benchmark_engine.py
```

Measured (100,000 events, both the MSVC `AnimusCore_v1.dll` build and the CMake `AnimusNative.dll` build): batched ingestion ran ~1.98-2.38x pure-Python's dict-loop throughput and ~6.8-7.5x the per-event `record_event()` ingestion path, reproducible across repeated runs on both binaries.

## Phase 12: Stress Test -- Sustained Load, Memory Leaks & C-ABI Boundary Safety

`benchmarks/stress_test_engine.py` pushes past the batch-size benchmarks above into two questions Phase 11 didn't answer: does memory grow unbounded under real sustained load, and what actually happens when `animus_record_events_batch`'s raw C-ABI is fed input it has no way to validate?

* **Sustained-load memory check:** 1,200,000+ events through the full pipeline (`record_events_batch` -> rule evaluation -> disk persistence), sampling this process's resident memory (RSS) between batches. The first version of this check flagged a false-positive leak (+24.85% growth) by measuring from RSS immediately after `init()`, before the ring buffers' pages were ever touched by a write -- OS lazy page-commit made that early growth look like a leak. Fixed by using a "warm" baseline (after the ring has cycled twice) instead.
* **C-ABI boundary fuzzing:** malformed *event data* (out-of-range fields, wrong tuple arity) through the public SDK -- always safely rejected by `struct.pack()` before any native call. Malformed *pointer/count* arguments against the raw C-ABI directly -- run in isolated subprocesses, since a `count` that lies about the buffer's real size is a genuine out-of-bounds read that can (and does) segfault the process reading it. The public `record_events_batch()` API is never reachable this way; only bypassing it and calling the raw library handle directly is.

```bash
python benchmarks/stress_test_engine.py
```

Measured across 5 consecutive runs: RSS growth from the warm baseline stayed in a 1.82-2.49% band (consistent with no leak), with all 108,000 expected threat signals matched and drained every run, zero loss. Boundary fuzzing found the out-of-bounds-count cases crash non-deterministically -- depending on heap layout, not the input -- so a run that doesn't crash isn't proof the input was safe; that distinction is reported explicitly rather than glossed over. See `AnimusCore_v1/BENCHMARKS.md`'s Phase 12 section for the full breakdown, including a real ctypes bug (a truncated 64-bit process handle) found and fixed while building the memory-check harness itself.

## Phase 13: Fintech-Style Tail Latency

`benchmarks/fintech_tail_latency.py` asks a different question than throughput: not "how fast on average," but "how bad is the tail" -- p50 through p99.99 -- for `record_events_batch()` at batch sizes of 100 / 1,000 / 10,000 events, timing only the native call itself (`time.perf_counter_ns()`, batch construction excluded from the measured window).

```bash
python benchmarks/fintech_tail_latency.py
```

Measured across 5 consecutive runs: at batch size 100, p50 is ~13.7 us but p99.99 is ~385 us -- roughly a 28x tail. At batch size 10,000, p50 is ~1,373 us and p99.99 ~1,959 us -- only ~1.4x. Smaller batches are dominated by fixed per-call jitter (OS scheduling, Python-level GC, allocator stalls) that their small amount of real work barely amortizes; larger batches trade higher absolute latency for a *tighter* tail relative to their own median. See `AnimusCore_v1/BENCHMARKS.md`'s Phase 13 section for the full per-batch-size breakdown and sample-size caveats on p99.99.

## Phase 14: Lock-Free SPSC Ring Buffer & CPU Core Pinning

Two new, fully additive primitives -- neither touches the existing `Engine` or its MPMC ring -- aimed at the one-dedicated-hot-thread case a general-purpose ingestion API can't specialize for:

* **`animus::SpscRingBuffer<T>`** (`AnimusCore_v1/animus.hpp`): a single-producer/single-consumer ring using a plain atomic load/store pair instead of the MPMC ring's compare-exchange retry loop, backed by one contiguous pre-allocated `std::vector<T>`. Fully header-only -- usable from C++ with zero DLL, same as `Engine`. Exposed as a standalone C-ABI channel (`animus_spsc_init` / `animus_spsc_record_events_batch` / `animus_spsc_drain`), and from Python as `AnimusBindings.spsc_*`.
* **`animus_pin_current_thread_to_core(core_id)` / `animus_get_cpu_count()`** (Windows: `SetThreadAffinityMask`; Linux: `pthread_setaffinity_np`; honestly returns `false` on platforms with no real pinning API rather than faking success), exposed from Python as `AnimusBindings.pin_current_thread_to_core()` / `get_cpu_count()`.

`benchmarks/fintech_tail_latency.py` was extended to compare baseline (MPMC, unpinned) against SPSC + pinned. The first version pinned to the highest-numbered CPU core -- a common informal convention -- and measured tail latency up to **34.5x worse**: this development machine has a hybrid Intel P-core/E-core CPU, and the last core is an Efficiency core. There is no portable way to ask the OS which cores are P-cores, so the benchmark now *probes* several candidate cores with a cheap workload and pins to whichever measures fastest, rather than guessing.

```bash
python benchmarks/fintech_tail_latency.py
```

With a well-chosen core, measured across 5 runs: **throughput and p50/p90 latency improve in every single trial** (15/15) -- real, consistent gains from cache locality and the simpler SPSC push path. p99 is a mixed bag (9/15 improved). **p99.99 gets worse more often than not (12/15 trials)**, sometimes by 5-6x, and this did not go away with a properly probed, fast core. Likely cause: `SetThreadAffinityMask`/`pthread_setaffinity_np` pin a thread but don't reserve a core *exclusively* -- real OS-level isolation would take `isolcpus`/CPU Sets, which this benchmark deliberately doesn't set up. An unpinned thread that hits contention can migrate to any idle core; a pinned thread has nowhere to go until its one core frees up, which specifically inflates the rare worst case even as it helps the common one. Reported as measured, not adjusted to match the "pinning reduces p99.99" outcome this phase originally set out to confirm. See `AnimusCore_v1/BENCHMARKS.md`'s Phase 14 section for the full per-batch-size numbers, the core-probe data, and the `alignas(64)` ctypes bug caught while building the SPSC drain path.

## Phase 15: Complex Event Processing (CEP) -- Sliding-Window Aggregation Rules

`add_rule()` evaluates each event on its own; real detection logic is often about a *window* of recent events instead -- "the sum of the last 100 orders exceeds X," "the average latency over the last 5 seconds is above Y." `animus::CepRuleState` (`AnimusCore_v1/animus.hpp`) adds that directly to the native hot path, fully additive to the existing `RuleThreshold` engine:

* **Count- and time-based sliding windows, SUM/AVG/MIN/MAX:** SUM/AVG maintain an O(1)-amortized running total; MIN/MAX use the standard monotonic-deque sliding-window algorithm (O(1) amortized, not a per-event rescan). AVG's threshold check cross-multiplies (`sum > threshold * count`) rather than dividing, so the comparison stays exact integer arithmetic -- no floating point anywhere in the CEP hot path.
* **New `Engine::add_cep_rule()` / `animus_add_cep_rule` C-ABI export**, evaluated on the same persistence-worker thread and delivered through the same `poll_signals()` queue as plain threshold rules -- a caller distinguishes which rule fired via `rule_id`, not a separate API.
* **Python only registers rules; all per-event evaluation stays in C++** -- `AnimusBindings.add_cep_rule()`, with `WindowType`/`AggregationFunction` exported at the `animus` top level, plus a full pure-Python fallback so the engine's contract holds without a compiled binary.

Verified before integration, not after: a standalone 200-trial-per-aggregation randomized test (1,608 trial sequences total, both window types, all four aggregations) compared this design against a naive brute-force reference before it went into `animus.hpp` -- zero discrepancies. Real end-to-end round trips through both the native engine and the pure-Python fallback produce identical results for the same input, including a dedicated regression test proving the AVG exact-arithmetic design isn't cosmetic: window `[10, 10, 11]`, `AVG > 10` correctly matches (31 > 30 exactly) where a naive floor-then-compare implementation would wrongly reject it (10 > 10 is false).

Measured native hot-path overhead (300,000 events, 5 consecutive runs): 1 and 10 concurrently registered CEP rules are statistically indistinguishable from the 0-rule baseline; 50 rules costs roughly 2.4-4.0 ns/rule/event, a real but small, linear-in-rule-count effect (~30-40% throughput reduction at 50 rules) consistent with each rule doing O(1) amortized work per matching event. See `AnimusCore_v1/BENCHMARKS.md`'s Phase 15 section for the full breakdown, including a measurement bug (rules silently accumulating across configurations within one process) caught and fixed before these numbers were recorded.

## Phase 16: Cross-Platform Shared-Memory (MMF) IPC Transport

Phase 6 gave two processes a way to exchange telemetry via `animus.shm.SharedTelemetryRing` (pure Python, `multiprocessing.shared_memory`). Phase 16 adds the native counterpart directly to `animus.hpp`:

* **`animus::SharedMemorySegment`** -- RAII over named OS shared memory: Windows `CreateFileMappingA`/`MapViewOfFile`, POSIX `shm_open`/`mmap`. Fully header-only, unlike CPU pinning (Phase 14), which stayed DLL-only specifically because pulling `windows.h` into the core header wasn't worth it for two OS calls -- shared memory is cross-platform and central enough to the transport-layer feature set to justify the tradeoff here.
* **`animus::SharedTelemetryChannel`** -- an SPSC ring living entirely inside the mapped segment, using real `std::atomic<uint64_t>` (`is_always_lock_free` enforced via `static_assert`) rather than the implicit aligned-store atomicity `SharedTelemetryRing` relies on. Deliberately wire-compatible with that existing module (same 24-byte header, same 24-byte record) -- a native producer and a pure-Python consumer, or vice versa, work together on the same segment. A real bug was caught before this shipped: the first draft indexed ring slots with a power-of-two bitmask, silently wrong against a segment the Python side created with a non-power-of-two capacity (which is normal there, since it uses plain modulo); fixed to modulo on the native side to match.
* **New handle-based C-ABI** (`animus_shm_create/attach/close/unlink/push/pop/capacity`) and a corresponding `SharedTelemetryChannel` class in `animus/bindings.py`, exported at the `animus` top level.

Verified, not assumed: both interop directions tested for real (native writes, Python `SharedTelemetryRing` reads, and the reverse); a genuine cross-process test using a real `subprocess.Popen` child, not a second handle in the same process; non-power-of-two capacity with real ring wraparound.

The "sub-microsecond IPC" claim was measured layer by layer, not asserted as one number: the native `push()` call itself is genuinely sub-microsecond (~35-40ns). A single Python ctypes call to it costs ~1.3us instead -- the same marshalling tax documented in Phases 11 and 13 -- and genuine cross-process propagation (one process's write becoming visible to another's read) measured at single-digit-to-low-double-digit microseconds even in native code, not sub-microsecond, on a general-purpose machine with no CPU isolation configured. Along the way, an unpaced Python burst test initially reported 4.4-5.2ms mean latency -- traced (not glossed over) to a producer-faster-than-consumer backlog accumulating linearly across the batch (first event: 14-26us; last event: 13.6ms), a throughput mismatch rather than a transport problem. See `AnimusCore_v1/BENCHMARKS.md`'s Phase 16 section for the full breakdown.

## Phase 17: Enterprise Edition -- Offline RSA-Signed Hardware Licensing

**This phase lives only on the `proprietary-edition` branch, not `master`.**
AnimusCore is MIT-licensed on `master`; a hardware-locked license gate
doesn't fit an open-source tree (anyone with the source can just delete
the check). `proprietary-edition` branches off `master` specifically to
carry this feature, with `LICENSE` replaced by a placeholder "All Rights
Reserved" notice -- explicitly **not** reviewed by counsel, and not to be
distributed to a customer before real legal review.

* **Offline, no phone-home:** `animus_verify_license(path)` (`AnimusCore_v1/animus.hpp`
  / `animus_engine.cpp`) validates a signed license file entirely locally --
  RSA-2048 signature check via Windows CNG/BCrypt (`BCryptVerifySignature`,
  PKCS1 padding + SHA-256), no network call, no license server.
* **Hardware fingerprint, not a serial number:** the fingerprint is
  SHA-256(`MachineGuid` + the machine's primary MAC address), not a literal
  CPU serial -- modern CPUs don't expose one via CPUID for privacy reasons
  since the Pentium III PSN was deprecated. `MachineGuid` comes from
  `HKLM\SOFTWARE\Microsoft\Cryptography`; the MAC is selected via
  `GetAdaptersAddresses`, filtered to the IEEE "locally administered
  address" bit (`(mac[0] & 0x02) == 0`) to exclude Windows' own
  virtual/randomized MACs (a real dev machine surfaced *three* distinct
  Wi-Fi MACs from virtual roles alone before this filter was added), then
  the smallest remaining candidate is chosen for determinism on multi-NIC
  machines.
* **Fail-closed core entitlement:** a license's `max_cores` field gates
  `animus_spsc_init` and `animus_pin_current_thread_to_core` directly --
  neither succeeds until `animus_verify_license` has succeeded in-process,
  and pinning additionally rejects any `core_id >= max_cores`, independent
  of the machine's real hardware core count. No unlicensed default, not
  even core 0.
* **Windows-only by design, not by omission:** `animus_verify_license`
  returns `false` immediately on non-Windows builds (no BCrypt equivalent
  wired up) rather than faking a pass -- an explicit, documented gap
  instead of a silent one.

```powershell
# One-time: generate the RSA-2048 signing keypair (private key is written
# to license_tools/private/, gitignored -- never commit it; the public key
# is baked into AnimusCore_v1/animus_license_pubkey.hpp, safe to commit).
powershell -File AnimusCore_v1/license_tools/generate_license_keypair.ps1

# Issue a license: 8 cores, no expiry, for the machine this runs on.
powershell -File AnimusCore_v1/license_tools/sign_license.ps1 -OutFile customer.lic -MaxCores 8

# Issue a license for a specific customer machine + a 365-day expiry.
powershell -File AnimusCore_v1/license_tools/sign_license.ps1 -OutFile customer.lic -MaxCores 4 -FingerprintHex <64 hex chars from the customer's machine> -ExpiresInDays 365
```

```python
bindings = AnimusBindings()
if not bindings.verify_license("customer.lic"):
    raise SystemExit("license invalid, expired, or for a different machine")
bindings.spsc_init(buffer_capacity=1_000_000)         # now unlocked
bindings.pin_current_thread_to_core(bindings.licensed_max_cores() - 1)
```

Verified end-to-end against the real compiled DLL, not just the signing
tools in isolation: a valid license for the current machine unlocks both
`spsc_init` and pinning up to exactly its `max_cores` boundary (one core
past it correctly fails); a validly-signed license for a *different*
machine's fingerprint, a byte-tampered license, and a nonexistent file
path are all correctly rejected; and a fresh process with no license
verified yet fails closed on both gated calls (confirmed via a
subprocess-isolated test, since license state is a process-wide singleton
that can't be un-set once verified). See `tests/test_bindings.py`'s
`RealNativeEngineIntegrationTests` license tests and `UnlicensedGatingTests`
for the full suite.

## Phase 18: Lock-Free Market Data Feed Adapters (L2/L3 Book + Trade Ticks)

`animus::MarketDataFeed` (`AnimusCore_v1/animus.hpp`) adds a dedicated
low-latency ingestion primitive for live market data, separate from the
general telemetry `Engine` and from the SPSC ring both used for
higher-throughput single-thread paths:

* **Two independent lock-free rings, one per message type:** order-book
  price-level updates (`L2Update`, 56 bytes) and executed-trade ticks
  (`TradeTick`, 56 bytes), each backed by the existing `LockFreeRingBuffer`
  (the Vyukov MPMC ring `EngineImpl`'s own telemetry ring already uses),
  not a new ring algorithm.
* **Genuinely thread-safe for concurrent producers *and* consumers** --
  unlike `SpscRingBuffer`/`SharedTelemetryChannel` elsewhere in this
  header, which are deliberately single-producer/single-consumer for
  extra throughput, `MarketDataFeed` supports multiple feed-handler
  threads (e.g. one per venue connection) pushing into the same instance
  concurrently, and multiple consumer threads draining it concurrently,
  with no external locking.
* **Handle-based, not a singleton:** `animus_feed_create`/`animus_feed_close`
  follow the same pattern as `animus_shm_create`/`animus_shm_close`, so a
  caller can run any number of independent feeds (e.g. one per venue or
  instrument shard) in one process.
* **`L2Update` carries a venue-relative depth index (`level`), not a raw
  price** -- consumers reconstruct book state by keyed
  `(instrument_id, side, level)` replacement, matching how incremental L2
  feeds (ITCH, ArcaBook, ...) actually publish updates, rather than by
  summing deltas. Both structs carry the venue's own `sequence_number`
  (for gap detection, left to the consumer) and both a locally-stamped
  `timestamp_cycles` and a caller-supplied `exchange_timestamp_ns`, so
  feed-to-ingestion latency can be measured directly.

```python
from animus import MarketDataFeed, BookSide, BookUpdateAction, TradeAggressor

feed = MarketDataFeed.create(l2_capacity=1 << 16, trade_capacity=1 << 16)

# Producer thread(s) -- e.g. one per venue connection:
feed.push_l2_update(instrument_id=7, side=BookSide.ASK, action=BookUpdateAction.NEW,
                     level=0, price_ticks=101250, quantity=300,
                     sequence_number=1, exchange_timestamp_ns=venue_ts_ns)
feed.push_trade(instrument_id=7, trade_id=42, aggressor_side=TradeAggressor.BUYER,
                 price_ticks=101250, quantity=50,
                 sequence_number=2, exchange_timestamp_ns=venue_ts_ns)

# Consumer thread(s) -- e.g. book builder + strategy, independently:
for update in feed.poll_l2_updates(max_count=1024):
    ...
for trade in feed.poll_trades(max_count=1024):
    ...
```

Verified, not assumed: struct layouts (56 bytes each) confirmed via a real
`sizeof()`/`offsetof()` build before being mirrored in
`animus/bindings.py`'s ctypes `Structure`s; a real multi-threaded stress
test with 6-8 concurrent producer threads and 3-4 concurrent consumer
threads pushing/draining tens of thousands of records across both rings
at once, confirming zero loss, zero duplication, and zero field
corruption under genuine OS-thread concurrency -- not merely asserted from
the ring algorithm's known correctness. See `tests/test_bindings.py`'s
`MarketDataFeedIntegrationTests` for the full suite.

## Phase 19: Automated Institutional Benchmark Suite

`AnimusCore_v1/animus_benchmark_suite.cpp` measures three things this
repo hadn't measured together before, orchestrated and rendered to
Markdown by `benchmarks/generate_benchmark_report.py`:

* **Tick-to-trade end-to-end latency** -- a single-threaded, sequential
  `MarketDataFeed` push -> poll -> `ExecutionClient::submit()` round trip,
  500,000 ticks.
* **Lock-free ring buffer throughput under real 8-thread concurrency** --
  `animus::LockFreeRingBuffer<TelemetryPayload>` (the same ring
  `EngineImpl`'s telemetry ring uses) driven by 8 concurrent producer
  threads, with a post-hoc drain-count correctness check every run.
* **CPU cache locality** -- a Sattolo-shuffled pointer-chase sweep across
  16 working-set sizes (4 KB-128 MB), plus a false-sharing A/B test tied
  directly to this codebase's own `alignas(64)` design choice in
  `LockFreeRingBuffer`/`SpscRingBuffer`.

Deliberately native C++, not Python/ctypes: the GIL would serialize
Python "threads" onto one core (faking the 8-thread concurrency test),
and the ~1,300 ns/call ctypes marshalling tax this repo already measured
(Phase 16) is on its own wider than the sub-microsecond budget this suite
reports on.

```bash
python benchmarks/generate_benchmark_report.py
```

Compiles the C++ binary (g++/clang++) if missing or stale, runs it, and
writes `benchmarks/BENCHMARK_REPORT.md` -- a fully reproducible report,
not a hand-typed one, with its own Methodology & Limitations section.

Measured across 5 consecutive runs: tick-to-trade latency lands at
p50/p99 = 100 ns and p99.9 = 100-200 ns -- genuinely sub-microsecond,
not a rounding artifact. Ring buffer throughput sustained 6.9-9.6M
pushes/sec under real 8-thread contention with zero lost or duplicated
pushes every run. Cache-line padding (`alignas(64)`) measured a
consistent >4x throughput improvement over false sharing (4.21x-4.99x
across runs), directly validating an existing design choice with data.
A real methodology bug was caught and fixed while building this suite --
an early two-thread tick-to-trade design measured *milliseconds*, not
nanoseconds, due to an unpaced-producer backlog effect this repo had
already documented once before (Phase 16) -- see
`AnimusCore_v1/BENCHMARKS.md`'s Phase 19 section for the full breakdown.

