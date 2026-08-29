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

## Phase 9: Distributed Cloud Orchestration & Clustering Benchmarks

### Raft-lite Consensus over mTLS (`animus::cluster`)

* **Target System:** `AnimusCore_v1/animus_cluster.hpp` -- `RaftNode`, a randomized-timeout leader-election and log-replication implementation built directly on `animus_transport.hpp`'s Schannel `SecureChannel` rather than gRPC/Protobuf (a deliberate scoping decision, made explicitly via `AskUserQuestion` before implementation: real gRPC+Protobuf would be this project's first external build dependency, breaking the "zero external dependency" property Phase 8 preserved by choosing Schannel over OpenSSL). Inter-node RPC is a small hand-rolled binary protocol -- fixed-size `RequestVote`/`AppendEntries` structs plus a new length-prefixed message envelope (`SecureChannel::send_message`/`recv_message`, added to `animus_transport.hpp` since Phase 8's `WireFrame` framing was fixed-size telemetry only) -- reusing the same mTLS transport and cert machinery as Phase 8, not a parallel wire stack
* **What's replicated:** control-plane `AddRule` commands, not telemetry -- each node keeps ingesting/processing its own telemetry locally on the existing zero-copy hot path (`animus.hpp`); only the *rule set* that hot path evaluates against goes through consensus, so cluster membership changes cost nothing on the ingestion fast path
* **Certificates:** `generate_demo_certs.ps1` (extended for Phase 9) issues three cluster-node identities (`animus-node-1/2/3`, dual Server+Client EKU since every node dials every other node **and** accepts inbound connections from them), signed by the same demo CA as Phase 8
* **Method:** `AnimusCore_v1/cluster_demo.cpp` runs a real 3-node cluster (three `RaftNode`s, three independent `animus::Engine`s, real loopback TCP + mTLS between every pair) and checks: (1) exactly one leader is elected; (2) a rule proposed through the leader replicates to a majority, commits, and is verified *functionally* -- the same triggering telemetry event is fired at all three independently-replicated engines and each one must produce a matching `ThreatSignal`, not merely show a log entry with the right bytes; (3) a `propose()` sent to a known follower is rejected with `NotLeader` and the correct leader hint; (4) the leader is stopped (its listener and every socket closed) to simulate a crash, a new leader is elected among the two survivors, and a second rule proposed through it replicates to both
* **Build/Run:** real MSVC (`cl /std:c++17 /O2`) compile and run -- not merely a compile check; verified stable across 25 consecutive runs (100% pass) after the fixes below

| Metric | Result (representative run) | Range across 25 runs |
|---|---|---|
| Initial 3-node leader election | 466.1 ms | 361.7 ms - 705.4 ms (avg 503.2 ms) |
| Rule #1 replicated & functionally matched | node-1, node-2, node-3 all `yes` | 25/25 |
| Follower `propose()` correctly redirected | `NotLeader` + correct leader hint | 25/25 |
| Failover re-election (2 survivors) | 629.3 ms | 454.6 ms - 1,451.8 ms (avg 716.4 ms) |
| New leader distinct from the stopped one | yes | 25/25 |
| Rule #2 replicated & functionally matched (both survivors) | `yes` | 25/25 |

* **Election Safety:** exactly one leader observed per term in every run, including immediately after failover, despite the 3-node cluster routinely producing genuine "dueling candidate" rounds (two nodes timing out and canvassing for votes within milliseconds of each other) -- Raft's term-based safety resolved every one without a split-brain
* **Debugging Notes -- three real defects found and fixed during verification, not merely compiled:**
  1. **`stop()` didn't close inbound connections.** Simulating a node failure via `stop()` only closed the *listener* socket; a handler thread already blocked in `recv_message()` on an already-accepted peer connection had no way to learn the node was shutting down, and `stop()`'s own thread-join would hang forever. Fixed by tracking every inbound `SecureChannel` in a list `stop()` force-closes before joining.
  2. **`SEC_I_RENEGOTIATE` deadlock.** Phase 8's demo never called `recv_frame` from its TLS *client* side, so it never observed a TLS 1.3 server's post-handshake `NewSessionTicket` message surfacing as `SEC_I_RENEGOTIATE` from `DecryptMessage`. Every Phase 9 cluster connection is bidirectional (the dialing side both sends a request and blocks reading the response), so it hit this on nearly every first RPC; a naive fix (skip the record) corrupted the Schannel context (`SEC_E_CONTEXT_EXPIRED` on the next call), and a "correct" fix that reprocessed the token via `{Initialize,Accept}SecurityContext` produced a genuine two-node deadlock instead. Fixed at the root by setting `SCH_CRED_DISABLE_RECONNECTS` on both credentials so the ticket is never issued, with a fail-fast (not hanging) error path kept as a backstop.
  3. **A dead peer could stall an entire election round.** Windows does not always RST a connection attempt to a socket that *just* stopped listening -- occasionally the connect can take several seconds to time out via the OS default, and since every election round includes a worker thread trying the (dead) third node, this could drag every round out by seconds. Fixed with a real bounded connect timeout (non-blocking `connect()` + `select()`, `animus_transport.hpp`'s `TcpSocket::connect_to`), used with a 200 ms bound for cluster RPC.
  4. **A genuine Raft correctness gap, not a demo bug -- reproduced live in a real 3-node failover (~20% of runs before the fix):** a new leader cannot advance its commit index by counting replicas of an entry from a *previous* term (Raft §5.4.2) -- so if leadership changes a second time shortly after an entry was already majority-replicated but before anyone re-confirms it under the new leader's own term, that entry can be stuck uncommitted forever if nothing new is ever proposed afterward. Fixed with the standard mitigation: every new leader immediately appends and replicates a no-op entry in its own term (`LogEntry::is_noop`, skipped by the state machine), which carries any earlier pending entry forward the moment it commits.
* **Status:** Phase 9 Raft-lite Cluster over mTLS Verified

## Phase 10: Final Commercial Packaging & Single-Header Release Benchmarks

### Single-Header Amalgamation (`AnimusCore_v1/animus_release.hpp`)

* **Target System:** `AnimusCore_v1/amalgamate.py` -- a small generator script (not a hand-maintained artifact) that concatenates the four source headers (`animus.hpp`, `animus_security.hpp`, `animus_transport.hpp`, `animus_cluster.hpp`) into one file, in dependency order, stripping each source's own `#pragma once` and local `#include "animus...hpp"` lines so the result is genuinely self-contained -- a client vendors one file, not four with an inter-file include order to get right
* **Method:** regenerate (`python amalgamate.py`) and real-compile the output with two different compilers/configurations, not just inspect it -- `AnimusCore_v1/release_header_smoke_test.cpp` exercises all four namespaces (`animus::`, `animus::security::`, `animus::transport::`, `animus::cluster::`) through one `#include "animus_release.hpp"`

| Check | Result |
|---|---|
| Generated file size | 2,549 lines, single `#pragma once` |
| MSVC (`cl /std:c++17 /EHsc /O2`) full build | compiles + links; smoke test runs all 4 layers OK |
| MinGW `g++ -std=c++17 -O2 -pthread` build | compiles + runs the portable core + RBAC layers OK |

* **Debugging Notes -- two real defects found and fixed while building the generator, not merely written and assumed correct:**
  1. **Un-stripped `#include "animus.hpp"` caused a real redefinition, not a harmless no-op.** The initial include-stripping regex only matched `animus_*.hpp` (the pattern used by the three dependent headers), missing that `animus_security.hpp` itself contains a literal `#include "animus.hpp"`. Left in, the amalgamation re-opened the real `animus.hpp` from disk for a second, separate parse -- `#pragma once` doesn't help here, since the first occurrence of `animus.hpp`'s content was already inlined directly (not `#include`-d), so the second, un-stripped `#include` was the *only* `#include` of that path in the whole translation unit, and the compiler dutifully redefined every type in it (`error C2011: 'animus::TelemetryPayload': 'struct' type redefinition`, plus five more, verified by a real MSVC compile before the fix). Fixed by broadening the strip pattern to match `animus*.hpp` generally, not just the `animus_*` subset.
  2. **A bare `#if defined(_WIN32)` guard was not actually sufficient for "Windows," only for "Windows + MSVC."** `animus_transport.hpp`'s certificate loading (`load_pfx_certificate`/`load_cer_certificate`) uses an MSVC-specific `std::ifstream(std::wstring, ...)` constructor overload that MinGW/libstdc++ does not provide -- a real `g++ -std=c++17` build of the amalgamation under MinGW on Windows (which does define `_WIN32`) failed to compile with six pages of cascading `deque`/iterator errors stemming from that one call. Fixed by tightening the generator's guard on the transport/cluster sections to `defined(_WIN32) && defined(_MSC_VER)`, verified by a clean `g++` build afterward that correctly excludes those two sections and still compiles + runs the portable core + RBAC layers.
* **A related false lead, recorded for anyone hitting the same symptom:** an early `g++`-built smoke test that *did* compile (before the `_MSC_VER` fix was needed for a different reason) segfaulted inside `std::basic_ofstream`'s constructor on this machine. `gdb` traced it to the running process loading `libstdc++-6.dll` from a different, ABI-incompatible MinGW distribution (Git for Windows' bundled `mingw64\bin`) ahead of the matching MSYS2 UCRT64 runtime the binary was actually linked against -- confirmed by statically linking the C++ runtime (`-static -static-libgcc -static-libstdc++`), which ran cleanly. Not a defect in `animus.hpp`; a local PATH/toolchain-shadowing artifact, included here only because it looked identical to a real crash until isolated.
* **Status:** Phase 10 Single-Header Amalgamation Verified

### Python SDK PyPI Packaging

* **Target System:** `pyproject.toml` / `setup.py` / `MANIFEST.in` -- added an MIT `LICENSE` file, PyPI classifiers (`License :: OSI Approved :: MIT License`, `Development Status :: 4 - Beta`, audience/topic tags), keywords, and `Documentation`/`Issues` project URLs; `MANIFEST.in` extended to bundle `LICENSE`, `QUICKSTART.md`, and `animus_release.hpp` into the sdist
* **Method:** a real `pip wheel . --no-deps` build (not a metadata-only check), followed by installing the resulting wheel into a **fresh, isolated venv** (no sibling source checkout present) and importing it -- the same bar Phase 6's wheel verification used

| Check | Result |
|---|---|
| `pip wheel . --no-deps` | `Successfully built animus-core`; `animus_core-1.0.0-py3-none-any.whl` |
| `LICENSE` auto-included in wheel metadata | yes -- `animus_core-1.0.0.dist-info/licenses/LICENSE` (setuptools auto-detected it, no extra config needed) |
| Native `AnimusCore_v1.dll` bundled in wheel | yes -- self-contained, no sibling checkout required at install time |
| Install into a fresh isolated venv | succeeds |
| `import animus` from that venv | succeeds; `animus.__version__ == '1.0.0'`, all public symbols present |

* **Status:** Phase 10 PyPI Packaging Verified

### Multi-Node Write Latency & Throughput (`AnimusCore_v1/cluster_latency_bench.cpp`)

* **Target System:** a real 3-node Raft-lite cluster (same topology as the Phase 9 demo), timing `RaftNode::propose()` itself rather than election/failover -- the number a client of this system actually experiences on every write, not a one-time startup cost
* **What's measured, and why two numbers, not one:** `propose()` blocks until an entry is committed to a **majority** (leader + 1 follower in a 3-node cluster) -- that's the real "is my write durable" latency a caller waits on. Separately, "extra time to full-cluster commit" measures the *additional* wall time (after `propose()` already returned) until the **slowest** follower -- not just the majority -- has also applied the entry; this is 0 whenever the lagging follower happened to be the one that already contributed to the majority, and driven by the next heartbeat cycle (`kTickMs` = 30 ms) otherwise. Conflating these into one number would misrepresent both.
* **Method:** two separate timed passes so the benchmark's own measurement doesn't become the bottleneck it reports -- Pass 1 fires 300 `propose()` calls back-to-back with no extra wait, giving a real sustained-throughput number; Pass 2 (100 further proposals) waits for full-cluster convergence after each one, kept out of the throughput measurement so it isn't self-throttled by its own polling. (An earlier single-pass version conflated the two and reported ~27 proposals/sec as "throughput," which was actually measuring Pass 2's convergence wait, not the system's real capacity -- caught before these numbers were recorded, not after.) Real MSVC (`cl /std:c++17 /O2`) build and run, 5 consecutive full runs.

| Metric | Representative run | Range across 5 runs |
|---|---|---|
| `propose()` call latency (majority commit) -- p50 | 0.131 ms | 0.120 ms - 0.377 ms |
| `propose()` call latency (majority commit) -- p99 | 0.368 ms | 0.236 ms - 5.032 ms |
| `propose()` call latency (majority commit) -- max | 1.181 ms | 0.299 ms - 13.904 ms |
| Sustained proposal throughput (Pass 1, back-to-back) | 6,648.9 proposals/sec | 1,656.3 - 7,798.7 proposals/sec |
| Extra time to full-cluster (not just majority) commit -- p50 | 30.9 ms | 30.9 ms - 31.5 ms |
| Extra time to full-cluster commit -- p99 | 46.8 ms | 46.8 ms - 61.1 ms |
| Proposals committed | 300/300 (Pass 1) + 100/100 (Pass 2) | 5/5 runs, zero failures |

* **Interpretation:** the full-cluster convergence numbers cluster tightly around 30 ms -- almost exactly `RaftNode`'s `kTickMs` heartbeat interval -- which is the expected signature of a follower that didn't win the majority race catching up on the *next* heartbeat rather than the original `AppendEntries` call; this is a real, explainable characteristic of the current heartbeat-driven replication design, not measurement noise, and would be the first thing to tighten (e.g. an immediate follow-up AppendEntries to the lagging follower rather than waiting for the next tick) if full-cluster (not just majority) convergence latency mattered for a given deployment.
* **Status:** Phase 10 Multi-Node Latency Verified

### Client Quickstart Guides

* **Target System:** `AnimusCore_v1/QUICKSTART.md` -- four PoC guides (Python SDK, C++ single header, secure multi-tenant + mTLS, distributed cluster), each layering on the previous
* **Method:** every code sample was either compiled and run for real as part of this phase's verification (guides 1 and 2 -- see the Python wheel and single-header checks above) or checked against the actual current method signatures in the source headers (guides 3 and 4 -- `CertificateIdentityMap::add`/`resolve`, `RaftNode`'s constructor and `propose()` signature), not written from memory of an earlier phase and left unverified
* **Status:** Phase 10 Client Quickstart Guides Verified

## Phase 11: Batched Event Ingestion (`animus_record_events_batch`)

### Diagnosis: isolating the real cost of driving the engine from Python

* **Target System:** `benchmarks/benchmark_engine.py`, comparing the native engine against an equivalent pure-Python `dict`-loop implementation across four scenarios instead of reporting one headline number: (1) the full native `record_event()` -> evaluate -> persist pipeline, (2) a pure-Python loop doing the same logical work in memory (dict-keyed rule lookup, no ctypes, no disk I/O), (3) native ring-buffer ingestion alone (no persistence worker running, so no rule evaluation and no disk I/O), (4) native batched ingestion via the new `animus_record_events_batch`.
* **Method:** 100,000 events per scenario per run, `time.perf_counter()` spanning each scenario's full call loop (native scenarios' internal timing follows the same convention already established for Phase 4's `ingest_engine.py` harness -- Python-side wall clock, not a native-side counter, so ctypes marshalling cost is included, not excluded). 5 consecutive runs against `build/Release/AnimusNative.dll` (the CMake build), plus a same-session cross-check against the MSVC `x64/Release/AnimusCore_v1.dll` build with the CMake binary temporarily removed so `find_native_library()` was forced onto the legacy binary.

| Scenario | Representative run | Range across 5 runs (`AnimusNative.dll`) |
|---|---|---|
| Native, full pipeline (ingest + evaluate + persist) | 115.39 ms (866,651 ev/s) | 109.11 - 125.27 ms |
| Pure-Python dict loop | 32.19 ms (3,106,748 ev/s) | 32.05 - 33.69 ms |
| Native, ring-buffer ingestion only (no eval, no disk I/O) | 113.46 ms (881,395 ev/s) | 107.71 - 113.46 ms |
| Native speedup over pure-Python, full pipeline | 0.28x | 0.26x - 0.31x |
| Native speedup over pure-Python, ring-buffer only | 0.28x | 0.28x - 0.31x |

* **Finding:** the native engine lost to a pure-Python in-memory loop by roughly 3.5x at this event count, and removing the disk I/O entirely (scenario 3) barely moved the needle (~113 ms vs. ~115 ms) -- ruling out batched-disk-flush cost as the dominant factor, contrary to the initial hypothesis when this comparison was first built.
* **Root-cause isolation:** a follow-up ad hoc measurement split `record_event()`'s ~1.1 us/call cost into its two components -- the native C-ABI call itself, and the ctypes call-marshalling Python performs to make it. Timed separately: 100,000 individual `animus_record_event` ctypes calls (Python-side loop) vs. one `animus_record_events_batch` call carrying all 100,000 events. The native side processed the full 100,000-event batch in ~2.5 ms; the remaining ~110 ms was Python-side ctypes call overhead paid once per event, not native execution time.

### Fix, and a second bottleneck found while fixing it

* **`animus_record_events_batch`** (`AnimusCore_v1/animus.hpp`'s `Engine::record_batch` / `EngineImpl::record_batch`, shimmed in `animus_engine.cpp`) pushes a whole batch of events onto the ring buffer in one C-ABI call, stopping at the first push that fails (ring full) so its return value tells the caller exactly how many of the batch, in order, were ingested -- same never-blocks contract as `record()`, extended to a batch. Exposed from Python as `AnimusBindings.record_events_batch()` (`animus/bindings.py`), with a matching method on `_PurePythonEngine` for API parity when no native binary is present.
* **First implementation, and why it only closed ~15% of the gap:** building the `(NativeEvent * N)` ctypes array by constructing one `NativeEvent(event_id, trace_id, metric_value)` Python object per event (then unpacking into the array) reduced that run's batched-ingestion time from 108.85 ms (ring-buffer-only, one call per event) to 92.70 ms -- Python object-construction overhead, not the FFI call boundary, was still the dominant cost, since eliminating 99,999 of the 100,000 ctypes *calls* only removed a small fraction of the total time.
* **Second fix:** replaced per-event `ctypes.Structure` construction with `struct.pack("<IIQ", event_id, trace_id, metric_value)` per event, joined into one `bytes` object, then a single `ctypes.memmove()` into the array's backing memory. Isolated measurement at 100,000 events: constructing the array via `ctypes.Structure` objects took ~82 ms; via `struct.pack` + `memmove`, ~14 ms -- roughly 6x faster to build, for byte-identical output (`struct.calcsize("<IIQ") == ctypes.sizeof(NativeEvent)`, asserted at import time in `bindings.py`).

### Result

| Scenario | Representative run | Range across 5 runs (`AnimusNative.dll`) |
|---|---|---|
| Native, batched ingestion (`record_events_batch`) | 16.03 ms (6,238,420 ev/s) | 14.13 - 17.49 ms |
| Native speedup over pure-Python, batched ingestion | 2.01x | 1.83x - 2.30x |
| Native speedup over native ring-buffer-only ingestion | 7.08x | 6.30x - 7.62x |

* **Build parity:** re-ran the full benchmark against the MSVC-built `x64/Release/AnimusCore_v1.dll` (with the CMake-built `AnimusNative.dll` temporarily hidden so `find_native_library()` fell back to it) -- batched ingestion measured 15.88 ms (2.10x pure-Python), inside the range measured against `AnimusNative.dll`, confirming the two build outputs behave identically now that both compile the same `animus.hpp` / `animus_engine.cpp` source carrying the new export.
* **Regression check:** the full `tests/test_bindings.py` suite (23 tests, including new coverage for `record_events_batch`'s native marshalling, empty-batch no-op, and bounded-capacity truncation behavior, plus the pure-Python fallback) passed against both binaries with no changes to existing test expectations.
* **Interpretation:** the native engine's own ingestion work is not the bottleneck at any point in this investigation -- 100,000 events costs it ~2.5 ms either way. What determines whether calling it from Python is faster or slower than staying in pure Python is entirely how the caller crosses the language boundary: once per event (loses to pure Python), or once per batch with the argument buffer built as raw bytes rather than individual wrapper objects (wins by ~2x).
* **Status:** Phase 11 Batched Event Ingestion Verified

## Phase 12: Stress Test -- Sustained Load, Memory Leaks, and C-ABI Boundary Safety

### Sustained-Load Memory Check (1,200,000 events)

* **Target System:** `benchmarks/stress_test_engine.py` -- the full real pipeline (`init` -> `add_rule` -> `start_logging` -> `record_events_batch` in 50,000-event batches -> `stop_logging`), not a synthetic allocation-only loop, with this process's resident memory (RSS) sampled between every batch
* **Method:** RSS read via a direct Windows API call (`psapi.GetProcessMemoryInfo` through ctypes, no `psutil` dependency -- consistent with this SDK's zero-dependency stdlib-only design), with a Linux (`/proc/self/status` VmRSS) and macOS/generic (`resource.getrusage`) fallback for portability. 5 consecutive full runs.
* **Baseline methodology, and a false positive caught while building it:** the first version of this script flagged a leak (+24.85% RSS growth) by measuring growth from RSS immediately after `init()`, before any event had been pushed. That's the wrong reference point: the ring buffers are *reserved* at init but their pages are only *committed* (and so counted in RSS) as each cell is first written to, so RSS legitimately climbs through the first cycle or two of a ring this large even with zero leaks -- it isn't a leak signature, it's one-time OS lazy-page-commit. Fixed by using a "warm" baseline instead: the RSS sample taken after the ring has been fully cycled twice (600,000+ events into a 131,072-capacity ring), by which point every cell's backing page is provably resident.

| Metric | Representative run | Range across 5 runs |
|---|---|---|
| Cold baseline RSS (before `init()`) | 25.23 MB | 24.87 - 25.23 MB |
| Post-init RSS (before any event pushed) | 47.07 MB | 46.73 - 47.11 MB |
| Warm baseline RSS (after 300,000 events -- the correct leak-detection reference point) | 57.84 MB | 57.84 - 58.03 MB |
| Final RSS (after `stop_logging()` + `gc.collect()`, all 1,200,000 events processed) | 58.89 MB | 58.89 - 59.47 MB |
| RSS growth, warm baseline -> final | +1.05 MB | +1.05 - +1.45 MB |
| RSS growth, warm baseline -> final (%) | +1.82% | +1.82% - +2.49% |
| Threat signals matched and drained | 108,000 / 108,000 expected | 108,000 / 108,000 in all 5 runs |

* **Leak verdict:** no run exceeded the script's 10% growth-flag threshold; growth stayed in a narrow 1.82-2.49% band across all 5 runs, consistent with normal allocator/heap fragmentation noise rather than a leak. `EngineImpl`'s own allocation profile supports this reading directly from the source: the ring buffers are sized once at construction (`LockFreeRingBuffer`'s backing `std::vector<Cell>`), the persistence worker's batch buffer is `reserve()`-d once and `clear()`-ed (not reallocated) every drain cycle, and every event/signal is stored by value with no per-event heap allocation on the native side.
* **Correctness under sustained load:** every run drained exactly 108,000 threat signals -- 1,200,000 events x 9% match rate (`metric_value = i % 100`, rule threshold `> 90` matching values 91-99) -- with zero drops or duplicates, across all 5 runs.
* **A real, unrelated bug found and fixed while writing this check:** the first `get_rss_bytes()` implementation raised `OSError: [WinError 6] The handle is invalid`. Root cause: `ctypes.windll.kernel32.GetCurrentProcess()` was called with no explicit `argtypes`/`restype`, so ctypes' default (`c_int`, 32-bit) truncated the function's 64-bit pseudo-handle (`0xFFFFFFFFFFFFFFFF`) and then zero-extended it back out incorrectly when passed into `GetProcessMemoryInfo`, producing a handle value Windows rejected. Fixed by explicitly declaring both functions' `argtypes`/`restype` with `ctypes.wintypes.HANDLE`, verified with a standalone reproduction before folding the fix into the script.
* **Status:** Phase 12 Sustained-Load Memory Check Verified

### C-ABI Boundary Safety: Malformed Input Against `animus_record_events_batch`

* **Target System:** two different threat models against the same function, deliberately handled two different ways -- malformed *event data* through the public `AnimusBindings.record_events_batch()` API, and malformed *pointer/count* arguments against the raw C-ABI export it wraps
* **Method, malformed data (in-process):** 7 cases -- an out-of-range `metric_value` (2^64), an out-of-range `event_id` (2^32), negative `event_id`/`metric_value`, a short (2-field) and a long (4-field) event tuple, and a non-integer field -- fed through the public SDK call. `struct.pack()` validates every field's type and range before any ctypes/native call happens, so none of these can reach native code, let alone corrupt memory; safe to run in-process.
* **Method, malformed pointer/count (subprocess-isolated):** 6 cases against the raw `_lib.animus_record_events_batch` handle (bypassing the public SDK method the way a caller never should, but a boundary fuzzer must) -- a NULL buffer with a nonzero count, a call before `animus_init` (no engine yet), a `count` claiming 100,000 events against a buffer that actually holds 1, `count = 2**64-1` against the same 1-event buffer, `count` passed as a negative Python int, and an empty batch through the SDK as a sanity control. `animus_record_events_batch` has no way to validate a caller-supplied `count` against the buffer's real size -- the same trust contract as `memcpy` -- so a lying `count` is a genuine out-of-bounds read that can crash the process reading it; each case therefore runs in its own subprocess (`python stress_test_engine.py --fuzz-case <name>`) so a real crash is *observed and reported*, not something that also kills the test suite running it. 5 consecutive full runs.

| Malformed-data case (in-process) | Outcome (5/5 runs) |
|---|---|
| `metric_value = 2**64` | `struct.error`, rejected |
| `event_id = 2**32` | `struct.error`, rejected |
| `event_id = -1` | `struct.error`, rejected |
| `metric_value = -1` | `struct.error`, rejected |
| 2-field event tuple | `ValueError`, rejected |
| 4-field event tuple | `ValueError`, rejected |
| Non-integer field (`"bad"`) | `struct.error`, rejected |

| Malformed pointer/count case (subprocess-isolated) | Outcome | Crash rate across 5 runs |
|---|---|---|
| NULL buffer, count=5 | Safely returns 0 (`if (!events) return 0;` guard) | 0/5 crashed |
| Called before `animus_init` | Safely returns 0 (`if (!engine) return 0;` guard) | 0/5 crashed |
| `count` claims 100,000; buffer holds 1 | Out-of-bounds read; crashes depending on heap layout | 2/5 crashed (`STATUS_ACCESS_VIOLATION`); 3/5 read ~1.6 MB of adjacent memory as garbage events without crashing -- not safe either way, just didn't hit an unmapped page those 3 runs |
| `count = 2**64-1`; buffer holds 1 | Out-of-bounds read, unbounded | 5/5 crashed (`STATUS_ACCESS_VIOLATION`, `0xC0000005`) |
| `count` passed as `-1` (Python int) | `ctypes.c_size_t` silently wraps -1 to `0xFFFFFFFFFFFFFFFF` -- **not** rejected with `OverflowError` as might be assumed | 0/5 crashed (small, 1,024-capacity ring bounds the OOB read to ~16 KB, which stayed inside mapped memory in every run observed -- happenstance, not a guarantee, same caveat as the moderate case above) |
| Empty batch via the public SDK | Safely returns 0, no native call made | 0/5 crashed (control case) |

* **Key finding -- non-determinism, not a guarantee:** whether a lying `count` crashes the process is a function of how far past the true buffer the read walks before hitting an unmapped page, which depends on heap layout at call time, not on anything about the input. The moderate case (100,000 claimed vs. 1 real) crashed in 2 of 5 runs and silently read garbage adjacent memory into the ring buffer in the other 3 -- neither outcome is "safe," and the script's reporting was corrected mid-development to say so explicitly (`NO CRASH*` is a distinct, footnoted outcome from `SAFE`, not treated as equivalent) rather than let a lucky non-crashing run read as "handled correctly."
* **A genuinely surprising sub-finding:** `ctypes.c_size_t` does not raise `OverflowError` for a negative Python int the way the `struct.pack`-validated fields above do -- it silently reinterprets `-1` as the maximum unsigned 64-bit value and passes that straight through. This was verified empirically (not assumed) before being relied on in the script's fuzz case, after an earlier assumption in this investigation about ctypes' negative-value handling turned out to be wrong for this specific argument-conversion path.
* **Why this is not a defect in the public SDK:** `AnimusBindings.record_events_batch()` (`animus/bindings.py`) always derives `count` from `len(events)` and sizes its `(NativeEvent * count)` buffer to exactly that count in the same call -- the two can never disagree through the public API. Reaching any of the crashing/unsafe cases above requires bypassing the SDK and calling the raw `_lib` handle directly, exactly as this fuzz harness does and exactly as no ordinary caller would. The raw C-ABI's behavior here is the same caller-must-not-lie-about-length contract every pointer+length C API has (`memcpy`, `read()`, ...), not something unique to this engine -- worth documenting precisely, not worth "fixing" by adding a length field to the wire struct at the cost of the zero-copy design this API exists for.
* **Status:** Phase 12 C-ABI Boundary Safety Verified (public SDK path: safe by construction; raw C-ABI path: behaves exactly as its documented trust contract implies, confirmed empirically rather than assumed)

## Phase 13: Fintech-Style Tail Latency -- Batched Ingestion (`animus_record_events_batch`)

* **Target System:** `benchmarks/fintech_tail_latency.py` -- `AnimusBindings.record_events_batch()` timed call-by-call with `time.perf_counter_ns()` (nanosecond resolution, reported in microseconds), framed the way a latency-sensitive caller (order ingestion, a risk check on a hot path) cares about it: not just a mean, but p50 through p99.99.
* **Method:** 1,000,000 events pushed at each of three batch sizes (100 / 1,000 / 10,000 events per call), timing only `record_events_batch()` itself -- the Python-side event-list for each call is built *before* that call's timer starts, so list construction never contaminates the measured latency. The ring buffer is sized to hold the entire 1,000,000-event sweep (`buffer_capacity=total_events`), so no call ever blocks on a full buffer or needs draining -- every call in a sweep measures the same thing. Each batch size runs in its own fresh subprocess (`--run-sweep <n>`), not sequentially in one process, specifically because tail-latency measurement is sensitive to cross-run contamination (a partially-drained ring, a warmer allocator from the previous sweep) in a way a mean is not. 5 consecutive full runs (all 3 batch sizes each).

### Batch size = 100 (10,000 calls)

| Metric | Representative run | Range across 5 runs |
|---|---|---|
| Throughput | 6,891,485 ev/s | 6,615,997 - 6,891,485 ev/s |
| Mean | 14.51 us | 14.51 - 15.11 us |
| p50 | 13.70 us | 13.70 - 14.40 us |
| p90 | 14.60 us | 14.60 - 15.60 us |
| p99 | 27.60 us | 24.80 - 28.40 us |
| p99.99 | 385.42 us | 182.66 - 590.01 us |

### Batch size = 1,000 (1,000 calls)

| Metric | Representative run | Range across 5 runs |
|---|---|---|
| Throughput | 8,723,895 ev/s | 7,985,224 - 8,806,654 ev/s |
| Mean | 114.63 us | 113.55 - 125.23 us |
| p50 | 107.50 us | 107.50 - 119.85 us |
| p90 | 122.31 us | 122.31 - 142.20 us |
| p99 | 211.97 us | 184.61 - 228.99 us |
| p99.99 | 895.49 us | 367.01 - 1,045.34 us |

### Batch size = 10,000 (100 calls)

| Metric | Representative run | Range across 5 runs |
|---|---|---|
| Throughput | 7,131,335 ev/s | 6,472,970 - 7,232,394 ev/s |
| Mean | 1,402.26 us | 1,382.67 - 1,544.89 us |
| p50 | 1,373.30 us | 1,350.15 - 1,507.15 us |
| p90 | 1,588.62 us | 1,473.25 - 1,694.62 us |
| p99 | 1,865.05 us | 1,815.56 - 2,056.52 us |
| p99.99 | 1,958.56 us | 1,958.56 - 2,586.12 us |

* **Sample-size caveat on p99.99, stated in the script's own output, not just here:** p99.99 needs on the order of 10,000+ samples to land on a real data point rather than interpolating near the max. The batch=100 sweep (10,000 calls) is reasonably resolved; batch=1,000 (1,000 calls) is thin; batch=10,000 (only 100 calls) is barely more than "the max of 100 samples" and should be read that way, not as a precise tail estimate.
* **Key finding -- the relative tail shrinks as batch size grows, even though absolute latency grows:** using the representative run, p99.99/p50 is ~28.1x at batch=100, ~8.3x at batch=1,000, and ~1.4x at batch=10,000. Smaller batches are dominated by fixed per-call jitter (OS scheduling, Python-level GC, allocator stalls) that a 100-event batch's real work barely amortizes; a 10,000-event batch's real work is large enough that the same fixed jitter becomes a much smaller fraction of the call, tightening the tail relative to the median even as every absolute number (mean, p50, p99.99) grows with batch size.
* **Throughput is essentially flat across batch sizes** (6.5-8.8M events/sec across all three, overlapping ranges) -- the native ring-buffer push itself is cheap enough (see Phase 11's ~2.5 ms measurement for 100,000 events) that batch size mostly trades off call-count against per-call tail risk, not raw throughput.
* **Status:** Phase 13 Fintech Tail Latency Verified

## Phase 14: Lock-Free SPSC Ring Buffer & CPU Core Pinning

### `animus::SpscRingBuffer<T>` and the Standalone SPSC Channel

* **Target System:** `animus::SpscRingBuffer<T>` (`animus.hpp`) -- a single-producer/single-consumer ring using a plain atomic load/store pair (no compare-exchange retry loop, unlike the existing MPMC `LockFreeRingBuffer`), backed by one contiguous pre-allocated `std::vector<T>`. Exposed as a standalone C-ABI channel (`animus_spsc_init` / `animus_spsc_record_events_batch` / `animus_spsc_drain`) fully independent of the existing `Engine` singleton and its MPMC ring -- additive, not a replacement.
* **Method:** unit tests against both a fake native lib (`tests/test_bindings.py`'s `_FakeNativeLib`) and the real compiled binary (`RealNativeEngineIntegrationTests`) -- push a batch, drain it back, assert every field round-trips intact.

| Check | Result |
|---|---|
| Push 50 events, drain 100 requested | 50 pushed, 50 drained, all fields match in order |
| Push into an under-capacity ring (4 slots, 6 events) | 4 pushed, matching `record_batch`'s stop-at-first-failure contract |
| Full test suite | 30/30 passing (7 new), against both `AnimusCore_v1.dll` (MSVC) and `AnimusNative.dll` (CMake) |

* **A real bug found and fixed during development:** the first `SpscTelemetryRecord` ctypes struct (mirroring `animus::TelemetryPayload` for `animus_spsc_drain`'s output buffer) had the four real fields but not `TelemetryPayload`'s `alignas(64)` cache-line padding -- 24 bytes instead of 64. This does not raise an error: it silently reads every record in the array at the wrong offset, since the native side's per-element stride is still 64 bytes regardless of what the Python side assumes. A manual push/drain test surfaced it immediately as garbled field values (`event_id=2461703904`, `trace_id=32763`, ...) rather than the pushed data; fixed by padding `SpscTelemetryRecord` to the full 64 bytes and asserting `ctypes.sizeof(SpscTelemetryRecord) == 64` at import time, then re-verified with a clean round trip before it went into `bindings.py`.
* **Status:** Phase 14 SPSC Ring Buffer Verified

### CPU Core Pinning (`animus_pin_current_thread_to_core`)

* **Target System:** `animus_pin_current_thread_to_core(core_id)` / `animus_get_cpu_count()` (`animus_engine.cpp`) -- Windows: `SetThreadAffinityMask`; Linux: `pthread_setaffinity_np` (requires `find_package(Threads)` + `Threads::Threads`, added to `CMakeLists.txt` for this); no portable hard-pinning API exists on other platforms, so the function returns `false` there rather than claiming a pin that didn't happen. Affects only the calling thread, not the whole process.
* **Test machine:** Intel Core i7-14650HX -- a hybrid CPU, 16 cores / 24 logical threads (6 Performance-cores with Hyper-Threading + 10 Efficiency-cores), confirmed via `Get-CimInstance Win32_Processor`.
* **A real, environment-driven finding, not a defect:** the first version of the fintech tail-latency benchmark (below) pinned to the highest-numbered logical core (`cpu_count - 1` = core 23), a common informal "avoid core 0" convention. On this hybrid CPU that silently picked an Efficiency core. Measured effect of pinning to core 23 vs. not pinning at all, same 1,000,000-event sweep:

| Batch size | Unpinned baseline p99.99 | Pinned to core 23 (an E-core) p99.99 | Ratio |
|---|---|---|---|
| 100 | 581.01 us | 2,642.91 us | 4.55x worse |
| 1,000 | 875.83 us | 30,201.11 us | 34.48x worse |
| 10,000 | 2,522.45 us | 8,529.81 us | 3.38x worse |

* **Why, and the fix:** there is no portable, vendor-neutral way to ask the OS "which logical cores are P-cores vs. E-cores" from C++ -- `animus_pin_current_thread_to_core` only pins, it has no opinion on where. Rather than hardcode a guess for one CPU vendor's numbering convention (fragile, and wrong on plenty of real hybrid layouts even for that vendor), the benchmark now *probes*: times a small, cheap workload pinned to each of 6 candidate cores spread across the logical range, and picks whichever measured the lowest p99 before running the real sweeps. See the next section for what that probe actually found on this machine, run after run.
* **Status:** Phase 14 CPU Core Pinning Verified (platform coverage: Windows + Linux; correctly refuses to claim success on platforms with no real pinning API)

### Fintech Tail Latency: Baseline vs. SPSC + Pinned (Probed Core)

* **Target System:** `benchmarks/fintech_tail_latency.py`, extended from Phase 13 to add a second producer path (`animus_spsc_record_events_batch` against the SPSC ring, from a thread pinned via `find_best_core()`'s probe) alongside the unchanged Phase 13 baseline (`animus_record_events_batch` against the MPMC ring, unpinned) -- same 1,000,000 events per batch size (100 / 1,000 / 10,000), same call-by-call `perf_counter_ns` timing discipline, same subprocess-per-sweep isolation.
* **Method:** 5 consecutive full runs, each independently re-probing for the best core (deliberately not cached between runs, to see whether the probe itself is reliable, not just the number it happens to land on once).

**Core probe** (representative run; p99 of a small workload per candidate core, lower is better):

| Core | p99 (representative run) | Selected how often (5 runs) |
|---|---|---|
| 0 | 60.16 us | 0/5 (range across runs: 50.32-93.43 us -- highest and noisiest of the low cores, consistent with OS/interrupt activity favoring core 0) |
| 1 | 53.55 us | 0/5 |
| 6 | 55.08 us | 4/5 |
| 12 | 47.85 us | 1/5 |
| 18 | 77.86 us | 0/5 |
| 23 | 69.55 us | 0/5 |

Every one of the 5 runs selected core 6 or 12 -- never 0, 18, or 23 -- consistent with a P-core/E-core split roughly along these lines on this CPU, though the probe never assumes that; it just measures.

**Baseline -> SPSC + pinned, by batch size** (representative run's values; delta range and improvement rate across all 5 runs):

Batch size = 100:

| Metric | Baseline | SPSC + pinned | Delta (representative) | Delta range (5 runs) | Runs improved |
|---|---|---|---|---|---|
| Throughput | 6,690,668 ev/s | 7,020,411 ev/s | +4.9% | +2.0% to +10.0% | 5/5 |
| p50 | 14.50 us | 13.70 us | -5.5% | -9.9% to -5.5% | 5/5 |
| p90 | 15.00 us | 14.40 us | -4.0% | -14.6% to -4.0% | 5/5 |
| p99 | 24.60 us | 18.10 us | -26.4% | -26.4% to +13.9% | 3/5 |
| p99.99 | 232.41 us | 746.17 us | +221.1% | +23.9% to +544.9% | 0/5 |

Batch size = 1,000:

| Metric | Baseline | SPSC + pinned | Delta (representative) | Delta range (5 runs) | Runs improved |
|---|---|---|---|---|---|
| Throughput | 8,277,591 ev/s | 9,325,844 ev/s | +12.7% | +5.2% to +14.0% | 5/5 |
| p50 | 116.00 us | 104.70 us | -9.7% | -12.2% to -9.7% | 5/5 |
| p90 | 125.32 us | 112.40 us | -10.3% | -16.1% to -10.0% | 5/5 |
| p99 | 217.04 us | 130.31 us | -40.0% | -40.0% to +17.1% | 4/5 |
| p99.99 | 551.73 us | 343.41 us | -37.8% | -37.8% to +285.7% | 2/5 |

Batch size = 10,000:

| Metric | Baseline | SPSC + pinned | Delta (representative) | Delta range (5 runs) | Runs improved |
|---|---|---|---|---|---|
| Throughput | 6,429,785 ev/s | 7,613,021 ev/s | +18.4% | +0.6% to +18.4% | 5/5 |
| p50 | 1,533.45 us | 1,278.15 us | -16.6% | -16.6% to -3.3% | 5/5 |
| p90 | 1,724.28 us | 1,394.08 us | -19.2% | -19.2% to -1.1% | 5/5 |
| p99 | 2,123.27 us | 1,934.98 us | -8.9% | -8.9% to +78.7% | 2/5 |
| p99.99 | 2,394.66 us | 2,452.37 us | +2.4% | -7.1% to +66.8% | 1/5 |

* **Key finding, stated as measured, not as hoped for:** throughput and p50/p90 improve *every single time* (15/15 trials across all batch sizes and runs) once a good core is selected -- consistent, real gains from cache locality and the SPSC ring's simpler push path. p99 is a mixed bag (9/15 improved). **p99.99 gets worse more often than not (12/15 trials), sometimes by 5-6x**, and this did not go away with a properly probed, genuinely fast core -- it is not the same failure mode as the naive-core-selection finding above.
* **Interpretation:** `SetThreadAffinityMask`/`pthread_setaffinity_np` pin a thread to a core; they do not reserve that core *exclusively*. Real OS-level isolation (Linux `isolcpus`/`nohz_full`, Windows CPU Sets in reserved-exclusive mode) is what that would take, and this benchmark deliberately doesn't set that up -- nor would a general-purpose development laptop, with its normal load of background OS/user processes, be a realistic target for it anyway. An unpinned thread that hits contention can migrate to any idle core; a pinned thread has nowhere to go until its one core frees up. That specifically inflates the rare, worst-case tail even as it improves the common case -- a real, repeatable, physically-explicable result, reported here exactly as measured rather than adjusted to match the "pinning reduces p99.99" outcome this phase originally set out to confirm.
* **Status:** Phase 14 Fintech Tail Latency (SPSC + Pinned) Verified -- throughput and typical-case latency improvement confirmed and reproducible; p99.99 reduction NOT confirmed under thread-affinity-only pinning on this hardware/OS combination, and the benchmark says so in its own output, not just here

## Phase 15: Complex Event Processing (CEP) -- Sliding-Window Aggregation Rules

### Design Verification (Before Integration, Not After)

* **Target System:** `animus::CepRuleState` (`animus.hpp`) -- sliding-window SUM/AVG/MIN/MAX over count-based or time-based windows, evaluated per matching event. SUM/AVG maintain an O(1)-amortized running total; MIN/MAX use the standard monotonic-deque sliding-window-minimum/maximum algorithm (O(1) amortized), not a per-event full rescan.
* **Method:** before this design went anywhere near `animus.hpp`, a standalone program exercised the exact same window/eviction/aggregation logic against a naive brute-force reference (rescan every event still logically inside the window, from scratch, on every step) -- 8 hand-picked deterministic sequences (covering both window types and all four aggregations, including a window-size-1 edge case) plus 200 randomized trials per aggregation per window type (1,600 randomized trials total: 200 x 4 aggregations x 2 window types), each a 50-event sequence with random values and random window sizes/time gaps.

| Check | Result |
|---|---|
| 8 hand-picked deterministic sequences (both window types, all 4 aggregations, incl. window-size-1) | All matched the naive reference at every step |
| 1,600 randomized trials (200 x 4 aggregations x 2 window types, 50 events each) | All matched the naive reference at every step |

* **Status:** Phase 15 CEP Design Verification Passed (0 discrepancies across 1,608 total trial sequences)

### Correctness: Real Engine Round Trips

* **Target System:** `animus_add_cep_rule` end-to-end through both engines the SDK can run on -- the real compiled binary (`AnimusBindings`, native path) and `_PurePythonEngine` (the fallback used when no binary is compiled) -- via `tests/test_bindings.py`.
* **Method:** the same count-window SUM case (window=3, threshold `SUM > 50`, events `[10, 20, 30, 5, 1, 100]`) run against both engines independently; expected matches at trace_id 2 (sum=60), 3 (sum=55), and 5 (sum=106) computed by hand before either engine ran.

| Check | Native engine | Pure-Python fallback |
|---|---|---|
| Matches at trace_id 2, 3, 5 with aggregated values 60, 55, 106 | Pass | Pass |

* **A design choice verified, not just implemented -- AVG's exact-integer comparison:** `CepRuleState::on_event` checks `sum COMPARATOR threshold * count` rather than dividing and comparing a float, so the comparison stays exact integer arithmetic (no floating point anywhere in the CEP hot path). A dedicated test proves this isn't cosmetic: window `[10, 10, 11]`, rule `AVG > 10`. Exact average is 10.333..., so the rule should match -- and the cross-multiplication check confirms it (`31 > 10*3` = `31 > 30` = true). A naive "floor the average, then compare" implementation would get this wrong: `floor(31/3) = 10`, and `10 > 10` is false -- silently missing a real match at the exact boundary. Verified against the real native engine; passed.
* **Full suite:** 35/35 tests passing (7 new for this phase: CEP marshalling, native round trip, pure-Python round trip, AVG cross-multiplication, and rejection of an unrecognized window type/aggregation/comparator).
* **Status:** Phase 15 Correctness Verified (native and pure-Python engines agree; the AVG exact-arithmetic design is regression-tested, not just documented)

### Native Hot-Path Evaluation Overhead

* **Target System:** cost of `evaluate_cep_rules` itself, isolated from disk I/O and signal-ring push cost -- every registered CEP rule's `threshold` is set unreachably high (`1 << 62`) so no rule ever matches, meaning every run does the same amount of *evaluation* work (window push + eviction + aggregate check) but never touches the signal ring.
* **Method:** 300,000 events pushed via `record_events_batch` with the persistence worker active (`start_logging` -> push -> `stop_logging`, which blocks until fully drained and evaluated -- the same full-pipeline timing convention as Phase 11-13), at 0 / 1 / 10 / 50 concurrently registered CEP count-window SUM rules (window=100). Each rule-count configuration runs in its own subprocess (rules can only accumulate within a native engine process -- `add_cep_rule` has no remove -- so reusing one process across configurations would silently test "N rules plus every rule from every earlier configuration," not N rules; caught by an obviously-wrong monotonically-increasing elapsed-time trend across configurations before this was fixed and rerun cleanly). 5 consecutive full runs.

| CEP rules registered | Representative run (elapsed / throughput) | Range across 5 runs (elapsed) | Range across 5 runs (throughput) |
|---|---|---|---|
| 0 (baseline) | 80.23 ms / 3,739,180 ev/s | 80.23 - 90.57 ms | 3,312,245 - 3,739,180 ev/s |
| 1 | 85.36 ms / 3,514,374 ev/s | 85.36 - 89.20 ms | 3,363,138 - 3,514,374 ev/s |
| 10 | 89.17 ms / 3,364,364 ev/s | 85.86 - 94.30 ms | 3,181,390 - 3,494,211 ev/s |
| 50 | 139.56 ms / 2,149,676 ev/s | 125.76 - 139.56 ms | 2,149,676 - 2,385,407 ev/s |

* **Finding:** 1 and 10 rules are statistically indistinguishable from the 0-rule baseline -- their ranges overlap almost entirely, consistent with each rule's O(1)-amortized per-event cost (one `deque::push_back`, an eviction check, one integer comparison) being small enough that disk I/O and batch-marshalling overhead dominate at low rule counts. 50 rules is a real, consistently reproducible effect across all 5 runs: throughput drops by roughly 30-40% (never overlapping the baseline's range in any run). Computing the marginal per-rule, per-event cost directly from each run's own baseline (`(elapsed@50 - elapsed@0) / events / 50`) gives 2.43-3.96 ns/rule/event across the 5 runs (mean 3.02 ns) -- consistent with a small, roughly constant per-rule cost that only becomes visible once enough rules are summed to rise above the persistence pipeline's other costs, the same shape Phase 4 found for plain `RuleThreshold` evaluation (linear in rule count, `evaluate_rules`/`evaluate_cep_rules` both iterate every registered rule per matching event).
* **Status:** Phase 15 Hot-Path Overhead Verified -- negligible at typical rule counts (0-10), small and linear at higher counts (measured at 50), consistent with the O(rules)-per-event iteration this design always implied

## Phase 16: Cross-Platform Shared-Memory (MMF) IPC Transport

### Correctness & Interop Verification

* **Target System:** `animus::SharedMemorySegment` + `animus::SharedTelemetryChannel` (`animus.hpp`) -- named OS shared memory (Windows `CreateFileMappingA`/`MapViewOfFile`, POSIX `shm_open`/`mmap`), deliberately wire-compatible with the pre-existing pure-Python `animus.shm.SharedTelemetryRing` (same 24-byte header, same 24-byte record) so either implementation can produce or consume on the same segment.
* **Method:** 46 tests (11 new for this phase) against the real compiled binary -- same-process round trip, ring-full/ring-empty boundary behavior, a deliberately non-power-of-two capacity (37) with real wraparound past capacity multiple times, both interop directions, error handling (attach to a nonexistent segment, create with a duplicate name, zero capacity), and a genuine cross-process test using a real `subprocess.Popen` child, not a second handle in the parent process.

| Check | Result |
|---|---|
| Same-process create -> push -> attach -> pop round trip | Pass |
| Non-power-of-two capacity (37), wrapped past capacity 3x over | Pass |
| Ring-full push returns False; ring-empty pop returns None | Pass |
| Native `SharedTelemetryChannel` creates, pure-Python `SharedTelemetryRing` reads | Pass |
| Pure-Python `SharedTelemetryRing` creates, native `SharedTelemetryChannel` reads | Pass |
| Real cross-process round trip (separate `subprocess.Popen` child process) | Pass |
| `attach()` to a nonexistent segment / `create()` with a duplicate name / capacity=0 | Correctly raise `OSError`/`ValueError` |
| Full suite | 46/46 passing |

* **A real interop bug caught before it shipped:** the first draft indexed ring slots with a power-of-two bitmask (`& (capacity - 1)`), matching `SpscRingBuffer`'s in-process convention. `animus.shm.SharedTelemetryRing` uses plain modulo and never requires a power-of-two capacity -- a bitmask against a segment the Python side created with, say, capacity 37 would silently index the wrong slot, corrupting data with no error raised anywhere. Caught during design review (before any test was run), fixed to modulo on the native side to match, then verified with the non-power-of-two test above.
* **Status:** Phase 16 Correctness & Interop Verified

### IPC Latency: Measured Layer by Layer, Not Asserted

* **Target System:** four distinct latency numbers, each isolating a different layer of the stack, because "IPC latency" is not one number -- the native transport operation, the ctypes call wrapping it, and genuine cross-process propagation are three different costs that a single benchmark would conflate.
* **Method:** (1) native same-process `push()` call latency, 1,000,000 calls, `std::chrono::steady_clock`. (2) Python same-process `push()` call latency, 100,000 calls, `time.perf_counter_ns()` -- same methodology as Phase 11/13. (3) native cross-process latency: a real second OS process (not a thread, not a second handle) attached via `SharedTelemetryChannel::attach()`, spin-polling `pop()`; the producer packs its own `steady_clock` timestamp into each event's `metric_value` field, the consumer computes `observed_time - packed_time` on receipt -- both processes read the same system-wide monotonic clock source (`QueryPerformanceCounter`/`clock_gettime(CLOCK_MONOTONIC)`), so cross-process comparison is valid. (4) the same measurement from Python, with the producer paced (1 event/ms) specifically to prevent a backlog from forming -- see the finding below for why that pacing turned out to matter. 3 runs each.

| Layer | Representative run (mean / p50) | Range across 3 runs (mean) |
|---|---|---|
| (1) Native same-process `push()` call | 39.6 ns / 0 ns | 35.3 - 39.6 ns |
| (2) Python same-process `push()` call (ctypes) | 1,327.7 ns / 1,300 ns | 1,308.8 - 1,329.5 ns |
| (3) Native cross-process, real 2-process handoff (burst, 20,000 events) | 3,472.6 ns / 2,400 ns | 3,472.6 - 22,148.3 ns (p50: 2,400 - 13,900 ns) |
| (4) Python cross-process, paced to prevent backlog (5,000 events) | 10,044.7 ns / 6,900 ns | 9,846.7 - 10,215.7 ns (p50: 6,900 ns every run) |

* **A real finding caught mid-measurement, not glossed over:** the first cross-process Python measurement (unpaced, producer pushing 20,000 events back-to-back as fast as possible) reported a wildly different number -- mean 4.4-5.2 ms across 4 runs, orders of magnitude higher than every other row here. Diagnosing it before reporting it: the *first* event observed had a latency of 14-26 us (consistent with row (4) above), but the *last* event's latency was 13.6 ms, growing roughly linearly across the batch. That is the signature of a producer-faster-than-consumer backlog accumulating in the ring (a throughput mismatch, not a transport-latency problem) -- not surprising in hindsight, since the Python consumer's per-`pop()` ctypes cost is comparable to the producer's per-`push()` cost, and a burst of 20,000 back-to-back pushes gives the queue no opportunity to drain between events. Row (3) above (native burst) mostly avoids this because native `pop()`/`push()` are ~35x cheaper per call than their Python-ctypes-wrapped equivalents, so the native consumer keeps pace even unpaced -- though even there, run-to-run variance (2.4-13.9 us p50) shows the same underlying sensitivity to consumer scheduling, just less severe. Row (4)'s pacing exists specifically to measure genuine per-event propagation latency instead of an artifact of this queueing effect.
* **Interpretation:** "sub-microsecond" is true of the native transport operation in isolation (row 1) -- writing into shared memory really does cost tens of nanoseconds. It is not true of genuine cross-process propagation, native or Python, on this general-purpose, non-isolated development machine (rows 3-4 are single-digit-to-low-double-digit microseconds) -- real OS scheduling and cross-core cache-coherency delay dominate once two actual processes are involved, the same theme Phase 14 found for CPU pinning on this same hardware. A caller choosing this transport for a genuinely latency-sensitive path should pace bursty producers (row 4's finding) and consider the CPU-pinning primitives from Phase 14 for the producer/consumer threads specifically, rather than assume the native call's own sub-microsecond cost is what a remote reader will actually observe.
* **Status:** Phase 16 IPC Latency Measured -- native local operation is genuinely sub-microsecond; cross-process propagation (native or Python) is not, and is reported here at every layer rather than only the most favorable one