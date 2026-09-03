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

## Phase 19: Automated Institutional Benchmark Suite

### Tick-to-Trade End-to-End Latency (`MarketDataFeed` -> `ExecutionClient`)

* **Target System:** `AnimusCore_v1/animus_benchmark_suite.cpp` -- a header-only, native C++17 binary (`#include "animus.hpp"` only, no DLL) driving `animus::MarketDataFeed` push -> poll -> `animus::ExecutionClient::submit()` (`LoopbackBrokerGateway`: an instant, deterministic in-process fill, so the measured cost is this pipeline's own overhead, never a real broker's). Compiled and run automatically by `benchmarks/generate_benchmark_report.py`, which also renders `benchmarks/BENCHMARK_REPORT.md`.
* **Why native C++, not Python/ctypes:** the GIL serializes Python "threads" onto one core, so an 8-Python-thread ring-contention test (below) would never exercise real cross-core contention; and the ctypes call-marshalling tax this repo already measured (Phase 16, row 2 above: ~1,300 ns/call) is on its own wider than the sub-microsecond budget this phase reports on. Both measurements need real OS threads and in-process calls with no FFI boundary.
* **Method:** 500,000 ticks, single-threaded, sequential -- push one tick (timestamped with `std::chrono::steady_clock`, not the TSC-based `read_cycle_counter()`, to avoid needing a calibrated TSC frequency), immediately poll it back out, immediately submit an order for it, timestamp again. 5 consecutive runs.

| Metric | Representative run | Range across 5 runs |
|---|---:|---:|
| Mean | 94.2 ns | 92.7 - 95.5 ns |
| p50 | 100.0 ns | 100.0 ns (every run) |
| p99 | 100.0 ns | 100.0 ns (every run) |
| p99.9 | 200.0 ns | 100.0 - 200.0 ns |
| Max | 118,500.0 ns | -- (single-outlier field; see note below) |
| Throughput (sequential) | 8,416,814 ticks/sec | 8,275,063 - 8,541,592 ticks/sec |

* **A real methodology bug caught before any number was reported, not after:** the first version of this benchmark used a dedicated producer thread pushing ticks and a separate consumer thread polling and submitting them -- and measured mean/p50 latency in the *milliseconds*, not nanoseconds. Diagnosis: an unpaced producer thread pushes 500,000 ticks far faster than the consumer thread can drain and process them, so most ticks sit queued in a growing backlog before ever being touched -- exactly the "producer-faster-than-consumer backlog" pitfall this repo already hit and documented once before, in Phase 16's shared-memory IPC benchmark above (a burst without pacing looked like catastrophic latency until it was traced to a throughput mismatch, not the transport). Fixed by rewriting the benchmark as a single-threaded sequential push -> poll -> submit loop, matching `execution_interop_demo.cpp`'s own established latency-measurement methodology (Phase 7) -- a decision-loop metric, not a queueing-delay metric, which is what "tick-to-trade latency" is actually supposed to mean.
* **Measurement note:** repeated values quantized to whole hundreds of nanoseconds reflect this machine's `steady_clock` resolution (Windows: `QueryPerformanceCounter`-backed), not true single-digit-nanosecond precision. Max varied by more than 3x across runs (38,500 - 120,500 ns) while p99.9 stayed flat at 100-200 ns every time -- consistent with rare, individual OS scheduling interruptions across 500,000 iterations on a general-purpose, non-real-time OS hitting one or two samples per run, not a systemic tail problem.
* **Status:** Phase 19 Tick-to-Trade Latency Verified -- genuinely sub-microsecond at p50/p99/p99.9 (100-200 ns), reproducible across 5 consecutive runs.

### Lock-Free Ring Buffer Throughput Under 8-Thread Concurrency

* **Target System:** `animus::LockFreeRingBuffer<TelemetryPayload>` -- the same Vyukov MPMC ring `EngineImpl`'s own telemetry ring uses, not a synthetic stand-in -- driven directly (no C-ABI, no ctypes) by 8 concurrent producer threads (real `std::thread`s, real OS scheduling across real cores), all contending on the same compare-exchange retry loop.
* **Method:** each of 8 threads pushes 200,000 records (1,600,000 total); the ring is pre-sized to hold every push from every thread so throughput reflects `push()` cost under contention, not backpressure stalls from a concurrent consumer. Per-push latency sampled into per-thread-local vectors (no shared results container touched inside the timed loop) and merged only after every thread joins. Correctness verified per run: every push must be drainable back out exactly once, or the binary exits with an error rather than reporting a result. 5 consecutive runs.

| Metric | Representative run | Range across 5 runs |
|---|---:|---:|
| Aggregate throughput | 7,216,498 pushes/sec | 6,929,317 - 9,565,082 pushes/sec |
| Per-push mean latency | 996.1 ns | 636.4 - 1,054.8 ns |
| Per-push p50 latency | 600.0 ns | 300.0 - 700.0 ns |
| Per-push p99 latency | 5,200.0 ns | 3,600.0 - 5,200.0 ns |

* **Status:** Phase 19 Ring Buffer Throughput Verified -- 7-9.6M pushes/sec sustained under real 8-thread contention across all 5 runs, with correctness (no lost/duplicated push) confirmed every time.

### CPU Cache Locality: Pointer-Chase Sweep + False-Sharing A/B Test

* **Target System:** a Sattolo-shuffled pointer-chase sweep (the standard "membench" technique -- a single N-cycle permutation, no shorter sub-cycles, so each jump is data-dependent on the previous one and effectively unpredictable to the hardware prefetcher) across 16 working-set sizes from 4 KB to 128 MB, one 64-byte node per cache line, 3,000,000 chase steps per size; plus a false-sharing A/B test (two `std::atomic<uint64_t>` counters, each incremented 20,000,000 times by its own thread -- once sharing a cache line, once on separate cache lines via `alignas(64)`).
* **Why this matters beyond an abstract microbenchmark:** the padded layout is the exact one `animus::LockFreeRingBuffer` (`enqueue_pos_`/`dequeue_pos_`) and `animus::SpscRingBuffer` (`head_`/`tail_`) already use in this codebase -- this test is a direct empirical justification of an existing design choice, not a hypothetical.

| Working Set | Avg Latency (ns/access) | Tier |
|---:|---:|---|
| 4 KB | 1.135 | Tier 1 (fastest) |
| 8 KB | 1.213 | Tier 1 |
| 16 KB | 1.243 | Tier 1 |
| 32 KB | 1.198 | Tier 1 |
| 64 KB | 3.728 | Tier 2 |
| 128 KB | 3.767 | Tier 2 |
| 256 KB | 3.205 | Tier 2 |
| 512 KB | 3.638 | Tier 2 |
| 1 MB | 4.516 | Tier 2 |
| 2 MB | 8.696 | Tier 3 |
| 4 MB | 11.891 | Tier 3 |
| 8 MB | 23.769 | Tier 4 |
| 16 MB | 18.350 | Tier 4 |
| 32 MB | 77.646 | Tier 5 (slowest -- consistent with spilling into DRAM) |
| 64 MB | 96.094 | Tier 5 |
| 128 MB | 100.132 | Tier 5 |

* **Tier labels are inferred from >1.8x jumps between consecutive points in this sweep's own measured curve, not a claim about this CPU's real L1/L2/L3 sizes** -- `animus_benchmark_suite.cpp` never queries CPUID or a vendor spec sheet for that information.
* **A real finding worth stating plainly, not smoothed over:** two knees were large and consistent in every one of 5 runs -- a ~3x jump at the 32 KB -> 64 KB transition, and a large jump into the 32-128 MB range consistent with spilling past the last on-die cache level into DRAM. The middle boundary (around 1-2 MB) was not consistent run to run: it crossed this benchmark's 1.8x tier threshold in some runs and not others, depending on measurement noise near that specific size. Reported as observed -- a real, if less sharply defined, transition -- rather than picking whichever single run made the tier count look cleanest.
* **False-sharing A/B result:**

| Layout | Combined ops/sec (representative) | Speedup range across 5 runs |
|---|---:|---:|
| Unpadded (false sharing) | 123,747,214 | -- |
| Padded (`alignas(64)`) | 562,887,161 | -- |
| **Speedup** | **4.55x** | **4.21x - 4.99x** |

* **Status:** Phase 19 CPU Cache Locality Verified -- consistent multi-tier latency curve with real knees at expected boundary scales; cache-line padding measured at >4x throughput improvement over false sharing in every run, directly validating this codebase's existing `alignas(64)` design choice with real data rather than by assertion.

### Reproducing Phase 19

```bash
python benchmarks/generate_benchmark_report.py
```

Compiles `AnimusCore_v1/animus_benchmark_suite.cpp` (g++/clang++, `-std=c++17 -O2 -pthread`) into `benchmarks/_build/` if the binary is missing or stale, runs it, and writes `benchmarks/BENCHMARK_REPORT.md` -- the auto-generated, always-current counterpart to this hand-maintained section (which records one point-in-time snapshot plus what was learned getting there, including the bug above; the generated report reflects whatever the suite measures on whichever machine last ran it).
* **Status:** Phase 16 IPC Latency Measured -- native local operation is genuinely sub-microsecond; cross-process propagation (native or Python) is not, and is reported here at every layer rather than only the most favorable one

## Phase 20: Header-Only Generic Shared-Memory IPC Ring (`include/animus/shm_ipc.hpp`)

### Cross-Process Lockstep Latency (`animus::sys::ipc::ShmRing<T>`)

* **Target System:** `include/animus/shm_ipc.hpp`'s `animus::sys::ipc::ShmRing<T>` -- a *different* shared-memory primitive from `animus::SharedMemorySegment`/`SharedTelemetryChannel` documented under Phase 6 and Phase 16 above. That pair is deliberately wire-compatible with the pure-Python `animus.shm` module and NOT cache-line-padded, to preserve byte-for-byte interop. `ShmRing<T>` has no such compatibility constraint: it is a generic template for any trivially-copyable `T`, with its `Header` explicitly padded to 3 `ANIMUS_CACHE_LINE_SIZE` lines (metadata, producer `head`, consumer `tail`, each on its own line) to eliminate false sharing between the two cursors -- built for raw producer/consumer latency between two native processes, not language interop. Exercised via `AnimusCore_v1/shm_ipc_bench.cpp`, a standalone two-process benchmark (build/run manually, same convention as `animus_benchmark_suite.cpp`/`cluster_latency_bench.cpp`); reuses `animus::cpu_relax()` and `ANIMUS_CACHE_LINE_SIZE` from `include/animus/thread_affinity.hpp` (Phase 14's pinning module) rather than duplicating them.
* **Method:** the benchmark IS both sides -- `shm_ipc_bench.exe --consumer <name> <n>` and `--producer <name> <n>`, launched as two genuinely separate OS processes (consumer first, owning ring creation/unlink; producer retries `ShmRing::open()` for up to 5s to absorb the startup race). Each process pins itself to a distinct core and requests high thread priority via `thread_affinity.hpp`. Latency is a raw TSC delta (`rdtsc()`, mirroring `animus.hpp`'s `read_cycle_counter()` but reimplemented locally so this header stays free of an `animus.hpp` dependency), converted to nanoseconds via a 200ms wall-clock TSC calibration each process performs independently at startup (not a nominal "rated" frequency -- turbo/power-state changes mean the real ratio during a run can differ). 500,000 round trips per run, 5 consecutive runs, real MSVC (`cl /std:c++17 /O2`) compile and run.
* **A real methodology bug found and fixed before any number was reported -- the same failure family as Phase 16 and Phase 19 above:** the first version used one large-capacity ring (4,096 slots) with an unpaced producer and measured 50-100+ *microseconds*, not nanoseconds. What exposed it: the producer's own wall-clock completion time (200,000 messages sent in 21ms -- a raw send rate of ~105 ns/message) was inconsistent by roughly 500x with the "latency" the consumer was reporting for those same messages, and the per-message delta was climbing linearly across the first several messages (not random noise) -- the signature of a standing backlog building up inside the ring, not genuine transit time. A miscalibrated TSC-to-ns conversion was checked and ruled out first (the calibrated frequency, ~2.42 GHz, was itself entirely plausible for this machine) before concluding the ring itself was the problem: an unthrottled producer can burst up to the ring's full capacity ahead of the consumer, and once a backlog of depth *D* exists, every subsequent message inherits roughly that same *D*-deep queueing delay -- exactly the "unpaced burst looks like catastrophic latency" pitfall this codebase had already hit twice before (Phase 16's shared-memory IPC section, Phase 19's tick-to-trade section) and documented both times. Fixed the same way both prior instances were: added a second ring carrying a same-sequence ack, and made producer/consumer lockstep (the producer blocks on the ack before sending the next message, so at most one message is ever in flight) -- matching `animus_benchmark_suite.cpp`'s own tick-to-trade methodology exactly.

| Metric | Representative run | Range across 5 runs |
|---|---:|---:|
| Mean | 37.36 ns | 27.14 - 37.77 ns |
| p50 | 37.62 ns | 23.56 - 37.62 ns |
| p99 | 45.47 ns | 43.40 - 51.26 ns |
| p99.9 | 51.26 ns | 51.26 - 57.04 ns |
| Max | 17,307.4 ns | 15,219.9 - 202,515.6 ns (single-outlier field; see note below) |
| `p50 < 50ns` | true | true (5/5 runs) |

* **Reported precisely, not rounded to fit the target:** p50 and p99 were sub-50ns in every one of the 5 runs. p99.9 was *not* -- it landed at 51-57 ns in all 5 runs, consistently just over the target rather than around it. Max varied enormously run to run (15.2 us to 202.5 us) out of 500,000 samples each -- consistent with a single, rare OS scheduling preemption hitting one or two of the half-million lockstep round trips per run, not a systemic property of the transport (every other percentile stayed tight and consistent across all 5 runs).
* **Status:** Phase 20 Cross-Process IPC Latency Verified -- p50/p99 genuinely sub-50ns and reproducible across 5 consecutive runs; p99.9 consistently just above the 50ns target (51-57 ns), reported as measured rather than omitted.

### Real Cross-Process Ingestion Pipeline (`AnimusCore_v1/shm_ipc_ingest_demo.cpp`)

* **Target System:** `ShmRing<T>` wired into a genuine ingestion pipeline, not just the synthetic latency probe above -- a producer process pushes `animus::RawEvent` telemetry across `ShmRing<RawEvent>`; a consumer process drains it into a real `animus::Engine` (`add_rule`, `record_batch`, `start_persistence`/`stop_persistence`, `poll_signals`), the same native pipeline `ingest_engine.py` drives via ctypes, except events arrive over zero-copy shared memory from a genuinely separate OS process instead of Python-level calls in the same one. Same rule setup as `ingest_engine.py` (event_id 101, rule 1 matches every event, rule 2 never matches) for direct comparability between the two transports.
* **Method:** 200,000 events, real MSVC (`cl /std:c++17 /O2`) compile and run, both processes launched from one shell script with a short fixed startup gap (0.3s) rather than two independently-timed manual invocations -- an early measurement using the latter showed ~4.1-4.2 second wall times dominated almost entirely by that launch-orchestration gap (confirmed via a diagnostic instrumentation pass showing the very first `pop_spin()` succeeding only after ~4.07s even though the corresponding producer process reports finishing in tens of milliseconds), an artifact of how the two processes were started in that measurement, not of `ShmRing`, the engine, or the transport itself -- discarded in favor of the timing below, which isolates real pipeline throughput. 5 consecutive runs.

| Metric | Representative run | Range across 5 runs |
|---|---:|---:|
| Events received / accepted | 200,000 / 200,000 | 200,000 / 200,000 (5/5 runs, zero loss every time) |
| Rule 1 matches (every event) | 200,000 | 200,000 (5/5 runs) |
| Rule 2 matches (should never fire) | 0 | 0 (5/5 runs) |
| Persisted bytes | 12,800,000 | 12,800,000 (5/5 runs -- exactly `accepted * sizeof(TelemetryPayload)` every time) |
| Total wall time | 359.85 ms | 349.89 - 360.09 ms |
| Throughput | 555,790 events/sec | 555,412 - 571,615 events/sec |

* **Two real bugs found and fixed while building this, both the same underlying pattern -- the transport never lost anything, a fast unthrottled consumer did:**
  1. **`record_batch()`'s "stop at the first push that fails" contract was being treated as a permanent drop instead of a retry signal.** A first version called `record_batch()` exactly once per 1,024-event batch and counted any remainder as rejected once the engine's own internal ring filled -- correct per that function's documented contract, but it meant this demo's own draining loop (a pop off `ShmRing` immediately followed by one non-retrying push) could outrun the engine's persistence worker and silently discard the majority of a run: an early 200,000-event run accepted only 65,536 (exactly one ring's worth) despite `ShmRing` having delivered every single event intact. Fixed by retrying the unpushed remainder with `animus::cpu_relax()` between attempts until the persistence worker catches up, matching the same "only the caller's choice not to retry loses data over a momentarily full ring" pattern this codebase's own `push_spin`/`pop_spin` already establish.
  2. **The signal ring (a separate bounded ring from the telemetry ring, same default capacity) saturated because `poll_signals()` was only called once, after the run finished** -- exactly the already-documented Phase 4 "Known Limit" ("under high fan-out with no concurrent `poll_signals()` consumer, it can saturate and silently drop signals past capacity"), triggered here because every one of this demo's events matches rule 1. Fixed the same way `ingest_engine.py`'s own harness already does: a background thread continuously drains `poll_signals()` while ingestion is still running. That fix alone was insufficient, though -- reusing `ingest_engine.py`'s own 1ms `sleep_for` between empty polls still dropped most signals (66,560 of 200,000 matched), because this native/`ShmRing` pipeline generates matches far faster than a ctypes-throttled Python one does: a signal-ring's worth of matches can accumulate *during* a single 1ms sleep. Fixed by spin-polling with `animus::cpu_relax()` instead of sleeping, which resolved it completely (200,000/200,000 matched).
* **Status:** Phase 20 Real Ingestion Pipeline Verified -- zero-loss, byte-exact persistence, and complete rule-match coverage confirmed across 5 consecutive runs; both bugs above were caught by the demo's own internal end-to-end assertions (it exits non-zero on a mismatch), not by eyeballing printed output.

## Phase 21: RBAC-Gated Multi-Tenant Execution Orchestration

### Multi-Tenant Order Routing Latency (`SecureExecutionGateway` + `ShmRing<OrderRequest>`)

* **Target System:** `animus::security::SecureExecutionGateway` (`animus_security.hpp`, new) -- an RBAC/tenant-routing facade over `animus::ExecutionClient`, same shape as the existing `SecureTelemetryGateway` (Phase 8), extended to a part of this codebase that had never had RBAC or tenancy before: `ExecutionClient` (Phase 7) and the entire `animus_security.hpp` layer (Phase 8) had zero C-ABI or Python exposure prior to this phase -- both had only ever been driven from C++ demos. New C-ABI (`animus_security_create_context/close_context/create_tenant/create_execution_tenant/submit_order/poll_execution_audit_log`) and a second `ShmRing<T>` instantiation, `ShmRing<animus::OrderRequest>` (`animus_shm_ring_order_*`), expose both to Python as `SecurityContext` and `ShmOrderRingChannel` (`animus/bindings.py`). Exercised via `AnimusCore_v1/execution_orchestration_demo.py`.
* **Isolation is structural, not a filter:** one `ShmRing<OrderRequest>` **per tenant**, not one shared ring carrying a tenant id on the wire -- a producer for tenant A has no way to address tenant B's ring, matching `animus_security.hpp`'s existing "one Engine per tenant" isolation principle exactly rather than introducing a second, weaker notion of tenant separation for orders specifically.
* **Method:** two tenants (10, 20), 20,000 orders each via `ShmOrderRingChannel.push_batch()` from a genuinely separate producer process per tenant; a consumer process (owning the `SecurityContext`, both tenants' setup, and both rings) drains each ring and calls `SecurityContext.submit_order()` with an `Operator` token scoped to that ring's own tenant -- per-call latency timed with `time.perf_counter_ns()` around the ctypes call (Python-observed, i.e. including ctypes marshalling, same honesty convention as Phase 16's layer-by-layer numbers -- not a native-only figure). 5 consecutive runs, real MSVC (`cl`) DLL build.

| Metric | Representative run (Tenant 10 / Tenant 20) | Range across 5 runs |
|---|---|---|
| Orders filled | 20,000/20,000 both tenants | 20,000/20,000 both tenants, every run (5/5) |
| `submit_order` p50 | 1,700 ns / 1,700 ns | 1,700 - 1,900 ns |
| `submit_order` p99 | 2,200 ns / 2,300 ns | 2,200 - 3,600 ns |
| `submit_order` mean | 1,828 ns / 1,782 ns | 1,782 - 2,023 ns |
| Combined throughput (both tenants) | 56,718 orders/sec | 54,032 - 56,718 orders/sec |
| Negative-path check (Viewer denied) | Pass | Pass (5/5) |
| Denial recorded in execution audit log | Pass | Pass (5/5) |
| Cross-tenant leakage | None (each tenant's fill count matches only its own ring) | None (5/5) |

* **Negative-path and audit verification, not just the happy path:** every run additionally attempts `submit_order()` with a `Role.VIEWER` token (which lacks `Permission.SUBMIT_ORDER`) and asserts it returns `None`, then polls `poll_execution_audit_log()` and asserts the denial is recorded with the correct `tenant_id`/`Permission.SUBMIT_ORDER`/`AuditOutcome.DENIED` -- the same "prove the deny path is load-bearing, not just present" bar Phase 8's own RBAC section already set.
* **A real bug found and fixed while building this demo's own audit check:** the first version polled `poll_execution_audit_log(max_count=10)` immediately after the negative-path probe and asserted the *last* entry was the denial. `poll_execution_audit_log()` is FIFO (oldest first), same as every other `poll_*` in this SDK -- with ~40,000 prior entries already queued from the allowed `submit_order()` calls in the main loop, draining only 10 returned the *oldest* entries (from tenant setup), not the denial. Fixed by fully flushing the backlog (looping `poll_execution_audit_log()` until empty) immediately before the probe, so the single entry left afterward is unambiguously the probe's own -- not a defect in the audit log itself, a fixed assumption in the test that consumed it.
* **Byte-layout verification, not assumed:** `OrderRequest` (32 bytes), `ExecutionReport` (40 bytes), `AccessToken` (24 bytes), and `AuditEvent` (32 bytes) all carry compiler-inserted padding around `uint8_t`-backed enum fields sitting between wider fields -- every offset in the `ctypes.Structure` mirrors (`animus/bindings.py`) was taken from a real `sizeof`/`offsetof` build, not calculated and trusted. `AccessToken`'s padding sits *between* `tenant_id` and `principal_id`, which would make positional construction (`AccessToken(10, 1, Role.OPERATOR)`) silently corrupt `principal_id` -- guarded against with a `classmethod AccessToken.make(tenant_id, principal_id, role)` instead, regression-tested in `tests/test_bindings.py`.
* **Status:** Phase 21 RBAC-Gated Execution Orchestration Verified -- zero-loss, fully isolated, correctly-denied-and-audited multi-tenant order routing confirmed across 5 consecutive runs; both bugs found while building this (the audit-log FIFO assumption above, plus the underlying `SecureExecutionGateway`/`ShmRing<OrderRequest>` machinery itself, which needed no fixes once the layout was verified) were caught by real runs and real assertions, not asserted from the design alone.

## Phase 22: Concurrent Multi-Tenant Stress Hardening

### High-Contention Multi-Tenant Throughput + Memory Leak Check

* **Target System:** `AnimusCore_v1/concurrency_stress_consumer.cpp` (new) -- N genuinely concurrent tenant threads, each its own `ShmRing<animus::RawEvent>` + `animus::Engine` + persistence + spin-polled signal poller (Phase 20's `shm_ipc_ingest_demo.cpp` logic, parameterized by tenant index), fed by `benchmarks/concurrency_stress_producer.py` (N genuinely separate Python processes, one per tenant) and driven by `benchmarks/concurrency_stress_test.py` (the orchestrator). `ShmRing<T>` is strictly single-producer/single-consumer by design -- "multiple Python producers and C++ consumers" here means **N independent tenant pairs running concurrently**, not multiple producers sharing one ring, matching the "isolation is structural" pattern Phase 20/21 already established, scaled up in tenant count and run simultaneously rather than sequentially.
* **Method:** 8 concurrent tenants (matching `animus_benchmark_suite.cpp`'s own "real 8-thread contention" convention), 2,000,000 events each (16,000,000 total per run -- scaled up from an initial 250,000/tenant attempt specifically to give the RSS sampler enough wall-clock duration to produce a meaningful curve, not just 3-4 samples), 5 consecutive runs. A background thread inside the consumer samples this process's own working-set size (`GetProcessMemoryInfo`, same API and reasoning as `benchmarks/stress_test_engine.py`'s `get_rss_bytes()`) every ~200ms throughout the run.
* **What "zero ring overflows" means here, stated precisely:** `ShmRing<T>::try_push`/`push_batch` already return `false`/a short count on a full ring rather than corrupting anything (proven in Phase 20/21's own tests) -- that isn't what's newly verified. What this suite verifies is that under sustained 8-way concurrent contention, every producer's retry-on-partial-push discipline holds up end-to-end: **total events received by every tenant's consumer thread exactly equals what that tenant's producer sent, every run, with no silent loss.**

| Metric | Representative run | Range across 5 runs |
|---|---:|---:|
| Total events (8 tenants x 2,000,000) | 16,000,000 / 16,000,000 received | 16,000,000 / 16,000,000, every run (5/5, zero loss) |
| Combined throughput | 8,025,946 events/sec | 8,014,232 - 8,210,441 events/sec |
| Peak RSS during load | 99.16 MB | 99.13 - 99.18 MB |
| Growth during load (warm -> peak, all 8 tenants active) | 9.43% | 9.40% - 9.43% |
| Post-teardown residual (vs. ~4-5 MB cold baseline) | 4.11 MB | 4.09 - 4.15 MB |

* **Two different memory questions, deliberately not conflated into one "growth %":** this suite's tenants each do a *finite* amount of work and then tear down (unlike Phase 12's single long-lived `Engine`, which stayed alive for its whole measurement), so "warm baseline vs. the sample after everyone finished" is not a leak signal here -- it's proper cleanup. `growth_during_load_pct` (warm -> peak, while all 8 tenants are still concurrently active) is the real "does sustained contention itself cause runaway growth" check -- a real leak would grow this without bound as more events are processed; it stayed a tight, reproducible ~9.4% across all 5 runs. `post_teardown_residual_mb` (sampled once, precisely, right after every tenant thread has joined and freed its own `Engine`/`ShmRing`) is checked for run-to-run *stability*, not for shrinking to zero -- and it landed within 0.1 MB of the ~4-5 MB cold baseline in every run, confirming every tenant's memory was genuinely returned, not leaked.
* **Three real bugs found and fixed while building this, none of them in `ShmRing<T>`/`Engine` themselves:**
  1. **The RSS-sampling window was initially too short to mean anything.** A first attempt at 250,000 events/tenant (2,000,000 total) completed in well under a second -- only 3-4 samples at the 200ms sampling cadence, one run of which showed *negative* "growth" (a sampling-noise artifact, not a real signal). Fixed by scaling up to 2,000,000 events/tenant (16,000,000 total, ~2 second runs), giving a genuinely meaningful curve.
  2. **"Final RSS" was being measured *after* cleanup, not during sustained load -- and comparing them produced a nonsensical result.** The periodic sampler kept running until every tenant thread had been `join()`-ed, by which point every `Engine`/`ShmRing` had already been destructed and its memory freed; comparing that against the warm baseline produced a "-82% growth" reading, as if properly freeing memory were a defect. Diagnosed by dumping the full RSS-vs-time trajectory (temporarily) and observing a *gradual* decline across several samples, not an instant drop -- consistent with tenants finishing and tearing down at staggered wall-clock times, not a measurement-timing bug in a single sample. Fixed by not conflating the two questions at all (see above): `growth_during_load_pct` while tenants are active, `post_teardown_residual_mb` as a separate, absolute figure checked for stability instead.
  3. **The post-teardown residual sample itself was still noisy (swung from ~15 MB to ~75 MB between otherwise-identical runs)** even after fix #2, because it was still being read from the periodic sampler's *last* tick -- which can land anywhere up to 200ms before or after the actual moment every tenant finished tearing down. Fixed by taking one explicit, precisely-timed `get_rss_bytes()` call immediately after the tenant-thread `join()` loop completes, rather than relying on sampler timing luck -- this is what brought the residual down to a tight 4.09-4.15 MB, consistent with the cold baseline, in every run.
  4. **A quieter fourth bug, found while cleaning up test artifacts, not from a wrong number:** each tenant's temp persistence-log file was never actually being deleted (`std::remove()` on Windows fails silently on a file with an open handle, and the `std::ifstream` used to check `persisted_bytes` was never explicitly closed before the `remove()` call). Fixed by scoping the `ifstream` so it closes before `remove()` runs.
* **Status:** Phase 22 Concurrent Multi-Tenant Stress Hardening Verified -- zero data loss across 80,000,000 total events (8 tenants x 2,000,000 x 5 runs), stable ~9.4% memory growth under sustained 8-way concurrent load, and a post-teardown memory residual indistinguishable from the cold baseline in every run -- no leak detected, reported from a measurement methodology that was itself debugged and fixed in the open above, not assumed correct on the first try.

## Phase 23: RSA License Enforcement Hardening

### Structured License Status, Offline Python Tooling, and an Opt-In Execution License Gate

* **Target system:** the offline RSA-2048 license verification introduced pre-session (`animus_verify_license`, `animus.hpp`/`animus_engine.cpp`) already existed and already did real, load-bearing work: signature verification via BCrypt/CNG, expiry enforcement, and machine-fingerprint binding, gating `pin_current_thread_to_core`/`spsc_init`. This phase is additive to that system, not a rebuild of it -- see QUICKSTART.md guide 5 for the full existing design this builds on.
* **What was actually new here, and why nothing else changed:** the request that started this phase asked for the engine to "verify on startup... block execution if invalid." Taken literally as gating `Engine::Create()`/`animus_init` itself, that would have been a breaking change to every existing demo, benchmark, and test in this repo that has never called `verify_license()` (essentially all of them) -- including Phase 21's own `SecurityContextIntegrationTests`, which assert `submit_order()` succeeds with **no license verified at all**, unconditionally, in CI. Silently breaking a feature this codebase shipped and released (`v1.1.0-rc1`) was not an acceptable way to satisfy the request, so the scope was narrowed (confirmed with the user before implementing) to: extend the *existing* opt-in gating pattern rather than making licensing a hard prerequisite for the whole engine.
* **New: `animus::LicenseStatus` + `animus_check_license_status`** (`animus.hpp`/`animus_engine.cpp`) -- the original `animus_verify_license` collapsed every failure to a single `false`; a real deployment wants to know *why* (no license file deployed yet vs. expired vs. issued for a different machine are very different operational situations to log/alert on). `animus_verify_license` is now defined in terms of this same check (`== LicenseStatus::Valid`), not a second, parallel implementation -- there is exactly one place the actual verification logic lives. Exposed to Python as `AnimusBindings.check_license_status()` (returns the enum) and `AnimusBindings.require_license()` (raises `LicenseError` with `.status` attached unless `Valid` -- the "block execution if invalid" primitive for a Python host application's own startup).
* **New: `SecureExecutionGateway::set_execution_license_required(bool)`, OFF by default** (`animus_security.hpp`, C-ABI: `animus_security_set_execution_license_required`) -- an opt-in toggle that, once enabled, makes `submit()` additionally require `animus_is_licensed()` before routing an otherwise RBAC-authorized order, denied-and-audited exactly like any other RBAC denial. This is the literal "blocks execution if invalid" capability: *execution*, in this codebase, means order submission (Phase 21), and this phase makes that specific, real action gate-able on a valid license -- without touching its default (OFF) behavior, so every existing caller and every existing test sees identical behavior to before this phase.
* **New: `scripts/generate_license.py`** -- a zero-dependency Python CLI (`keypair`/`sign`/`status` subcommands). `keypair`/`sign` shell out to the existing `license_tools/*.ps1` scripts rather than reimplementing RSA-2048 keygen/signing in Python: this SDK's zero-third-party-dependency rule (`pyproject.toml`'s `dependencies = []`) rules out `cryptography`/`pycryptodome`, and hand-rolling RSA signing without a real crypto library would be a large, security-sensitive duplicate of a tool that already works and is already verified against `animus_verify_license`'s own `BCryptVerifySignature` call. `status` is the one genuinely new capability -- it calls straight into `animus.bindings.AnimusBindings.check_license_status()`, the real native check, not a reimplementation.
* **New: `tests/test_licensing.cpp`** -- this repo's first native (non-Python) test file. No C++ test framework exists in this codebase (the zero-dependency rule applies natively too) -- hand-rolled asserts + a PASS/FAIL summary + a non-zero exit code, same style as `release_header_smoke_test.cpp`. Links against the compiled DLL's import library directly (a real consumer, not a `GetProcAddress` reimplementation), so it exercises the actual struct layouts and the actual DLL exports, not a hand-mirrored copy of them.

| Check | Result |
|---|---|
| Missing license file | `LicenseStatus::Missing`, `verify_license()` false |
| Truncated (wrong-size) file | `LicenseStatus::Malformed` |
| Wrong-machine fixture (committed, safe) | `LicenseStatus::WrongMachine`, `verify_license()` false |
| Tampered copy of that fixture (signature corrupted) | `LicenseStatus::BadSignature` |
| Valid license for this machine | `LicenseStatus::Valid`, `is_licensed()` true, `licensed_max_cores() > 0` |
| `submit()` with the execution license gate OFF (default) | Succeeds regardless of license state -- unchanged from pre-Phase-23 behavior |
| `submit()` with the gate ON, no license verified | Denied (verified in a fresh process with no prior license state) |
| `submit()` with the gate ON, valid license verified | Succeeds |
| `scripts/generate_license.py sign` -> `status` round trip | `VALID`, exit code 0 |
| `scripts/generate_license.py status` on the wrong-machine fixture | `WRONG_MACHINE`, exit code 1 |
| Full Python suite (`tests/test_bindings.py`) after this phase | 88 tests, 0 failures (5 skipped only when no local test license exists) |

* **A real ambiguity resolved before writing any code, not after:** "no license verified" is a process-wide, one-way state (`animus_verify_license` only ever sets it true, never false, matching the existing `g_license_verified` design) -- so the ON-gate's denial path cannot be exercised in a test process that already verified a valid license earlier for an unrelated check. `tests/test_licensing.cpp` orders its checks so the denial assertion runs before the valid-license section, and the corresponding Python test (`ExecutionLicenseRequirementTests`) runs in a fresh `subprocess`, same convention `UnlicensedGatingTests` already established for exactly this reason.
* **Status:** Phase 23 RSA License Enforcement Hardening Verified -- structured status reporting, an opt-in execution license gate, and offline Python tooling all added without changing any existing caller's behavior; every new code path (all 6 `LicenseStatus` outcomes, both states of the opt-in gate, and the new CLI) exercised against a real compiled DLL and a real signed license, not asserted from the design alone.

## Phase 24: 10-Minute Continuous Soak Test -- Memory Stability, Zero-Allocation Proxy, and P99 Latency Persistence

### Target System

`benchmarks/soak_test_engine.py` -- distinct from `benchmarks/stress_test_engine.py`'s Part 1 (a fixed 1.2M-event burst) and `benchmarks/fintech_tail_latency.py` (max-throughput tail latency over a fixed event count): this script holds the real pipeline (`init` -> `add_rule` -> `start_logging` -> paced `record_events_batch` -> `stop_logging`) at a bounded, sustained rate for a fixed 600-second (10-minute) wall-clock duration against the real compiled `AnimusNative.dll`, sampling RSS on an independent 15-second timer and bucketing every batch's call latency into 30-second windows so a slow drift over minutes is visible instead of averaged into one headline number.

### Method

* **Paced, not max-throughput:** 50,000 events/sec target (500-event batches, ~100 calls/sec), deliberately well below the engine's proven multi-million-events/sec ceiling (Phase 11) -- a soak test's job is holding a bounded sustained load for a long duration, not re-finding a throughput ceiling already characterized elsewhere.
* **Memory:** RSS sampled every 15s on a wall-clock timer independent of batch cadence; warm baseline taken once total events crossed 2x ring capacity (same reasoning as Phase 12's warm-baseline fix -- ring pages are committed, and so counted in RSS, only as first written, not at allocation time).
* **Latency:** each `record_events_batch()` call timed with `perf_counter_ns` around the call only (batch construction happens before the timer starts, same discipline as Phase 13's `fintech_tail_latency.py`); percentiles computed per 30-second window (20 windows total) rather than once for the whole run, so drift shows up between windows, not just in an aggregate number.
* **Zero dynamic allocation:** no native allocation-counting API exists (none was added for this phase either) -- this reuses Phase 12's proxy directly: a flat RSS curve after warm-up is what a zero-per-event-heap-allocation hot path looks like from the outside, since `EngineImpl`'s ring buffers and persistence batch buffer are sized once at construction and never reallocated per drain.
* Ring capacity 65,536; rule threshold 90 against `metric_value = i % 100` (~9% match rate), same convention as Phase 12. One full real run, not a partial or synthetic loop.

### Results (600.0s wall clock, 28,467,000 events, 56,934 batches)

| Metric | Result |
|---|---|
| Sustained throughput | 47,444 events/sec (target 50,000; difference is Python-side pacing-loop overhead) |
| Cold baseline RSS (before `init`) | 25.25 MB |
| Post-init RSS | 36.62 MB |
| Warm baseline RSS (2.8s, ring cycled 2x) | 37.45 MB |
| Final RSS (after `stop_logging` + `gc.collect`) | 38.47 MB |
| Max RSS observed during the run | 38.49 MB |
| RSS growth, warm baseline -> final | +1.02 MB (+2.72%) |
| Memory stability (<=10% growth threshold) | PASS |
| Persistence integrity | OK -- 1,821,888,000 / 1,821,888,000 bytes (28,467,000 x 64 bytes/record) |
| Threat signals matched and drained | 2,562,030 |
| First-stable-window p99 (window 1, 30-60s) | 287.30 us |
| Worst-window p99 (window 15, 450-480s) | 648.96 us (+125.9% vs. baseline) |
| P99 latency persistence (<=50% drift threshold) | FLAGGED |
| Zero dynamic allocation (RSS-plateau proxy) | CONSISTENT |

* **Memory: genuinely flat.** RSS climbed from 36.62 MB (post-init) to a 37.45 MB warm baseline within the first ~3 seconds -- ring pages committing on first write, the same one-time effect Phase 12 documented -- then held in a 37.45-38.49 MB band for the entire remaining ~597 seconds and 28.3M events. 2.72% total drift from warm baseline to final is comfortably under the 10% leak-flag threshold and consistent with ordinary allocator/heap noise, not a leak; none of the 40 RSS samples taken across the run shows unbounded growth.
* **P99 latency: window 15 flagged, but the underlying signal doesn't point at the engine.** Window 15 (450-480s) posted a 648.96 us p99, +125.9% over the window 1 baseline (287.30 us), tripping the drift threshold. What did and didn't move across all 20 windows matters more than the one flagged number: p50 stayed essentially flat the entire run (71.4-88.5 us in every window, including window 15 itself at 88.5 us -- barely above its neighbors); every window's max sits far above its own p99 and both are noisy window-to-window (e.g. window 16's 14,179.4 us max against a 382.89 us p99) in a pattern present from window 1 onward, not one that worsens with elapsed time or events processed. This is the same category of effect Phase 14 documented for an unpinned thread on a general-purpose OS: no CPU pinning or OS-level core isolation means the producer thread can be preempted by the scheduler for an arbitrary duration at any point, inflating exactly the tail percentiles (p99, max) without touching the typical case (p50, mean), independent of run duration. Read together with the flat p50 and the flat RSS over the same window, this run's data does not support a real latency-persistence defect in the native engine -- it reads as ordinary OS scheduler jitter on an unpinned thread, not engine degradation under sustained load. The drift check is reported as FLAGGED rather than silently reclassified as a pass: a threshold that gets explained away by hand every time it fires stops being a check. A follow-up run pinned via `animus_pin_current_thread_to_core` (Phase 14) is the natural next step to separate scheduler noise from a real regression with more confidence than a single unpinned run can provide.
* **Status:** Phase 24 Soak Test Verified (600s continuous run, real native engine, not synthetic) -- memory stability PASS (2.72% growth, well under the 10% threshold), zero-dynamic-allocation proxy CONSISTENT, persistence integrity OK; P99 latency persistence FLAGGED by the automated threshold on one window (+125.9%), attributed to unpinned-thread OS scheduler jitter rather than a native engine issue based on the accompanying flat p50/RSS evidence -- not silently passed, and not overstated as a confirmed regression either.

## Phase 25: 10-Minute Soak Test, Re-run With CPU Affinity Pinning

### Target System

Phase 24's own hypothesis for its FLAGGED P99 check was unpinned-thread OS scheduler jitter, not a native engine defect -- this phase tests that hypothesis directly rather than leaving it as an educated guess. `benchmarks/soak_test_engine.py` gained `try_pin_producer_thread()` and `find_best_core()`, which pin the soak test's single producer thread to a specific logical core and raise its scheduling priority before the 600-second loop starts, then re-run the identical 600-second, 50,000-events/sec, 500-event-batch configuration Phase 24 used, so the two runs are an apples-to-apples before/after comparison, not a different workload.

**No changes to `animus.hpp` / `animus_engine.cpp` / `animus/bindings.py` were needed.** `animus_pin_current_thread_to_core`, `animus_set_thread_high_priority`, and `animus_get_cpu_count` already existed end-to-end (added in Phase 14, license-gated in Phase 23) and needed nothing beyond calling them from this new script -- the same primitives `benchmarks/fintech_tail_latency.py` already uses for its SPSC-ring producer, applied here to this script's real `Engine` pipeline (`init`/`add_rule`/`start_logging`/`record_events_batch`) instead. This was confirmed by reading the native gating logic (`animus_pin_current_thread_to_core` fails closed with no verified license, not even core 0) before writing any new code, rather than assumed.

### Method

* **License:** pinning is opt-in and license-gated at the native layer with no unlicensed default -- a fresh local test license for this machine was generated (`python scripts/generate_license.py sign --out AnimusCore_v1/license_tools/private/test_license_for_this_machine.lic --max-cores 24`), the same gitignored, per-machine, regenerate-as-needed file `tests/test_bindings.py`'s `_LOCAL_TEST_LICENSE` convention already established -- never committed, since a license is bound to one machine's hardware fingerprint. `try_pin_producer_thread()` verifies it via `animus_verify_license` and warns-and-falls-back-to-unpinned (does not fail the run) if no valid license is present, the same graceful-skip philosophy Phase 23 established for every other opt-in licensed capability.
* **Which core:** probed, not guessed, reusing Phase 14's exact method (`find_best_core`) rather than a fixed heuristic -- 6 candidate logical cores (0, 1, 6, 12, 18, 23 on this 24-logical-core machine) each timed with a small SPSC-ring workload (50 calls x 200 events), lowest measured p99 wins. Core 6 was selected (33.95 us probe p99, vs. 42.52-61.58 us for the other five candidates) and used for the entire 600-second run via `animus_pin_current_thread_to_core(6)` + `animus_set_thread_high_priority()`.
* Everything else identical to Phase 24: real `AnimusNative.dll`, 500-event batches paced to 50,000 events/sec, 65,536-capacity ring, RSS sampled every 15s, latency bucketed into 30-second windows, one full 600-second run.

### Results (600.0s, 28,659,500 events, 57,319 batches) -- vs. Phase 24 (unpinned)

| Metric | Phase 24 (unpinned) | Phase 25 (pinned to core 6) |
|---|---|---|
| Sustained throughput | 47,444 events/sec | 47,765 events/sec |
| Warm baseline RSS | 37.45 MB | 44.88 MB |
| Final RSS | 38.47 MB | 45.10 MB |
| RSS growth, warm -> final | +2.72% | +0.49% |
| Memory stability | PASS | PASS |
| Persistence integrity | OK | OK |
| First-stable-window p99 | 287.30 us | 127.30 us |
| Worst-window p99 | 648.96 us (window 15) | 132.71 us (window 2) |
| P99 drift vs. baseline | +125.9% | +4.3% |
| P99 latency persistence (<=50% threshold) | FLAGGED | PASS |
| Per-window mean/p50 range | 77.97-126.27 / 71.4-88.5 us | 79.33-81.62 / 74.9-77.6 us |
| Per-window max range | 682.4-14,179.4 us | 152.4-1,188.9 us |

* **The P99 drift flag is gone, and the mechanism matches the Phase 24 hypothesis exactly.** Pinned, every one of the 20 windows' p99 sits in a tight 118.6-132.7 us band -- the +125.9%-drift outlier window from Phase 24 has no counterpart here. This is precisely what "unpinned scheduler jitter, not engine degradation" predicts: removing the jitter source (an unpinned thread free to be preempted and migrated) removes the symptom (tail-percentile spikes uncorrelated with elapsed time or event count), without needing any change to the engine itself.
* **Higher baseline RSS is expected, not a regression.** Both runs' warm-baseline RSS differ by ~7.4 MB (37.45 -> 44.88 MB) because this run does strictly more at startup than Phase 24's: verifying an RSA-2048 license (`animus_verify_license`, Windows BCrypt/CNG) and initializing a second ring (the standalone SPSC ring used only for the pre-run core probe, 65,536 x 64-byte `TelemetryPayload` records, ~4 MB) that Phase 24 never touched. Warm-to-final growth is what actually answers "does this run leak," and it's smaller here (+0.49%) than Phase 24's already-passing +2.72%, not larger.
* **Pinning is not a complete fix for tail-latency spikes, and this run's own data says so plainly.** Two of the twenty windows (10 and 16) still show a max latency far above their neighbors (1,188.9 us and 963.1 us respectively, against a ~150-210 us max everywhere else) -- an order of magnitude smaller than Phase 24's worst spikes (up to 14,179.4 us), but not zero. This matches Phase 14's own documented caveat precisely: `SetThreadAffinityMask` pins a thread to a core, it does not reserve that core exclusively, so a pinned thread can still be preempted by something else scheduled onto the same core -- it just has nowhere else to go while that happens, which is why the *frequency* of spikes drops sharply (2 of 20 windows here vs. essentially every window in Phase 24) while a rare spike is not fully eliminated. Reporting this rather than rounding it off to "pinning fixed it" is the same standard Phase 14 already set for itself.
* **Status:** Phase 25 CPU Affinity Pinning Verified -- re-running Phase 24's identical 600-second workload with the producer thread pinned to a probed-best core turned the P99 latency persistence check from FLAGGED (+125.9% drift) to PASS (+4.3% drift), directly confirming Phase 24's scheduler-jitter explanation rather than leaving it as an inference; memory stability and persistence integrity remained PASS/OK as in Phase 24, and two residual tail-latency outliers (down an order of magnitude from Phase 24, but not eliminated) are reported rather than omitted, consistent with Phase 14's own finding that thread pinning without OS-level core isolation narrows, but does not guarantee away, rare scheduler-preemption spikes.

## Phase 26: Regression Check -- `stress_test_engine.py` Clean, `fintech_tail_latency.py` Fix + Re-Verification

### Target System

A full regression pass across the two other native-engine benchmark/stress scripts, run after Phase 24/25's soak-testing work landed, to confirm nothing in that work (or since) broke previously-verified behavior.

`benchmarks/stress_test_engine.py` (Phase 12) required no changes and passed clean on this run -- see Results below.

`benchmarks/fintech_tail_latency.py` (Phase 14) **crashed immediately** on this run, before this phase's fix: `RuntimeError: AnimusBindings.spsc_init() must succeed before recording events`, thrown from the very first call inside `find_best_core()`'s core-probing step. Root cause, found via `git log -S` against `animus_engine.cpp` rather than guessed: `animus_pin_current_thread_to_core` / `animus_spsc_init` fail closed with no verified license and no unlicensed default (not even core 0) -- but that gate was added by a **later** commit (`2c9b9c1`, "offline RSA-signed hardware licensing for proprietary-edition") than the one that created `fintech_tail_latency.py` (`15b16a5`, Phase 14 itself). The script was never updated to call `animus_verify_license` and has no license-verification call anywhere in its history. The real, already-verified pinned numbers in Phase 14 above were captured before the licensing commit landed; every run of this script since then, on any process without an already-verified license, would have hit this same crash -- unrelated to Phase 24/25's soak-test work, which never touched this file, `animus.hpp`, `animus_engine.cpp`, or `animus/bindings.py`.

* **Fix:** `_verify_local_license()`, called in the parent process (before the core probe) and again in every `--run-sweep-spsc-pinned` child subprocess (license state is process-wide and does not cross a subprocess boundary), using the same gitignored, per-machine, regenerate-as-needed local test license convention `tests/test_bindings.py` and `benchmarks/soak_test_engine.py`'s Phase 25 work already established. Unlike Phase 25's graceful unpinned fallback, this raises rather than degrading silently -- this script's entire purpose is a pinned-vs-unpinned comparison, so silently running unpinned and reporting it as "pinned" would misrepresent every number in its output table, a worse outcome than failing loudly.

### Results

**`stress_test_engine.py`** (1,200,000-event sustained-load check + SDK validation + C-ABI fuzzing):

| Check | Result |
|---|---|
| RSS growth, warm baseline -> final | +0.08% (well under the 10% leak-flag threshold) |
| Threat signals matched and drained | 108,000 / 108,000 expected |
| SDK-level malformed-input cases (7) | All 7 safely rejected (in-process) |
| Raw C-ABI fuzz cases (6, subprocess-isolated) | 2/6 crashed (`count_exceeds_buffer_moderate`/`_extreme`) -- matches Phase 12's documented non-deterministic OOB-read behavior for a caller that bypasses the SDK; not a new issue |

No changes required; behavior matches Phase 12's original documentation exactly.

**`fintech_tail_latency.py`** (after the license-verification fix, 1,000,000 events per batch size/variant):

| Batch size | Variant | Throughput (ev/s) | p50 (us) | p90 (us) | p99 (us) | p99.99 (us) |
|---|---|---|---|---|---|---|
| 100 | Baseline (unpinned) | 6,885,876 | 14.00 | 15.00 | 21.40 | 275.30 |
| 100 | SPSC + pinned (core 6) | 6,976,585 | 13.40 | 13.80 | 18.50 | 1,320.13 |
| 1,000 | Baseline (unpinned) | 8,218,379 | 116.40 | 125.70 | 227.40 | 536.19 |
| 1,000 | SPSC + pinned (core 6) | 9,103,895 | 102.60 | 110.00 | 179.27 | 1,180.26 |
| 10,000 | Baseline (unpinned) | 7,166,250 | 1,342.95 | 1,590.07 | 1,802.61 | 2,165.73 |
| 10,000 | SPSC + pinned (core 6) | 7,242,871 | 1,286.65 | 1,450.78 | 3,240.30 | 4,318.31 |

Core probe selected core 6 again (38.64 us probe p99 vs. 46.58-77.19 us for cores 0/1/12/18/23), consistent with Phase 14's and Phase 25's finding that core 6 is repeatedly the fastest candidate on this machine.

* **Consistent with Phase 14's documented pattern, not a new finding.** p50 and p90 improved with pinning at every batch size (-4.2% to -12.5%), matching Phase 14's "improves consistently" result for the typical case. p99 improved for batch sizes 100 and 1,000 (-13.5%, -21.2%) but worsened for batch 10,000 (+79.8%) -- Phase 14's own 5-run range for that exact case was -8.9% to +78.7%, so this single run lands just outside (by ~1 point) a range built from 5 runs, not 1; not treated as evidence of anything new given that. p99.99 worsened at all three batch sizes (+379.5%, +120.1%, +99.4%), reproducing Phase 14's headline finding that thread pinning without OS-level core isolation does not reliably improve the extreme tail and can make it markedly worse -- two of these three deltas fall inside Phase 14's originally-documented 5-run ranges, and the third (batch 10,000, +99.4% vs. a previously observed max of +66.8%) is a single run exceeding a 5-run range, which is expected sampling variance for a metric Phase 14 already characterized as noisy, not a regression.
* **Status:** Phase 26 Regression Check Verified -- `stress_test_engine.py` required no changes and reproduced Phase 12 exactly; `fintech_tail_latency.py` had a real, pre-existing license-verification gap (unrelated to Phase 24/25) fixed and confirmed working, with its re-verified numbers matching Phase 14's documented pinned-vs-unpinned behavior (typical case improves, extreme tail does not reliably improve) rather than contradicting it.

## Phase 27: `fintech_tail_latency.py`, Second Re-Verification Run

### Target System

A second independent run of `benchmarks/fintech_tail_latency.py` (no code changes since Phase 26 -- this is a repeat run of the same fixed script, following up on Pilot_Kit work that touched unrelated files only), to check the license fix continues to hold and to accumulate a further data point against Phase 14's originally-documented 5-run ranges.

### Results (1,000,000 events per batch size/variant)

| Batch size | Variant | Throughput (ev/s) | p50 (us) | p90 (us) | p99 (us) | p99.99 (us) |
|---|---|---|---|---|---|---|
| 100 | Baseline (unpinned) | 6,786,899 | 13.80 | 15.60 | 30.60 | 242.33 |
| 100 | SPSC + pinned (core 6) | 6,643,622 | 13.50 | 15.50 | 26.91 | 1,481.72 |
| 1,000 | Baseline (unpinned) | 8,056,946 | 117.30 | 139.50 | 233.62 | 491.90 |
| 1,000 | SPSC + pinned (core 6) | 8,643,072 | 106.70 | 117.81 | 279.43 | 1,378.34 |
| 10,000 | Baseline (unpinned) | 6,853,244 | 1,406.70 | 1,588.01 | 2,209.81 | 2,524.42 |
| 10,000 | SPSC + pinned (core 6) | 7,129,474 | 1,308.75 | 1,529.80 | 3,282.95 | 3,669.99 |

Core probe again selected core 6 (37.48 us probe p99 vs. 45.42-75.39 us for the other five candidates) -- the fourth consecutive run (Phase 25, Phase 26, and now this one) to select core 6 on this machine.

* **Ran clean; license fix confirmed stable across a second independent run.** No crash, no regression in the fix itself.
* **A wider spread against Phase 14's 5-run ranges than Phase 26 alone showed, still within the same qualitative pattern.** Four of this run's twelve percentile deltas fall outside Phase 14's originally-documented 5-run ranges: p50 at batch 100 (-2.2% vs. a documented best-case range of -9.9% to -5.5%), p90 at batch 100 (-0.6% vs. -14.6% to -4.0%), p50 at batch 1,000 (-9.0% vs. -12.2% to -9.7%), and p99 at batch 1,000 (+19.6% vs. -40.0% to +17.1%). In every one of these four cases the deviation is in the "less improvement than previously observed" or "marginally worse than the previous worst case" direction, by single-digit-to-low-double-digit points, not a sign reversal or an order-of-magnitude jump -- p50 and p90 stayed negative (improved) at every batch size in this run, exactly as in every prior run. The remaining eight of twelve deltas (including all four at batch 10,000) land inside the original 5-run ranges.
* **Interpretation, not dismissal:** Phase 14's ranges were built from 5 runs; Phase 26 added a 6th; this is a 7th. It is expected, not suspicious, for a small-sample empirical range to occasionally not contain a later run's exact value -- that is what "range observed across 5 runs" means, as distinct from "guaranteed bound." The qualitative finding these three phases now jointly support across 7 total runs is unchanged from Phase 14's original conclusion: p50/p90 (typical case) improve with pinning far more often than not, while p99/p99.99 (tail) are genuinely noisy and can move against pinning's favor, consistent with pinning-without-OS-isolation's known inability to protect the rare case. Nothing here motivates widening the documented Phase 14 ranges themselves -- that would take a deliberate, dedicated multi-run study (as Phase 14 itself was), not incidental data points gathered while verifying something else.

## Phase 28: C++23 Telemetry Dispatch Benchmark Harness (Cross-Core SPSC, RDTSC-Resolution)

### Target System

`benchmarks/telemetry_benchmark.cpp` -- a self-contained C++23 harness built for institutional-grade proof points (HFT/execution client due diligence), independent of `animus.hpp`'s existing `SpscRingBuffer<T>`: its own compile-time-sized `SpscRingBuffer<T, Capacity>` template (`std::array`-backed, no heap allocation ever, not even at construction) and a 64-byte, cache-line-aligned `TelemetryEvent`. One producer thread and one consumer thread, pinned to separate physical cores, exchange events through the ring; timestamps are lfence-serialized RDTSC reads calibrated against `std::chrono::steady_clock` (a measured, not assumed, cycle rate), giving sub-100ns resolution the `QueryPerformanceCounter`-quantized figures elsewhere in this document structurally cannot show.

### Method

10,000,000 total measured events (plus 1,000,000 unmeasured warm-up through the identical path), split into two purpose-built phases rather than one flooded run:

* **Latency phase** (1,000,000 events): the producer waits for the consumer's receipt acknowledgment before dispatching the next event (in-flight depth bounded to 1), so each sample is the actual enqueue-to-receipt transport cost, not queueing delay.
* **Throughput phase** (9,000,000 events): unthrottled flood at maximum sustained rate.

Real MSVC (`cl /std:c++latest /EHsc /O2`) and MinGW GCC 15.2 (`g++ -std=c++23 -O3 -pthread -lstdc++exp`) builds, both run for real and cross-checked against each other for consistency, not merely compiled. 3 consecutive runs.

### Results

**Latency (depth-1 phase, producer -> consumer, cross-core):**

| Percentile | Latency (representative run) | Range across 3 runs |
|---|---|---|
| min | 45.5 ns | 39.7 - 45.5 ns |
| p50 | 53.3 ns | 52.5 - 53.3 ns |
| p90 | 57.9 ns | 56.6 - 57.9 ns |
| p99 | 64.9 ns | 62.4 - 64.9 ns |
| p99.9 | 111.6 ns | 88.5 - 113.7 ns |

**Throughput (unthrottled flood phase):**

| Metric | Representative run | Range across 3 runs |
|---|---|---|
| Sustained throughput | 47.306 M msgs/sec | 47.306 - 49.497 M msgs/sec |

* **A real methodology bug found and fixed before these numbers were recorded, not after:** the first version of this harness measured latency and throughput in one unthrottled flooded run -- producer floods the ring, consumer drains continuously, no flow control. That reported a p50 of ~354,000 ns (354 microseconds), because the ring saturates and stays near-full under sustained flooding, so a "latency" sample there is mostly queueing delay behind a deep backlog, not transport cost -- a real number, but not the one an HFT client asking "how fast is one event" wants, and indistinguishable from a genuine transport regression without knowing the cause. Fixed by splitting into the two-phase design above; re-measured, the same pipeline reports p50 53ns, not 354,000ns -- the fix, not the transport, explains the three-order-of-magnitude difference.
* **A real build portability finding:** `<print>`/`std::println` compiles cleanly under MinGW GCC 15.2 but fails to *link* (`undefined reference to std::__open_terminal` / `std::__write_to_terminal`) without `-lstdc++exp` -- libstdc++ ships `<print>`'s terminal-writing backend in a separate "experimental" static library until that TS support stabilizes. Found by isolating the link step and inspecting the raw `ld` invocation after `collect2` swallowed the real error behind a generic "ld returned 1 exit status." `CMakeLists.txt`'s GNU-compiler branch links `stdc++exp` accordingly.
* **A real toolchain finding:** this machine's MSVC (19.51, a pre-release "Visual Studio 2026" toolset) does not yet expose a literal `/std:c++23` flag -- only `c++14|c++17|c++20|c++latest` -- despite already shipping the `<print>`/`<stacktrace>` headers. `/std:c++latest` is what actually compiles this file under MSVC today; recorded directly in the build commands below rather than left as a surprise for whoever runs this next.
* **How this compares to the other benchmark layers in this document:** SPSC (one producer, one consumer, zero compare-exchange contention) with a consumer actively draining concurrently is not directly comparable to Phase 14's 8-producer MPMC contention numbers (different contention model, no consumer draining there) or to any single-threaded, no-cross-core-hop figure elsewhere in this document (different workload -- no actual transport happening at all). See `BENCHMARK_DATASHEET.md`'s "Cross-core SPSC dispatch latency" section for the client-facing summary of these same numbers with that methodology caveat spelled out.
* **Build commands:**

  ```
  # MSVC (x64 Native Tools Prompt)
  cl /std:c++latest /EHsc /O2 /DNDEBUG /Fe:telemetry_benchmark.exe benchmarks\telemetry_benchmark.cpp

  # GCC/Clang + libstdc++ -- needs -lstdc++exp for <print>'s terminal-I/O backend
  g++ -std=c++23 -O3 -DNDEBUG -pthread -o telemetry_benchmark benchmarks/telemetry_benchmark.cpp -lstdc++exp

  # CMake (target added to the root CMakeLists.txt; own cxx_std_23, doesn't touch animus_native's C++17)
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
  cmake --build build --target telemetry_benchmark
  ```

* **Status:** Phase 28 C++23 Telemetry Dispatch Benchmark Harness Verified

## Phase 29: Cross-Process Shared-Memory Producer -- Real Linux CI Verification (`benchmarks/harness_benchmark.cpp`, `eval_kit/`)

### Target System

`benchmarks/harness_benchmark.cpp` -- the C++ producer half of the client-facing `eval_kit/` evaluation package, exercising `animus::sys::ipc::ShmRing<animus::ExecutionEvent>` (Phase 20's `ShmRing<T>` primitive, `include/animus/shm_ipc.hpp`) against a genuine second OS process (`eval_kit/scripts/verify_stream.py`'s nanobind consumer) over real POSIX `/dev/shm`, rather than the two-process-on-one-Windows-machine setup Phase 20 itself used. Run via `.github/workflows/eval_kit_packaging.yml` on a real `ubuntu-22.04` GitHub Actions runner -- [run 33806378357](https://github.com/alakshendra-roy/AnimusCore_v1/actions/runs/33806378357) -- not a container claiming Linux compatibility, not cross-compiled, not emulated. This is the first time this specific header's POSIX code path had ever actually executed, on any machine, in this repository's history; every prior verification of `ShmRing<T>` (Phase 20 above) ran the Windows branch (`CreateFileMappingA`/`OpenFileMappingA`) exclusively.

### Method

10,000,000 synthetic `ExecutionEvent` records, decoupled/non-blocking overwrite mode (`--mode overwrite`) -- the producer never waits on a consumer, so it completes and reports numbers with no consumer attached at all, a direct demonstration of the "producer must not block the core execution thread" requirement `ShmRing::push_overwrite()` exists to satisfy. Built `-O3 -march=x86-64-v3` (Ubuntu clang 14.0.0), `eval_kit/scripts/package_kit.sh` probing for that instruction-set level rather than assuming it, with `-march=native` as a documented fallback for older compilers. Per-event enqueue latency is a serialized RDTSC read (`_mm_lfence()` before and after `__rdtsc()`) immediately before and after each `push_overwrite()` call, converted to nanoseconds via a 200ms wall-clock calibration performed once at process startup -- same methodology as Phase 20 and Phase 28 above, applied to this transport for the first time on real Linux.

### Results

**Per-event enqueue latency (decoupled overwrite mode):**

| Percentile | Latency |
|---|---|
| min | 20.0 ns |
| p50 | 30.3 ns |
| p90 | 40.1 ns |
| p99 | 40.1 ns |
| p99.9 | 2,695.2 ns |
| max | 32,470.5 ns |

**Throughput:**

| Metric | Result |
|---|---|
| Sustained throughput | 13.856 M events/sec |
| Wall time | 0.722 s (10,000,000 events) |

* **A real bug this exact run caught and fixed -- the whole reason this verification was worth doing, not a formality:** `include/animus/shm_ipc.hpp`'s `SharedMemoryRegion::open()` (the consumer/attach side, POSIX branch) called the raw `::open(name, O_RDWR)` syscall instead of `shm_open(name, O_RDWR, 0600)`. `::open()` on a bare name like `"animus_eval_demo_1916"` performs an ordinary filesystem lookup relative to the current working directory; the segment `create()`'s own (correct) `shm_open()` call actually put there resolves through the shared-memory namespace to `/dev/shm/animus_eval_demo_1916` instead. Every C++ or nanobind consumer calling `ShmRing::open()` on Linux has been silently unable to find a real segment since this file was written -- invisible until now because every earlier POSIX-adjacent test used either the Windows branch (unaffected -- `OpenFileMappingA` has no equivalent bug) or `benchmarks/consumer.py`'s pure-Python `multiprocessing.shared_memory` path, which performs its own correct `shm_open` internally and never touches this C++ code at all. First exposed as `eval_kit/scripts/verify_stream.py` failing with `ShmRing::open('...') failed -- no such segment` immediately after a successful producer run on the CI runner; fixed in one line, re-verified on the same CI job (the results above are from the passing re-run).
* **Verified two independent ways, not just "the job went green":** (1) the CI run's own inspection steps confirmed `bin/harness_benchmark` is a genuine `ELF 64-bit LSB pie executable, x86-64, ... for GNU/Linux` (not a stray `.exe` or a misidentified artifact) and that the packaged wheel carries a real `cp310-cp310-linux_x86_64` tag, not a Windows one; (2) independently, downloading the produced tarball after the fact (`gh run download 33806378357`) and recomputing its SHA-256 locally reproduced the exact checksum CI itself reported byte-for-byte: `24f0902f0b28eda5d34ed05cd686765ff39b3bca02ff5a35c1c12675aa1ba1fd`.
* **A second, independent bug this same verification effort caught, in the consumer's own self-check, not the transport:** `eval_kit/scripts/verify_stream.py` originally reported `Gaps == dropped_count? NO -- investigate` on this exact run despite it being completely correct -- its gap-counting only looked at spans between records it actually received, never the block of records dropped *before* the very first one it ever saw. Since `run_demo.sh`'s own documented flow runs the producer to completion in overwrite mode before the consumer ever attaches, every drop in a 10,000,000-event overwrite run precedes the first record the consumer observes, so the original logic reported zero gaps against a real `dropped_count` of 8,951,424. Fixed by seeding the gap-tracking sequence counter at -1 instead of `None` (valid because this producer always numbers events starting at sequence 0); re-verified on Linux showing `Sequence gaps seen: 8,951,424` exactly matching `Producer dropped_count: 8,951,424`.
* **Also present-tense pip/toolchain finding, unrelated to this repo's own code:** a stock `ubuntu-22.04` runner's apt-installed system `pip` (22.0.2, still on the long-deprecated vendored `pep517` build backend) failed to resolve a mutually-compatible `scikit-build-core`/`packaging` combination when building the nanobind wheel, raising `AttributeError: module 'packaging.utils' has no attribute 'InvalidName'` from deep inside `scikit-build-core`'s vendored metadata parser -- a pip-version compatibility issue, not a defect in either `pyproject.toml` in this repository. Fixed in `eval_kit/scripts/package_kit.sh` by building both wheels inside a throwaway venv with a freshly upgraded pip rather than trusting the packaging machine's system pip.
* **Build/run commands:**

  ```bash
  # Via the eval kit (builds the binary + both wheels, runs the full demo):
  bash eval_kit/scripts/package_kit.sh
  cd eval_kit/dist/animus-eval-kit-linux-x86_64 && ./run_demo.sh

  # Or directly, with a pre-built toolchain:
  g++ -std=c++17 -O3 -march=x86-64-v3 -Iinclude benchmarks/harness_benchmark.cpp -o harness_benchmark -lpthread
  ./harness_benchmark --events 10000000 --mode overwrite
  ```

* **Status:** Phase 29 Cross-Process Shared-Memory Producer, Real Linux CI Verified -- genuine `ubuntu-22.04` ELF binary and `linux_x86_64` wheel confirmed, tarball SHA-256 independently reproduced locally, and two real bugs (one in `shm_ipc.hpp`'s POSIX attach path, one in the eval kit's own consumer self-check) caught and fixed by this exact verification effort, not left latent.
* **Status:** Phase 27 Re-Verification Run Recorded -- `fintech_tail_latency.py` remains fixed and functional; this run's numbers extend, rather than contradict, the pinned-vs-unpinned behavior already characterized in Phase 14/26.