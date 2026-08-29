# Animus Core v1.0 -- Client Quickstart Guides

Four proof-of-concept quickstarts, one per way of consuming Animus Core.
Pick the one that matches your integration:

| Guide | For | Platform |
|---|---|---|
| [1. Python SDK](#1-python-sdk-pip-install) | Orchestrators, SOAR pipelines, scripting | Any (Windows/Linux/macOS) |
| [2. C++ single header](#2-c-single-header-embedded--in-process) | Embedding directly in a C++17 service, ultra-low-latency execution paths | Any (portable subset); Windows + MSVC for the full feature set |
| [3. Secure multi-tenant + mTLS](#3-secure-multi-tenant--mtls-transport) | Multiple isolated tenants, encrypted remote ingestion | Windows + MSVC |
| [4. Distributed cluster](#4-distributed-raft-lite-cluster) | High-availability, multi-node rule replication | Windows + MSVC |

All four PoCs below are real, runnable code paths already exercised in this
repo's own verification demos (see `AnimusCore_v1/BENCHMARKS.md` for the
measured numbers) -- not illustrative pseudocode.

---

## 1. Python SDK (`pip install`)

Zero third-party Python dependencies (see `CLAUDE.md`) -- everything the
`animus` package imports is Python 3.8+ stdlib.

```bash
pip install animus-core
# or, from a source checkout:
pip install -e .
```

```python
import animus

# 1. Bring up the native engine (a lock-free ring buffer + rule engine
#    running in-process, no IPC).
engine = animus.AnimusBindings()
engine.init(buffer_capacity=1 << 16)

# 2. Register a declarative threshold rule: fire when event_id=500's
#    metric_value exceeds 100.
engine.add_rule(
    rule_id=1,
    event_id=500,
    threshold=100,
    comparator=animus.RuleComparator.GREATER_THAN,
    severity=5,
)

# 3. Ingest telemetry -- never blocks; returns False if the ring is full.
engine.record_event(event_id=500, trace_id=1, metric_value=150)

# 4. Drain matched signals (a background persistence worker evaluates rules
#    as events are logged -- see start_logging() below).
engine.start_logging("telemetry.log")
import time; time.sleep(0.05)  # let the worker catch up
for signal in engine.poll_signals(max_count=32):
    print(f"MATCH rule={signal.rule_id} event={signal.event_id} value={signal.metric_value}")
engine.stop_logging()
```

**Complex Event Processing (CEP): sliding-window rules, not just one-event
thresholds.** `add_rule()` above evaluates each event on its own. Real
detection logic is often about a *window* of recent events instead --
"the sum of the last 100 orders exceeds X," "the average latency over the
last 5 seconds is above Y." `add_cep_rule()` registers that directly,
evaluated entirely in C++ on the native hot path (Python only registers
the rule; every per-event window update and aggregate check happens
in `animus.hpp`, never crossing back into Python):

```python
# Fire when the SUM of the last 3 event_id=42 values exceeds 50.
engine.add_cep_rule(
    rule_id=2,
    event_id=42,
    window_type=animus.WindowType.COUNT,   # or animus.WindowType.TIME (window_size = ms)
    window_size=3,                          # last 3 matching events
    aggregation=animus.AggregationFunction.SUM,  # or AVG / MIN / MAX
    comparator=animus.RuleComparator.GREATER_THAN,
    threshold=50,
    severity=5,
)
for i, value in enumerate([10, 20, 30, 5, 1, 100]):
    engine.record_event(event_id=42, trace_id=i, metric_value=value)
# ... start_logging()/poll_signals() as above -- matches fire at trace_id
# 2 (running sum=60), 3 (sum=55), and 5 (sum=106). signal.metric_value
# carries the window's aggregated value, not the triggering event's raw
# metric_value.
```

`WindowType.COUNT` windows hold the last N matching events; `WindowType.TIME`
windows (`window_size` in milliseconds) hold events from the last N ms,
evaluated fresh on every new matching event either way. `AggregationFunction.AVG`'s
threshold check is exact integer arithmetic internally (cross-multiplied,
not divided) so it never disagrees with what the true average would say at
a boundary -- see `AnimusCore_v1/BENCHMARKS.md`'s Phase 15 section for the
verification this held up to (1,608 trial sequences against a naive
brute-force reference, plus a regression test for that exact boundary
case) and measured evaluation overhead at higher rule counts.

**High-volume ingestion with `record_events_batch`:**

`record_event()` crosses the ctypes/C-ABI boundary once per call -- fine at
low rates, but at high event counts that per-call marshalling cost (not
native execution time) dominates and can make calling into the native
engine from Python *slower* than an equivalent pure-Python loop. If you're
ingesting events in bursts (a batch pulled off a queue, a bulk import, a
replay), push the whole batch in one call instead:

```python
events = [(event_id, trace_id, metric_value) for ...]  # e.g. 10k-100k events
pushed = engine.record_events_batch(events)
# Returns the number actually pushed -- fewer than len(events) if the ring
# buffer fills partway through (never blocks, same contract as record_event).
assert pushed == len(events)
```

Measured at 100,000 events: `record_events_batch` ran ~2x pure-Python's
throughput and ~7x `record_event`'s per-event ingestion path -- see
`AnimusCore_v1/BENCHMARKS.md`'s Phase 11 section for the full breakdown,
including why the naive fix (building the batch as one `ctypes.Structure`
object per event) only closed a small fraction of that gap on its own.

**Is it safe to lean on for sustained, high-volume ingestion?**
`benchmarks/stress_test_engine.py` pushes 1,200,000+ events through the
full pipeline (`record_events_batch` -> rule evaluation -> disk
persistence) while sampling this process's memory, and separately feeds
malformed input across the C-ABI boundary `record_events_batch` sits on
top of:

```bash
python benchmarks/stress_test_engine.py
```

Across 5 consecutive runs: RSS growth stayed in a 1.82-2.49% band after
warm-up (no leak), and every malformed-*data* case (an out-of-range field,
a wrong-arity event tuple) was safely rejected by `struct.pack()` before
it could reach native code -- because the public `record_events_batch()`
you call always derives `count` from `len(events)` and sizes its buffer to
match in the same call, the underlying C-ABI's real edge case (a `count`
argument that lies about the buffer's actual size, which the native side
cannot itself detect) is only reachable by bypassing this API entirely.
See `AnimusCore_v1/BENCHMARKS.md`'s Phase 12 section for the full
breakdown, including that boundary-fuzzing result.

**What does the tail actually look like, not just the average?**
If you're calling `record_events_batch()` from a latency-sensitive path (an
order-ingestion or risk-check hot path, not just a bulk import),
`benchmarks/fintech_tail_latency.py` times the call itself -- not batch
construction -- call-by-call across 1,000,000 events at batch sizes of
100 / 1,000 / 10,000, reporting p50 through p99.99:

```bash
python benchmarks/fintech_tail_latency.py
```

Representative numbers (5-run ranges in `AnimusCore_v1/BENCHMARKS.md`'s
Phase 13 section): at batch size 100, p50 is ~13.7 us but p99.99 is
~385 us -- a ~28x tail. At batch size 10,000, p50 is ~1,373 us and
p99.99 ~1,959 us -- only ~1.4x. Smaller batches are dominated by fixed
per-call jitter (OS scheduling, Python-level GC, allocator stalls) that
their small amount of real work barely amortizes; if you're choosing a
batch size for a latency-sensitive path, larger batches trade higher
absolute latency for a *tighter* tail relative to their own median, not
just higher throughput.

**Going further: a lock-free SPSC ring + CPU pinning, for one dedicated
hot-path thread.** `record_events_batch()` above targets the general-purpose
MPMC ring (safe to call from multiple producer threads at once). If you
have exactly one producer thread doing all your ingestion, a standalone
single-producer/single-consumer channel plus pinning that thread to one
CPU core is available too:

```python
core_count = engine.get_cpu_count()
engine.pin_current_thread_to_core(core_count - 1)  # see the caveat below first
engine.spsc_init(buffer_capacity=1_000_000)

pushed = engine.spsc_record_events_batch(events)   # producer thread only
records = engine.spsc_drain(max_count=1024)        # consumer thread only
```

Single-producer, single-consumer *only* -- calling `spsc_record_events_batch`
from more than one thread concurrently is undefined behavior, not merely
wrong, the same way it would be in the C++ template underneath
(`animus::SpscRingBuffer`). These are native performance primitives with no
pure-Python fallback: they raise `RuntimeError` if no compiled binary is
loaded, rather than silently degrading to something that wouldn't measure
the same thing.

**The pinning caveat, stated plainly because the numbers don't round the
way you'd expect:** don't just pin to `core_count - 1`. `benchmarks/
fintech_tail_latency.py` did exactly that first and got tail latency up to
34.5x *worse* -- on a hybrid CPU (P-cores + E-cores, common on recent
laptops/desktops), the highest-numbered core is not reliably a fast one,
and there's no portable way to ask the OS which is which. The benchmark now
probes a handful of candidate cores with a cheap workload and picks
whichever measures fastest before pinning for real -- do the same rather
than hardcoding a core number. Even with a good core, measured across 5
runs: throughput and p50/p90 latency improve consistently (real gains,
every run), but **p99.99 tail latency does not reliably improve, and
often gets worse** -- thread affinity alone doesn't reserve a core
exclusively, so a pinned thread has nowhere to go on the rare occasion its
one core is briefly needed elsewhere, while an unpinned thread can migrate
away. See `AnimusCore_v1/BENCHMARKS.md`'s Phase 14 section for the full
numbers, including the exact ratio of runs where each percentile improved
vs. got worse.

**Instrumenting existing code with `@animus.trace`:**

```python
@animus.trace(event_id=42)
def call_downstream_api():
    ...  # each call is recorded as one telemetry event (duration in ns)
```

**Cross-process telemetry (no serialization step):**

```python
from animus import SharedTelemetryRing
ring = SharedTelemetryRing.create("my-shared-segment", capacity=65536)
# ... hand the segment name to a second process, which attaches with
#     SharedTelemetryRing.attach("my-shared-segment") and reads zero-copy.
```

See `AnimusCore_v1/soar_orchestrator.py` for a full automated-response
pipeline built on this, and `AnimusCore_v1/shm_ipc_demo.py` for the
cross-process ring in action.

---

## 2. C++ single header (embedded / in-process)

For an embedded or ultra-low-latency deployment, skip Python and ctypes
entirely: `animus_release.hpp` is one self-contained file -- drop it into
your project and `#include` it, no build step, no linking against a
separate `.dll`/`.so`.

```cpp
#include "animus_release.hpp"

int main() {
    auto engine = animus::Engine::Create(1 << 16);

    engine->add_rule(
        /*rule_id=*/1, /*event_id=*/500, /*threshold=*/100,
        static_cast<uint8_t>(animus::RuleComparator::GreaterThan),
        /*severity=*/5);

    engine->record(/*event_id=*/500, /*trace_id=*/1, /*value=*/150);

    engine->start_persistence("telemetry.log");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    animus::ThreatSignal sig{};
    if (engine->poll_signals(&sig, 1) == 1) {
        std::printf("MATCH rule=%u event=%u value=%llu\n",
            sig.rule_id, sig.event_id, (unsigned long long)sig.metric_value);
    }
    engine->stop_persistence();
}
```

**Complex Event Processing (CEP): sliding-window rules.**
`Engine::add_cep_rule` is a normal virtual method, same as `add_rule` --
fully header-only, no C-ABI, no ctypes involved (unlike the Python SDK's
`add_cep_rule`, there's no marshalling boundary to think about here, only
the rule definition itself). Evaluated on the persistence worker thread
alongside plain `RuleThreshold` rules, delivered through the same
`poll_signals()` queue:

```cpp
// Fire when the SUM of the last 3 event_id=42 values exceeds 50.
engine->add_cep_rule(
    /*rule_id=*/2, /*event_id=*/42,
    /*window_type=*/0 /*Count*/, /*window_size=*/3,
    /*aggregation=*/0 /*Sum*/, /*comparator=*/0 /*GreaterThan*/,
    /*threshold=*/50, /*severity=*/5);

for (uint64_t i = 0; i < 6; ++i) {
    static const uint64_t values[] = {10, 20, 30, 5, 1, 100};
    engine->record(/*event_id=*/42, /*trace_id=*/(uint32_t)i, values[i]);
}
// ... start_persistence()/poll_signals() as above -- matches fire at
// trace_id 2 (running sum=60), 3 (sum=55), and 5 (sum=106). sig.metric_value
// carries the window's aggregated value, not the triggering event's raw
// metric_value.
```

`window_type`/`aggregation`/`comparator` use the same integer encodings as
the Python SDK's `WindowType`/`AggregationFunction`/`RuleComparator`
(0/1 for Count/Time, 0-3 for Sum/Avg/Min/Max, 0-2 for GreaterThan/LessThan/
Equal) -- see `animus::WindowType`/`animus::AggregationFunction` in
`animus.hpp` if you'd rather spell them out than remember the numbers.
`AggregationFunction::Avg`'s threshold check is exact integer arithmetic
(cross-multiplied, not divided), verified against a naive brute-force
reference over 1,608 trial sequences before this went into the engine --
see `AnimusCore_v1/BENCHMARKS.md`'s Phase 15 section for that verification
and measured evaluation overhead at higher rule counts.

**Batched ingestion (`Engine::record_batch`):** if you already have a run
of events available at once -- read off a queue, replayed from a file,
received as one network payload -- push them in a single call instead of
looping over `record()`:

```cpp
std::vector<animus::RawEvent> events;
events.push_back(animus::RawEvent{/*event_id=*/500, /*trace_id=*/1, /*metric_value=*/150});
// ... fill in the rest of the batch ...

size_t pushed = engine->record_batch(events.data(), events.size());
// Returns the number actually pushed -- fewer than events.size() if the
// ring buffer fills partway through (never blocks, same contract as
// record()). Stops at the first failed push rather than skipping ahead,
// so `pushed` also tells you exactly how many of `events`, in order,
// made it in.
```

Unlike the Python SDK's `record_events_batch` (guide 1), there's no
ctypes/C-ABI boundary here to amortize -- this guide's whole point is that
`record()` already avoids that cost. `record_batch()` still saves N-1
virtual-dispatch calls into `Engine` and is convenient when your data is
already batched, but at this call site it isn't the ~2x/~7x win it is from
Python; see `AnimusCore_v1/BENCHMARKS.md`'s Phase 11 section for where
that number actually comes from.

**A lock-free SPSC ring, header-only, no `Engine` involved:**
`animus::SpscRingBuffer<T>` is a separate, fully inline template class --
usable with zero DLL and zero linking, same as `Engine` itself -- for
when you have exactly one producer thread and one consumer thread and
want the simpler, faster primitive that constraint buys (a plain atomic
load/store pair, no compare-exchange retry loop):

```cpp
animus::SpscRingBuffer<animus::TelemetryPayload> ring(1 << 16);

// Producer thread only:
ring.push(animus::TelemetryPayload{
    animus::read_cycle_counter(), /*event_id=*/500, /*trace_id=*/1, /*metric_value=*/150});

// Consumer thread only (a *different* thread than the producer is fine;
// a second producer or a second consumer thread is not -- see the
// class's own docstring in animus.hpp for why that isn't enforced at
// runtime on this specific hot path):
animus::TelemetryPayload out;
if (ring.pop(out)) { /* ... */ }
```

**CPU pinning is not header-only, unlike everything else in this
guide** -- `animus_pin_current_thread_to_core`/`animus_get_cpu_count`
are declared in `animus.hpp` but only *defined* in `animus_engine.cpp`
(the DLL shim), so calling them from a pure single-header build without
linking that file is a link error, not a compile error. Pulling in the
DLL shim just for two OS calls defeats guide 2's "zero DLL" premise, so
call the platform API directly instead -- it's one line either way:

```cpp
// At file scope, near your other #includes -- windows.h in particular
// contains extern "C" blocks that are a compile error if this #include
// ends up inside a function body instead:
#if defined(_WIN32)
#include <windows.h>
#elif defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

// Then, in whichever function pins the calling thread:
#if defined(_WIN32)
SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)1 << core_id);
#elif defined(__linux__)
cpu_set_t cpuset; CPU_ZERO(&cpuset); CPU_SET(core_id, &cpuset);
pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
#endif
```

**Before picking a `core_id`, read this:** don't just pin to the
highest-numbered core. `benchmarks/fintech_tail_latency.py` did exactly
that first and measured tail latency up to 34.5x *worse* -- on a hybrid
CPU (P-cores + E-cores), the last core is not reliably a fast one, and
there's no portable way to ask the OS which is which; that benchmark now
probes several candidate cores with a cheap workload and pins to
whichever measures fastest, rather than guessing. Even with a good core,
throughput and p50/p90 latency improved consistently across 5 runs, but
**p99.99 tail latency did not reliably improve, and often got worse** --
thread affinity alone doesn't reserve a core exclusively, so a pinned
thread has nowhere to go on the rare occasion its one core is briefly
needed elsewhere. See `AnimusCore_v1/BENCHMARKS.md`'s Phase 14 section
for the full numbers before relying on pinning to fix a tail-latency
problem specifically.

```bash
# Portable core + RBAC layer (animus::, animus::security::) compiles with
# any C++17 compiler -- Linux, macOS, or Windows with g++/clang -- and was
# verified with a real g++ (MinGW) build and run, not just a compile check:
g++ -std=c++17 -O2 -pthread poc.cpp -o poc

# Full feature set (adds animus::transport::, animus::cluster::) requires
# MSVC specifically, not just "Windows" -- Schannel/SSPI is Windows-only,
# and certificate loading additionally relies on an MSVC-only
# std::ifstream(std::wstring, ...) overload that MinGW/libstdc++ doesn't
# provide. animus_release.hpp gates those two sections on
# `defined(_WIN32) && defined(_MSC_VER)` (not just _WIN32) for exactly this
# reason -- building the single header with MinGW g++ on Windows still only
# gets you the portable core + RBAC layer, verified above, not a build error:
cl /std:c++17 /EHsc /O2 poc.cpp
```

`animus_release.hpp` is a **generated** file -- it is produced from the
four source headers (`animus.hpp`, `animus_security.hpp`,
`animus_transport.hpp`, `animus_cluster.hpp`) by `amalgamate.py`. If you're
working from this repo rather than a downloaded release, regenerate it
after touching any of the four sources:

```bash
python amalgamate.py
```

See `AnimusCore_v1/execution_interop_demo.cpp` for a full broker/execution
integration built the same way (against the un-amalgamated headers).

---

## 3. Secure multi-tenant + mTLS transport

For a deployment serving multiple isolated tenants over an untrusted
network link. Windows/MSVC only (Schannel-based).

```powershell
# Generate a demo CA + server/client leaf certificates (native Windows PKI
# cmdlets, no OpenSSL):
powershell -File AnimusCore_v1/generate_demo_certs.ps1
```

```cpp
#include "animus_release.hpp"   // or animus_security.hpp + animus_transport.hpp

// Server side: verify a client cert, resolve it to an AccessToken, and
// route every call through RBAC + tenant isolation.
animus::security::TenantRegistry registry;
registry.create_tenant(/*tenant_id=*/42, 1 << 16);
animus::security::SecureTelemetryGateway gateway(registry);

animus::transport::TrustedRoot trust(ca_cert);
animus::transport::CertificateIdentityMap identity_map;
identity_map.add(L"animus-client-tenant-42", animus::security::AccessToken{42, /*principal_id=*/1, animus::security::Role::Operator});

// ... accept an mTLS connection (animus::transport::SecureChannel), verify
// the peer cert against `trust`, resolve its CN through `identity_map.resolve(cert, token)`,
// then dispatch each received WireFrame through `gateway.record(token, ...)`.
```

**No batched ingestion here yet:** `SecureTelemetryGateway` only wraps
`Engine::record()`, not `record_batch()` (`animus_security.hpp:136-140`) --
every call is individually RBAC-checked and separately audited by design,
so a batch would need its own gateway method (checking the token once,
then dispatching the whole batch to the resolved tenant `Engine`) rather
than just forwarding to `record_batch()` under an existing single-event
method name. Calling `registry.get_tenant(id)->record_batch(...)` directly
would skip both the RBAC check and the audit trail this guide exists to
provide, so don't do that; if you need batched ingestion under RBAC, add
a `record_batch(token, ...)` method to the gateway following the same
`authorize_and_dispatch` pattern the other methods use.

Full working client + server: `AnimusCore_v1/secure_multitenancy_demo.cpp`
(RBAC/tenancy only) and `AnimusCore_v1/secure_transport_demo.cpp` (adds real
TLS 1.3 mutual auth over loopback TCP). Build/run:

```powershell
# From an "x64 Native Tools Command Prompt for VS":
cl /std:c++17 /EHsc /O2 AnimusCore_v1/secure_transport_demo.cpp
secure_transport_demo.exe
```

---

## 4. Distributed Raft-lite cluster

For high-availability rule replication across multiple nodes, with no
gRPC/Protobuf dependency -- inter-node RPC reuses the same mTLS transport as
guide 3. Windows/MSVC only.

```powershell
# generate_demo_certs.ps1 also issues 3 cluster-node identities
# (animus-node-1/2/3):
powershell -File AnimusCore_v1/generate_demo_certs.ps1
```

```cpp
#include "animus_release.hpp"   // or animus_cluster.hpp directly

using namespace animus::cluster;

PeerConfig p2{2, "10.0.0.2", 47902, L"animus-node-2"};
PeerConfig p3{3, "10.0.0.3", 47903, L"animus-node-3"};

auto engine = animus::Engine::Create(1 << 16);
engine->start_persistence("node1.log");  // required for rules to evaluate

RaftNode node(/*id=*/1, {p2, p3}, std::move(my_cert), trust, *engine, /*listen_port=*/47901);
node.start();

// From whichever node is currently leader (check node.is_leader()):
AddRuleCommand cmd{/*rule_id=*/1, /*event_id=*/500, /*threshold=*/100, /*comparator=*/0, /*severity=*/5};
NodeId leader_hint = 0;
if (node.propose(cmd, &leader_hint) == ProposeResult::Ok) {
    // Committed to a majority; every node's local `engine` now has the rule.
}
```

`propose()` blocks until the command is committed to a majority (not just
accepted locally) -- see `AnimusCore_v1/BENCHMARKS.md`'s Phase 10 section
for measured write-latency and full-cluster convergence numbers. Full
3-node PoC with real failover: `AnimusCore_v1/cluster_demo.cpp`.

Only rule commands (`AddRuleCommand`) go through `propose()`/consensus --
`RaftNode` never wraps telemetry ingestion (`animus_cluster.hpp` calls
`engine_.add_rule()` on commit and nothing else against `engine_`). Each
node's `engine` above is the same `animus::Engine` from guide 2, so its
`record_batch()` applies here identically and unconditionally: call it on
your local `engine` the same way, no cluster-specific wrapping needed,
since ingestion stays node-local by design.

```powershell
cl /std:c++17 /EHsc /O2 AnimusCore_v1/cluster_demo.cpp
cluster_demo.exe
```

---

## Which guide should I start with?

- Building a Python-based SOAR/orchestration pipeline, or just scripting
  against telemetry? **Guide 1.**
- Embedding directly in a latency-sensitive C++ service (e.g. an execution
  path) with no Python/ctypes hop? **Guide 2.**
- Need per-tenant isolation and/or encrypted remote ingestion? **Guide 3**
  (layers on top of Guide 2's engine).
- Need the rule set to survive a node failure? **Guide 4** (layers on top
  of Guide 3's transport).
