# Animus Core — InfoSec & Risk Committee Memo

**Classification:** Institutional Technical Due-Diligence Document
**Audience:** InfoSec, Security Architecture, and Risk committees evaluating Animus Core for deployment inside a regulated trading environment.
**Scope:** The compiled engine (`AnimusCore_v1/animus.hpp` → `AnimusNative.dll`/`.so`, built via `bindings/CMakeLists.txt`) and the C-ABI/Python SDK surface on top of it (`animus/`, `sdk/python/`). This memo does **not** cover repository-internal developer or marketing tooling (e.g. `telemetry_bridge.py`, `docs/dashboard.html`) — see §6 for why that distinction matters and how to verify it yourself.
**Companion documents:** [`ARCHITECTURE.md`](ARCHITECTURE.md) (ingestion pipeline deep-dive) · [`PILOT_EVAL_CHECKLIST.md`](PILOT_EVAL_CHECKLIST.md) (hands-on host qualification) · [`COMPATIBILITY_TUNING_GUIDE.md`](COMPATIBILITY_TUNING_GUIDE.md) (compiler/kernel/Python bridge reference) · [`../../COMPLIANCE_AND_RISK_MITIGATION.md`](../../COMPLIANCE_AND_RISK_MITIGATION.md) (contractual SLA, liability, and legal terms — this memo is technical/architectural, not a legal representation).

Every claim below cites the exact file and line range it is drawn from, and §6 gives the grep/build commands to reproduce the verification independently rather than take it on faith. Where a claim has a boundary or an exception, that boundary is stated explicitly — a security memo that hides its own scope is worse than none.

---

## 1. Network Egress Posture

**The core ingestion/ring-buffer hot path makes zero network calls.** `include/animus/shm_ipc.hpp` (the cross-process SPSC/SPMC ring transport), `include/animus/telemetry.hpp` (the diagnostic sampler), and `AnimusCore_v1/animus.hpp` (the core single-header engine, including `LockFreeRingBuffer<T>` and `SpscRingBuffer<T>`) contain no `socket()`, `connect()`, `WSAStartup`, or HTTP/TLS client code of any kind. All producer↔consumer data movement is local: Windows named file mappings (`CreateFileMappingA`/`MapViewOfFile`) or POSIX `shm_open`/`mmap` — memory shared between processes on the *same host*, never a wire protocol to another host.

**There is one opt-in exception, and it is scoped deliberately.** `AnimusCore_v1/animus_transport.hpp` and `AnimusCore_v1/animus_cluster.hpp` implement a Windows-only, mutually-authenticated TLS 1.3 transport (OS-native Schannel/SSPI — `secur32.lib`/`crypt32.lib`/`ws2tcpip.h`, not a third-party TLS library) for **customer-operated multi-node clustering** — replicating rule-engine control-plane state (not telemetry payloads) across nodes the customer deploys and owns, via a hand-rolled Raft-lite consensus protocol. Key points for a risk assessment:

- A single-node deployment — the default, and what a Desk License customer runs unless clustering is explicitly configured — never links or loads this module at all.
- The transport requires a verified mutual TLS client certificate before any RBAC/tenant identity is trusted (`animus_transport.hpp`, `CertificateIdentityMap`); it is not an open listener.
- It connects only to peer addresses the customer configures. There is no hardcoded hostname, IP, or Animus-operated endpoint anywhere in the codebase — verified by grep, see §6.
- If your deployment model is single-node (the common case for a desk evaluating latency-sensitive ingest), this module is out of scope entirely: do not build/link `animus_transport.hpp`/`animus_cluster.hpp`, and the air-gap claim in §5 applies without qualification.

## 2. Third-Party Telemetry / Phone-Home

No analytics SDK, crash reporter, update checker, or license-activation network call exists anywhere in the engine, the C-ABI bindings, or the Python SDK. License validation (`animus_verify_license`/`animus_check_license_status` in `animus_engine.cpp`, checked against the embedded **RSA-2048** public key in `AnimusCore_v1/animus_license_pubkey.hpp`) is **offline signature verification** — the engine checks a locally-provided license file's RSA signature; it does not call out to any license-activation server to do so. Nothing in this codebase reports usage, telemetry payloads, or diagnostic data to Animus Core Systems or any other third party. This was verified by grepping every `.hpp`/`.cpp` file reachable from the core engine build target for socket/HTTP-client symbols and hardcoded domains (§6) — the only matches are the customer-controlled cluster transport described in §1.

## 3. Memory Allocation & Bounded Buffers

Every ring-buffer type the hot path uses allocates its backing storage **once**, at construction/open time, from a caller-supplied or negotiated capacity — never per-event:

- `animus::SpscRingBuffer<T>` and `animus::LockFreeRingBuffer<T>` (`AnimusCore_v1/animus.hpp:481`, `:369`) size a `std::vector<T>`/`std::vector<Cell>` once in the constructor.
- `sys::ipc::ShmRing<T>` / `SpmcRing<T>` (`include/animus/shm_ipc.hpp`) placement-`new` their header into an already-mapped, fixed-size OS shared-memory segment (`:410`, `:869`) — the "allocation" is a page mapping negotiated at `create()`/`open()`, not a heap call.

**`push()`, `try_pop()`, `pop()`, `push_spin()`, `pop_spin()`, and `push_overwrite()` perform zero heap allocations** — this is enforced, not just asserted: `bench/hotpath_bench.cpp`, `animus_sandbox/soak_harness.cpp`, and this repository's own `poc_eval/poc_eval_harness.cpp` (§6) override the global `operator new`/`operator delete` and abort/flag the run if either fires while the timed hot-path region is armed. Run `run_poc_eval.sh`/`run_poc_eval.bat` yourself and check the "heap allocations during timed region" line in the scorecard — that number is measured on your own hardware, not asserted from the source.

Ring capacity is bounded and fixed for the lifetime of the ring (`capacity_ = round_up_pow2(requested_capacity)`, immutable after construction). A full ring under backpressure mode (`try_push`) simply refuses the push (returns `false`) rather than growing; under overwrite mode (`push_overwrite`) it reclaims the oldest slot rather than allocating a new one. Neither path can cause unbounded memory growth.

## 4. Thread-Safety Invariants

Two distinct concurrency contracts exist in this codebase, and conflating them is the single most common way to misuse it — so both are stated precisely:

| Type | Contract | Enforcement |
|---|---|---|
| `animus::SpscRingBuffer<T>` | Exactly **one** producer thread, exactly **one** consumer thread, for the life of the ring. | **Not runtime-checked.** A debug-only thread-identity check would cost real overhead on the exact path this class exists to make fast (`animus.hpp:471`); violating the contract is undefined behavior, not a caught error. Callers integrating this type must enforce the one-producer/one-consumer invariant at the architecture level. |
| `animus::LockFreeRingBuffer<T>` (Vyukov MPMC) | Any number of concurrent **producer** threads; documented for exactly one consumer thread in this codebase's usage (`animus.hpp:364`). | CAS-retry loop on `enqueue_pos_`/`dequeue_pos_` makes concurrent producers safe by construction — no external locking required. |
| `sys::ipc::ShmRing<T>` (cross-process SPSC) | One producer process, one consumer process. `push_overwrite()` is documented to allow a **torn read** at the reclaim boundary when the ring is full and overwriting a slot the consumer may be mid-`try_pop()` on (`shm_ipc.hpp:516`–`527`) — a deliberate, counted (`dropped_count`) tradeoff for "producer must never block," not an oversight. | Pair `push_overwrite()` only with a consumer that tolerates occasional torn records (e.g. best-effort telemetry sampling). A channel requiring clean records under a lagging consumer must use `try_push()`/`push_spin()` (bounded backpressure) instead. |
| `sys::ipc::SpmcRing<T>` (cross-process broadcast) | One producer process, **N** consumer processes, each with its own local read cursor (no shared consumer state, so no cross-consumer contention). | Consumer lag/overrun is a per-consumer concept, not observable from the ring itself by an unattached sampler (`telemetry.hpp:74`–`93`). |

`AnimusGetMetrics()` (`include/animus/telemetry.hpp`) is the one component explicitly designed to be safe to call from an unbounded number of concurrent observers without contending with the producer or consumer: it never attaches as a consumer, uses only `std::memory_order_relaxed` loads of values the producer/consumer already publish via `memory_order_release`, and takes no lock.

## 5. Air-Gapped Deployment

For the common single-node deployment (no `animus_transport.hpp`/`animus_cluster.hpp` linked): the compiled engine requires **no outbound firewall rule**. Every code path it exercises is local — CPU registers/cache, mapped memory, and (for cross-process delivery) OS shared-memory primitives. It performs no DNS resolution, opens no listening socket, and writes to no location outside the shared-memory segment(s) it creates/attaches and whatever log file path the host application configures. A host with all outbound network access blocked at the OS or hypervisor level will run the engine identically to one with unrestricted egress — network access is not on any success path.

## 6. Verification — Reproduce Every Claim Above Yourself

Do not take §1–§5 on faith. Every claim is independently checkable from a clean clone:

```bash
# §1/§2 — zero network calls in the core engine (expect: no matches, or only
# comments/CPU-socket references, in animus.hpp / include/animus/*.hpp):
grep -rniE "socket\(|connect\(|WSAStartup|curl|CURLOPT|http://|https://" \
    AnimusCore_v1/animus.hpp include/animus/

# §1 — confirm the ONLY networking code in the core module set is the
# customer-controlled cluster transport, and that it has no hardcoded host:
grep -rniE "socket\(|connect\(|WSAStartup" AnimusCore_v1/animus_release.hpp \
    AnimusCore_v1/animus_transport.hpp AnimusCore_v1/animus_cluster.hpp
grep -rnEo "\"[a-zA-Z0-9.-]+\.(com|io|net|org)\"" AnimusCore_v1/*.hpp AnimusCore_v1/*.cpp include/animus/*.hpp
# (expect zero results for the second command)

# §3 — zero heap allocations on the hot path, measured on YOUR hardware:
./run_poc_eval.sh 10        # Linux
run_poc_eval.bat 10         # Windows
# check the "heap allocations during timed region" / "TARGET VALIDATION" lines

# §4 — read the thread-safety contracts directly at their cited line numbers:
sed -n '460,532p' AnimusCore_v1/animus.hpp
sed -n '500,610p' include/animus/shm_ipc.hpp
```

If any of the above turns up a result this memo doesn't account for, that is a finding — please raise it; the intent of this document is to be falsifiable, not persuasive.

## 7. Out of Scope (Explicitly)

The following exist in this repository but are **not** part of the shipped engine/SDK and are excluded from every claim above:

- `telemetry_bridge.py` / `command_center_bridge.py` — developer-run WebSocket bridges that feed local HTML dashboards (`docs/dashboard.html`, a private, gitignored command-center page) for demo/marketing purposes. They bind to `127.0.0.1` by default and are not linked into, shipped with, or required by `AnimusNative.dll`/`.so` or the Python SDK.
- Any `.html`/marketing artifact in the repository root or `docs/marketing_whitepapers/`.

A reviewer auditing the *product* should scope their review to the files named in this memo's header; a reviewer auditing the *repository* should treat everything else as out-of-band developer tooling, subject to a separate, lower bar than the shipped engine.

---

*This memo describes architecture as implemented in the code cited above. It is a technical representation, not a contractual one — see [`COMPLIANCE_AND_RISK_MITIGATION.md`](../../COMPLIANCE_AND_RISK_MITIGATION.md) for SLA/liability terms and which performance/security figures a specific order form makes contractually binding.*
