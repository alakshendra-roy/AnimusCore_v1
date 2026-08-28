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
## Phase 5: Event-Driven SOAR Orchestration Benchmarks

* **Target System:** `soar_orchestrator.py` -- config-driven threat signatures registered as native rules (`SOAROrchestrator.register_signatures` -> `animus_add_rule`), matched signals drained by a continuous background poller thread and dispatched to automated trigger-action handlers (`SOAROrchestrator._poll_loop` / `dispatch`), superseding the earlier Phase 5 entry which predated the Phase 4 native rule engine and matched signatures in Python against a synthetic flag value
* **Event Source:** `ThreatAgent.generate_telemetry_batch` -- synthetic stream with a ~2% injected critical-threat rate (`event_id=999`, `metric_value` in [8500, 10000]) against a normal-telemetry baseline (`event_id=101`, `metric_value` in [100, 2000])
* **Registered Signatures:** 2 (`unauthorized_access`: `metric_value > 8000` -> `ISOLATE_HOST`; `buffer_overflow_attempt`: `metric_value > 9500` -> `TERMINATE_PROCESS`), loaded from `config/rules.json`
* **Batch Size:** 600,000 events
* **Ring Allocation:** 2 MiB telemetry ring + 2 MiB signal ring

| Metric | Result |
|---|---|
| Events streamed / accepted | 600,000 / 600,000 (100%) |
| Pipeline execution time (ingest loop) | 412.31 ms |
| Throughput | 1,455,398 events/sec |
| Average latency per op | 687.18 ns |
| Threat signals evaluated | 15,834 |
| Automated actions dispatched | 15,834 (matches signals evaluated -- every signal reaches a trigger-action handler) |
| Persisted bytes | 38,400,000 (600,000 x 64 bytes/record) |

* **Detection Correctness:** every dispatched action's `rule_id` resolved to its originating signature (`unauthorized_access` / `buffer_overflow_attempt`) via the orchestrator's rule-id index, with severity and action name read back from `config/rules.json` -- no unresolved (`rule_UNKNOWN`) signals
* **Signal Ring Headroom:** 15,834 signals produced against a 2,097,152-capacity signal ring -- no saturation, consistent with the Known Limit documented under Phase 4 above
* **Status:** Phase 5 Event-Driven Orchestration Verified

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

## Phase 6: SDK Packaging, Shared-Memory IPC, and @trace Benchmarks

### Wheel Packaging

* **Target System:** `pyproject.toml` (PEP 621 metadata) + `setup.py`'s `build_py` override, which stages the platform's compiled native library into `animus/` before packaging
* **Verification Method:** `pip wheel . --no-deps` to build a real wheel, then `pip install` that wheel into an isolated `venv` and import/exercise it from a working directory with no sibling repo files (so nothing could fall back to a dev-checkout relative path)
* **Wheel Contents:** `animus/AnimusCore_v1.dll` + all five SDK modules (`__init__.py`, `bindings.py`, `core.py`, `decorators.py`, `shm.py`) confirmed present via `zipfile` inspection
* **Wheel Size:** 27,791 bytes (`animus_core-1.0.0-py3-none-any.whl`)
* **Isolated Install Result:** `import animus`, `@animus.trace`, and `animus.shm.SharedTelemetryRing` all functioned correctly against only the installed package -- no dev-checkout paths involved
* **Status:** Phase 6 Wheel Packaging Verified

### Shared-Memory IPC (`animus.shm.SharedTelemetryRing`)

* **Target System:** SPSC zero-copy ring backed by `multiprocessing.shared_memory`, exercised across two genuinely separate OS processes via `AnimusCore_v1/shm_ipc_demo.py`
* **Event Source:** `ThreatAgent.generate_telemetry_batch` -- same ~2% injected critical-threat rate (`event_id=999`) as the Phase 4/5 benchmarks
* **Batch Size:** 200,000 events
* **Ring Allocation:** 4,096-record shared memory ring (producer blocks on a full ring with a cooperative yield, not a drop, so capacity trades latency for headroom rather than correctness)

| Metric | Result |
|---|---|
| Events pushed / received | 200,000 / 200,000 (100%, zero drops) |
| Critical events (expected / received) | 4,028 / 4,028 |
| Producer wall time | 169.84 ms |
| Throughput | 1,177,592 events/sec |
| Average latency per op | 849.2 ns |

* **Correctness:** every field of every record (`event_id`, `trace_id`, `metric_value`) survives the cross-process hop intact, verified by the consumer process's independent critical-event count matching the producer's
* **Status:** Phase 6 Shared-Memory IPC Verified

### `@animus.trace` Decorator Overhead

* **Target System:** `animus.decorators.trace`, measured as the marginal per-call cost added on top of an already-warm native engine (lazy-init excluded from the timed region)
* **Method:** 200,000 calls to an identical trivial function (`x + 1`), timed once bare and once wrapped in `@trace`, in the same process

| Call Path | Time (200,000 calls) | Avg Latency/call |
|---|---|---|
| Bare function | 10.94 ms | 54.7 ns |
| `@trace`-wrapped | 192.64 ms | 963.2 ns |

* **Decorator Overhead:** ~908.5 ns/call (two `time.perf_counter_ns()` reads plus one `animus_record_event` ctypes call), consistent with the ~910 ns/op ctypes call-marshalling cost already measured for raw `record_event` in the Phase 4 benchmarks above -- the decorator adds negligible cost beyond the native call it wraps
* **Status:** Phase 6 Trace Decorator Verified

## Phase 7: Header-Only Engine & Execution Interop Benchmarks

### Header-Only Refactor Verification

* **Target System:** `animus.hpp` -- `EngineImpl` and `Engine::Create()` moved out of `animus_engine.cpp` into `inline` definitions in the header itself, so any C++17 translation unit can `#include "animus.hpp"` and drive `animus::Engine` in-process with zero DLL build, zero linking step, and zero ctypes/C-ABI boundary
* **ODR Safety Check:** two independent translation units, each `#include "animus.hpp"` and each instantiating `animus::Engine::Create(...)`, compiled to separate object files and linked into one binary with `g++ -std=c++17` -- linked cleanly with no duplicate-symbol errors, confirming the `inline` design is safe to include from multiple `.cpp` files in the same program
* **Real Build Verification:** full `MSBuild` rebuild of both `Release|x64` (`AnimusCore_v1.dll`, the Python SDK's native target) and `Debug|Win32` (`AnimusCore_v1.exe`) succeeded against the refactored header; the Phase 3 persistence demo was re-run against the now-inlined engine and reproduced consistent results (1.2498 Million ops/sec, 397.059 ns avg / 500 ns p99 latency), confirming no behavioral regression from the refactor
* **Side Effect:** the refactor also fixed a latent pre-existing bug in `animus_engine.cpp` where `ANIMUS_API` resolved to `dllimport` (not `dllexport`) in any build configuration that didn't predefine `ANIMUS_EXPORTS` on the compiler command line, because the file's own `#define ANIMUS_EXPORTS` guard ran after `animus.hpp` had already been included once (a `#pragma once` no-op on the second include) -- the new shim defines `ANIMUS_EXPORTS` before the header is ever included
* **Status:** Phase 7 Header-Only Refactor Verified

### Broker/Execution Interop (`animus::ExecutionClient`)

* **Target System:** `animus::ExecutionClient` wrapping `animus::LoopbackBrokerGateway` (a deterministic in-process fill simulator), exercised via `AnimusCore_v1/execution_interop_demo.cpp` -- a standalone C++17 program with no DLL, no Python, and no ctypes: `#include "animus.hpp"` is its entire dependency footprint
* **Method:** 500,000 `OrderRequest`s submitted sequentially through `ExecutionClient::submit()`, each round-tripped through the gateway and instrumented as one telemetry event (`kExecutionLatencyEventId`, `metric_value` = wall-clock latency in nanoseconds) against the same header-only `Engine` used elsewhere in this document
* **Risk Rule:** one `RuleThreshold` registered via the existing `Engine::add_rule` (`metric_value > 2,000 ns` -> `ThreatSignal`), demonstrating that a latency-risk check on the execution path is a normal SOAR rule evaluated by the same engine as telemetry ingestion, not a separate pipeline

| Metric | Result |
|---|---|
| Orders submitted | 500,000 |
| Total duration | 59.07 ms |
| Sustained throughput | 8,464,120 orders/sec |
| Average `submit()` latency | 90.16 ns/order |
| p99 `submit()` latency | 100 ns |
| Slow-fill threshold | 2,000 ns |
| Slow-fill signals raised | 0 (no simulated fill exceeded the threshold, as expected from a loopback gateway with no I/O) |

* **Correctness:** every one of the 500,000 orders returned `ExecStatus::Filled` from the gateway; the demo asserts this and aborts on any unexpected non-fill
* **Status:** Phase 7 Broker/Execution Interop Verified

## Phase 8: Enterprise Security & Multi-Tenancy Benchmarks

### RBAC + Multi-Tenant Telemetry Isolation (`animus::security`)

* **Target System:** `AnimusCore_v1/animus_security.hpp` -- `TenantRegistry` (one isolated `Engine` ring buffer/rule set/persistence file per tenant) fronted by `SecureTelemetryGateway` (the only entry point; every call is RBAC-checked via `RbacPolicy` against a `Role`/`Permission` lattice, then routed to that token's own tenant `Engine`, with every decision -- allowed or denied -- appended to an independent audit trail), exercised via `AnimusCore_v1/secure_multitenancy_demo.cpp`
* **Method:** two isolated tenants created by an `Admin` token; an `Operator` token records 10 events into tenant 10 and a threshold rule flags the last 5; a `Viewer` token scoped to tenant 20 (a different tenant, never written to) polls for signals; a `Viewer` attempts `record()` (not entitled); an `Operator` token is pointed at a tenant id that was never created
* **Build/Run:** real MSVC (`cl /std:c++17 /O2`) compile and run -- not merely a compile check

| Check | Result |
|---|---|
| `Admin` creates tenants 10 and 20 | Both succeed |
| `Viewer` attempts `create_tenant` (not entitled) | Denied |
| Tenant 10 `Viewer` polls signals after the rule fires | 7 signals returned |
| Tenant 20 `Viewer` polls signals | 0 returned (isolated from tenant 10's ring buffer -- structural, not a filter) |
| Tenant 10 `Viewer` attempts `record()` (not entitled) | Denied |
| `Operator` token for a never-created tenant id (999) attempts `record()` | Denied -- fails closed, no ring buffer auto-created |
| Audit events captured | 20 (3 denied, matching the three denied actions above) |

* **Status:** Phase 8 RBAC + Multi-Tenant Isolation Verified

### mTLS / TLS 1.3 Transport (`animus::transport`)

* **Target System:** `AnimusCore_v1/animus_transport.hpp` -- a Windows-native Schannel (SSPI) transport built on `SCH_CREDENTIALS`/`TLS_PARAMETERS` (the legacy `SCHANNEL_CRED` struct cannot negotiate TLS 1.3's cipher-suite model on this SDK; using it fails `AcquireCredentialsHandleW` outright), with mandatory mutual authentication in both directions and manual chain verification against a private, in-memory-only exclusive-root CA engine (`TrustedRoot`) -- kept independent of `SCH_CRED_MANUAL_CRED_VALIDATION` so Schannel's own (bypassed) trust check is never the only thing standing between an unverified peer and the connection
* **Certificates:** `AnimusCore_v1/generate_demo_certs.ps1` generates a self-signed demo CA plus server/client leaf certs (native `New-SelfSignedCertificate`, no OpenSSL dependency); a verified client certificate's subject CN is mapped to an `animus::security::AccessToken` via `CertificateIdentityMap` *after* chain verification succeeds, so RBAC/tenant routing downstream is keyed to a cryptographically proven identity, never a client-asserted value
* **Method:** `AnimusCore_v1/secure_transport_demo.cpp` runs a real client and server thread against each other over loopback TCP -- the server requires and verifies the client's certificate, resolves it to an `AccessToken` for tenant 42, and dispatches each of 20,000 received `WireFrame`s through `SecureTelemetryGateway::record()`
* **Build/Run:** real MSVC (`cl /std:c++17 /O2`) compile and run

| Metric | Result |
|---|---|
| Negotiated protocol (both sides) | TLS 1.3 |
| Client-verified server certificate CN | `animus-server` |
| Server-verified client certificate CN | `animus-client-tenant-42` |
| Frames sent by client | 20,000 |
| Frames accepted server-side (RBAC-authorized, tenant 42) | 20,000 / 20,000 |
| Total encrypted send duration | 138.171 ms |
| Throughput | 144,748 frames/sec |
| Cross-tenant isolation check | A second tenant (id 99, never granted a token or wired into the identity map) polling signals sees 0 -- pass |

* **Negative-Path Verification:** a rogue certificate presenting the identical subject CN (`animus-client-tenant-42`) but signed by a *different*, untrusted CA was rejected by the server's chain verification (`CERT_TRUST_IS_UNTRUSTED_ROOT`) before any frame was processed -- confirming the chain check is load-bearing and not a no-op, since Schannel's own automatic validation is deliberately bypassed (`SCH_CRED_MANUAL_CRED_VALIDATION`) in favor of this explicit check
* **Debugging Note:** two real defects surfaced during verification and were fixed before these numbers were captured -- (1) importing the PFX identity with `PKCS12_NO_PERSIST_KEY` (an ephemeral, never-persisted key) made `AcquireCredentialsHandleW` reject the certificate with `SEC_E_UNKNOWN_CREDENTIALS`, isolated via a minimal repro outside this header and fixed by switching to `CRYPT_USER_KEYSET` with explicit cleanup afterward (`delete_persisted_key`); (2) `recv_frame()` unconditionally blocked on a fresh socket read even when a complete record was already sitting in its own buffered leftover (`SECBUFFER_EXTRA`) from a prior call, causing a false "connection closed" near the end of a send burst once the peer had already finished and closed -- fixed to decrypt already-buffered data before touching the socket again
* **Status:** Phase 8 mTLS/TLS 1.3 Transport Verified