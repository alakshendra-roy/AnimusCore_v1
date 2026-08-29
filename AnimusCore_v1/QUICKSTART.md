# Animus Core v1.0 -- Client Quickstart Guides

Six proof-of-concept quickstarts, one per way of consuming Animus Core.
Pick the one that matches your integration:

| Guide | For | Platform |
|---|---|---|
| [1. Python SDK](#1-python-sdk-pip-install) | Orchestrators, SOAR pipelines, scripting | Any (Windows/Linux/macOS) |
| [2. C++ single header](#2-c-single-header-embedded--in-process) | Embedding directly in a C++17 service, ultra-low-latency execution paths | Any (portable subset); Windows + MSVC for the full feature set |
| [3. Secure multi-tenant + mTLS](#3-secure-multi-tenant--mtls-transport) | Multiple isolated tenants, encrypted remote ingestion | Windows + MSVC |
| [4. Distributed cluster](#4-distributed-raft-lite-cluster) | High-availability, multi-node rule replication | Windows + MSVC |
| [5. Enterprise licensing](#5-enterprise-edition-offline-rsa-signed-hardware-licensing) | Node-locked commercial deployments (`proprietary-edition` branch only) | Windows |
| [6. Market data feed adapters](#6-market-data-feed-adapters-l2l3-book--trade-ticks) | Live L2/L3 order book + trade tick ingestion (`proprietary-edition` branch only) | Any (Windows/Linux/macOS) |

All six PoCs below are real, runnable code paths already exercised in this
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

**Automated benchmark suite: tick-to-trade latency, 8-thread ring buffer
throughput, and CPU cache locality, rendered to a Markdown report:**

```bash
python benchmarks/generate_benchmark_report.py
```

This compiles and runs `AnimusCore_v1/animus_benchmark_suite.cpp` --
deliberately native C++, not Python/ctypes, since two of its three
measurements can't be honestly taken from Python: the GIL would
serialize an "8 concurrent threads" test onto one core instead of really
exercising cross-core ring contention, and the ~1,300 ns/call ctypes
marshalling tax documented in Phase 16 is on its own wider than the
sub-microsecond tick-to-trade latency this suite measures. The script
itself is the automation layer -- compile (g++/clang++, cached until the
source changes), run, parse, and write `benchmarks/BENCHMARK_REPORT.md`,
a fully reproducible report with its own Methodology & Limitations
section, not a hand-typed one.

Measured across 5 consecutive runs on the development machine: a
single-threaded, sequential `MarketDataFeed` push -> poll ->
`ExecutionClient::submit()` round trip lands at p50/p99 = 100 ns and
p99.9 = 100-200 ns -- genuinely sub-microsecond. `LockFreeRingBuffer`
sustained 6.9-9.6M pushes/sec under real 8-thread producer contention,
with a post-hoc drain-count check confirming zero lost or duplicated
pushes every run. A pointer-chase cache-locality sweep plus a
false-sharing A/B test found cache-line padding (`alignas(64)`) worth a
consistent >4x throughput improvement over false sharing -- a direct,
data-backed justification of the same `alignas(64)` layout
`LockFreeRingBuffer`/`SpscRingBuffer` already use for their head/tail
index atomics, not an abstract exercise. See
`AnimusCore_v1/BENCHMARKS.md`'s Phase 19 section for the full breakdown,
including a real methodology bug (an early two-thread tick-to-trade
design that measured *milliseconds*, not nanoseconds, due to the same
unpaced-producer-backlog effect Phase 16 already documented once before)
found and fixed while building this suite.

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

**The same thing, backed by compiled C++ instead of pure Python:**
`SharedTelemetryChannel` is a ctypes wrapper over `animus::SharedTelemetryChannel`
(a native shared-memory transport -- Windows `CreateFileMappingA`/
`MapViewOfFile`, POSIX `shm_open`/`mmap`), deliberately wire-compatible
with `SharedTelemetryRing` above: both read/write the identical byte
layout, so one process can produce with either implementation and
another can consume with either one, in any combination.

```python
from animus import SharedTelemetryChannel
producer = SharedTelemetryChannel.create("my-shared-segment", capacity=65536)
producer.push(event_id=500, trace_id=1, metric_value=150)

# From a second process (or the pure-Python SharedTelemetryRing above,
# attached to the same name -- either works):
consumer = SharedTelemetryChannel.attach("my-shared-segment")
record = consumer.pop()  # None if nothing pending; never blocks
```

**Read this before reaching for it on a latency-sensitive path:** the
native `push()` call itself is genuinely sub-microsecond (~35-40ns,
measured). A *single* call to it from Python costs ~1.3us instead --
the same ctypes-marshalling tax documented in Phases 11 and 13 -- and
genuine cross-process propagation (one process's write becoming visible
to another's read) measured at single-digit-to-low-double-digit
microseconds even from native code, not sub-microsecond, on a
general-purpose machine with no CPU isolation set up. A related trap,
found and documented rather than left for you to hit: pushing a burst
of events back-to-back with no pacing can make a Python consumer fall
behind the producer, and the *later* events in that burst end up
waiting in a growing backlog -- multiple milliseconds by the end of a
20,000-event unpaced burst in one measurement -- which looks like
terrible IPC latency but is actually a throughput mismatch, not a
transport problem. See `AnimusCore_v1/BENCHMARKS.md`'s Phase 16 section
for the full layer-by-layer numbers (same-process vs. cross-process,
native vs. ctypes) before assuming which one applies to your use case.

Single-producer/single-consumer, same as `SharedTelemetryRing`: don't
share one channel across multiple producer or multiple consumer
processes. No pure-Python fallback for this specific class -- that's
what `SharedTelemetryRing` already is; attach to the same segment with
whichever implementation fits the process that doesn't have a compiled
binary available.

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

**Cross-process, not just cross-thread: `animus::SharedTelemetryChannel`.**
`SpscRingBuffer` above lives on this process's heap -- a second *thread*
can share it, a second *process* cannot. `SharedTelemetryChannel` is the
same SPSC contract over a named OS shared-memory mapping instead
(Windows `CreateFileMappingA`/`MapViewOfFile`, POSIX `shm_open`/`mmap`),
also fully header-only, and deliberately wire-compatible with the Python
SDK's `SharedTelemetryRing`/`SharedTelemetryChannel` (guide 1) -- any
combination of a C++ process and a Python process can produce/consume on
the same segment:

```cpp
auto producer = animus::SharedTelemetryChannel::create("my-shared-segment", /*capacity=*/65536);
producer->push(/*event_id=*/500, /*trace_id=*/1, /*metric_value=*/150);

// From a second process (C++ via attach() below, or Python via
// SharedTelemetryChannel.attach()/SharedTelemetryRing.attach() --
// any of the three read the identical byte layout):
auto consumer = animus::SharedTelemetryChannel::attach("my-shared-segment");
animus::SharedTelemetryRecord rec{};
if (consumer && consumer->pop(rec)) { /* ... */ }
```

`create()`/`attach()` return `nullptr` (not an exception) on failure --
a name collision, a segment that doesn't exist, or a segment too small
to hold a valid header -- check before dereferencing, same convention
as `Engine::Create()`'s own error handling elsewhere in this guide.
`unlink(name)` destroys the underlying OS object: a real operation on
POSIX (call once every attached process has closed), a documented no-op
on Windows (named file mappings are destroyed automatically once every
handle to them closes).

**Measured, not assumed, before you rely on this for a latency budget:**
the native `push()` call above costs ~35-40ns in isolation. Genuine
cross-process propagation -- one process's write becoming visible to
another's `pop()` -- measured at single-digit-to-low-double-digit
microseconds even here, in native code with no ctypes involved, on a
general-purpose machine with no CPU isolation configured. See
`AnimusCore_v1/BENCHMARKS.md`'s Phase 16 section for the full
same-process-vs-cross-process breakdown, including a burst-without-
pacing backlog effect that looked like catastrophic latency until it
was traced to a producer/consumer throughput mismatch, not the
transport itself.

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

**Same gap for CEP rules:** `SecureTelemetryGateway` wraps `Engine::add_rule()`
(`animus_security.hpp:142-146`) but not `add_cep_rule()` -- it predates
the CEP engine. As with `record_batch()` above, don't call
`registry.get_tenant(id)->add_cep_rule(...)` directly (skips RBAC and the
audit trail); add an `add_cep_rule(token, ...)` method to the gateway,
same `authorize_and_dispatch` pattern, if a tenant needs sliding-window
rules under this guide's access control.

**`SharedTelemetryChannel` is a bigger problem than a gap here -- it's a
real bypass, not just a missing wrapper.** `animus_security.hpp` doesn't
reference shared memory at all, and structurally can't wrap it the way
it wraps `record()`/`add_rule()`: `SharedTelemetryChannel` isn't an
`Engine` method routed through a tenant's isolated instance, it's a
standalone named-OS-object primitive with no concept of a tenant, a
role, or a permission -- `create()`/`attach()` take nothing but a name
and a capacity. Any process on the same machine that knows (or guesses)
that name can attach and read or write raw telemetry directly, with no
RBAC check, no audit entry, and no tenant boundary -- completely
bypassing everything `SecureTelemetryGateway` exists to enforce. If a
deployment using this guide's RBAC/tenancy layer also uses
`SharedTelemetryChannel` anywhere, treat the segment name like a secret
and the OS's own file-mapping/shared-memory permissions as the only
access control actually in effect for it -- this guide's RBAC has no
reach into it at all.

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

**CEP rules do not replicate across the cluster.** `AddRuleCommand` (and
so `propose()`/consensus) only carries a plain `RuleThreshold` -- there is
no `AddCepRuleCommand`, and `apply_committed_entries_locked()` never calls
`add_cep_rule()` on anything. Calling `engine->add_cep_rule(...)` works --
it's the same node-local `Engine` as guide 2 -- but only on the node you
call it on; the other nodes in the cluster won't know about that rule
unless you register it on each of them yourself. If every node needs the
same sliding-window rule, that's a real gap to close (extend
`animus_cluster.hpp`'s command set with an `AddCepRuleCommand`, mirroring
`AddRuleCommand`'s existing replication path), not something this guide
can currently paper over.

**`SharedTelemetryChannel` is node-local too, and for a more basic
reason: nothing in this cluster layer knows it exists.**
`animus_cluster.hpp` doesn't reference shared memory at all -- there's
no `WireFrame`/`SecureChannel` message for it, and `RaftNode` has no
opinion on how telemetry reaches a node's local `engine` in the first
place, only on how `AddRuleCommand`s replicate once they're there. If
you feed one node's ingestion via a shared-memory segment (guide 2's
`SharedTelemetryChannel` section) with a separate co-located collector
process, that's exactly as valid as feeding it via `engine->record()`
directly -- but it's local to that one node's machine, same as every
other ingestion path in this guide; nothing about it crosses to the
other nodes, and consensus never touches it.

```powershell
cl /std:c++17 /EHsc /O2 AnimusCore_v1/cluster_demo.cpp
cluster_demo.exe
```

---

## 5. Enterprise Edition: Offline RSA-Signed Hardware Licensing

**`proprietary-edition` branch only** -- not part of the MIT `master`
tree, for the same reason a node-lock and open source don't mix: anyone
with the source can delete a check baked into it. This branch replaces
`LICENSE` with a placeholder "All Rights Reserved" notice (not reviewed
by counsel -- don't ship it to a customer as-is) specifically to carry
this feature.

`animus_verify_license(path)` validates a signed license file entirely
offline -- no license server, no network call -- and gates two existing
primitives from guide 1/2 (`spsc_init`/`animus_spsc_init` and
`pin_current_thread_to_core`/`animus_pin_current_thread_to_core`) behind
it: neither succeeds until a valid license has been verified in-process,
and pinning additionally rejects any core at or beyond the license's
`max_cores`, independent of the machine's real core count.

**1. Generate a signing keypair (once, on the machine that issues licenses):**

```powershell
powershell -File AnimusCore_v1/license_tools/generate_license_keypair.ps1
```

Writes the private key to `license_tools/private/license_private.blob`
(gitignored -- never commit it; anyone with this file can mint a license
for any machine) and the public key as a generated C++ header,
`AnimusCore_v1/animus_license_pubkey.hpp` (safe to commit -- it's baked
into every shipped binary so `animus_verify_license` can check signatures
without ever calling home). Regenerating the keypair invalidates every
license issued against the old one.

**2. Issue a license for a customer's machine:**

```powershell
# Local testing -- licenses THIS machine (auto-computes its fingerprint):
powershell -File AnimusCore_v1/license_tools/sign_license.ps1 -OutFile test.lic -MaxCores 8

# A real customer license -- their fingerprint, their core entitlement,
# an optional expiry:
powershell -File AnimusCore_v1/license_tools/sign_license.ps1 `
    -OutFile customer.lic -MaxCores 4 `
    -FingerprintHex <64 hex chars from the customer's machine> `
    -ExpiresInDays 365
```

The fingerprint is SHA-256(`MachineGuid` + primary MAC address) -- not a
literal CPU serial number, since modern CPUs don't expose one via CPUID
for privacy reasons. `MachineGuid` comes from
`HKLM\SOFTWARE\Microsoft\Cryptography`; the MAC is the smallest
candidate among the machine's genuine, manufacturer-assigned addresses
(filtered via the IEEE "locally administered address" bit to exclude
Windows' own virtual/randomized MACs -- a real dev machine surfaced three
different Wi-Fi MACs from virtual roles alone before this filter was
added, so "first adapter" is not a safe rule). `sign_license.ps1`
computes this identically to `animus_verify_license`'s C++ side -- cross-
verified byte-for-byte before either was relied on.

**3. Verify the license and use the entitlement it grants:**

```python
from animus.bindings import AnimusBindings

bindings = AnimusBindings()
if not bindings.verify_license("customer.lic"):
    raise SystemExit("license invalid, expired, or issued for a different machine")

assert bindings.is_licensed()
core_budget = bindings.licensed_max_cores()   # e.g. 4

bindings.spsc_init(buffer_capacity=1_000_000)          # fails closed until verified above
bindings.pin_current_thread_to_core(core_budget - 1)   # fails if >= core_budget
```

`verify_license()`/`is_licensed()`/`licensed_max_cores()` have no
pure-Python fallback, same as `spsc_init`/`pin_current_thread_to_core`
themselves -- they raise `RuntimeError` without a compiled native binary
loaded, rather than silently granting an entitlement nothing actually
checked. License state is a process-wide singleton: once
`verify_license()` succeeds, it stays succeeded for the rest of the
process (there's no way to "un-verify"), and it does not persist across
process restarts -- call it again at startup every time.

**Fails closed, checked directly against the real DLL, not just
asserted:** a validly-signed license for a *different* machine's
fingerprint is rejected; a byte-tampered license (even one bit flipped
in an otherwise correctly-signed file) is rejected; a nonexistent file
path is rejected; and a fresh process that hasn't called
`verify_license()` at all fails closed on both `spsc_init()` and
`pin_current_thread_to_core()` -- confirmed via a subprocess-isolated
test, since license state can't be reset within a single process once
set. See `tests/test_bindings.py`'s `RealNativeEngineIntegrationTests`
license tests and `UnlicensedGatingTests` for the full suite, and
`AnimusCore_v1/license_tools/test_fixtures/wrong_machine_test_license.lic`
for a real, validly-signed license fixture safe to commit and test
against on any machine (including CI) precisely because it's signed for
a fingerprint that can never match real hardware.

**Windows-only, explicitly, not silently:** `animus_verify_license`
returns `false` immediately on non-Windows builds (RSA verification uses
Windows CNG/BCrypt directly, `BCryptVerifySignature` with PKCS1 padding +
SHA-256 -- no OpenSSL, no external crypto dependency) rather than faking
a pass. A Linux/macOS build of this branch cannot verify a license at
all yet; that's a documented gap, not an oversight.

---

## 6. Market Data Feed Adapters (L2/L3 Book + Trade Ticks)

**Currently lives on the `proprietary-edition` branch** (alongside guide
5) simply because that's where it was added -- unlike guide 5, nothing
about this feature actually depends on licensing or Windows; it's plain
portable C++17 like guides 1/2's core engine.

`animus::MarketDataFeed` (`AnimusCore_v1/animus.hpp`) is a dedicated
low-latency ingestion primitive for live market data: two independent
lock-free rings (order-book updates, trade ticks), each backed by the
same `LockFreeRingBuffer` (Vyukov MPMC) `EngineImpl`'s own telemetry ring
uses. Unlike guide 2's `SpscRingBuffer`, it's genuinely thread-safe for
**concurrent producers and concurrent consumers** -- multiple feed-handler
threads (e.g. one per venue connection) can push into the same feed at
once, and multiple consumer threads can drain it at once, with no
external locking.

```python
from animus import MarketDataFeed, BookSide, BookUpdateAction, TradeAggressor

feed = MarketDataFeed.create(l2_capacity=1 << 16, trade_capacity=1 << 16)

# Any number of producer threads, e.g. one per venue connection:
feed.push_l2_update(
    instrument_id=7, side=BookSide.ASK, action=BookUpdateAction.NEW,
    level=0,                     # venue-relative depth index, 0 = best
    price_ticks=101250, quantity=300,
    sequence_number=1,           # the venue's own feed sequence number
    exchange_timestamp_ns=venue_ts_ns,
)
feed.push_trade(
    instrument_id=7, trade_id=42, aggressor_side=TradeAggressor.BUYER,
    price_ticks=101250, quantity=50,
    sequence_number=2, exchange_timestamp_ns=venue_ts_ns,
)

# Any number of consumer threads, e.g. a book builder and a strategy,
# each independently draining what they need:
for update in feed.poll_l2_updates(max_count=1024):
    print(update.instrument_id, BookSide(update.side), BookUpdateAction(update.action),
          update.level, update.price_ticks, update.quantity)
for trade in feed.poll_trades(max_count=1024):
    print(trade.instrument_id, trade.trade_id, TradeAggressor(trade.aggressor_side),
          trade.price_ticks, trade.quantity)

feed.close()
```

**`level`, not price, indexes the book.** `L2Update.level` is a
venue-relative depth index (0 = best bid/ask), not a raw price -- a
consumer reconstructs book state by keyed `(instrument_id, side, level)`
replacement, matching how incremental L2 feeds (ITCH, ArcaBook, ...)
actually publish updates, rather than by summing per-price deltas.
`action` tells you what kind of change it is: `NEW` (a fresh level
entered the book at this index), `UPDATE` (that level's quantity
changed), or `DELETE` (the level was removed -- `quantity` is not
meaningful on a `DELETE` and should be ignored).

**Two independent rings, two independent capacities.** `l2_capacity` and
`trade_capacity` are sized separately at `create()` time (each rounded up
to the next power of two internally, same as `init()`'s
`buffer_capacity`) -- a full order-book ring does not block trade
ingestion, and vice versa, since they're genuinely separate
`LockFreeRingBuffer` instances, not a shared one keyed by message type.

**Sequence numbers are yours to reconcile, not this feed's.**
`sequence_number` carries whatever the venue's own feed sequence number
was; `MarketDataFeed` stores and returns it but does not itself detect
gaps -- compare successive values per `instrument_id` and decide how to
react (resync, drop, ...) at the strategy/book-builder layer, since that
policy is venue- and strategy-specific. `timestamp_cycles` is stamped
locally at the moment of the `push_*` call (`read_cycle_counter()`, same
convention as `TelemetryPayload`); `exchange_timestamp_ns` is whatever
timestamp the feed itself carried -- comparing the two measures
feed-to-ingestion latency directly.

**Handle-based, not a singleton, same as `SharedTelemetryChannel`
(guide 1) --** `MarketDataFeed.create()` returns an independent instance,
so run as many feeds as you need in one process (e.g. one per venue or
instrument shard), each with its own pair of rings.

**No pure-Python fallback, same reasoning as `SpscRingBuffer` and
`SharedTelemetryChannel`:** `MarketDataFeed.create()` raises
`FileNotFoundError` if no compiled native binary is found -- this is a
native concurrency primitive, not something a Python reimplementation
could meaningfully provide the same thread-safety guarantee for.

**Thread-safety verified directly, not just claimed:** a stress test
drives 6-8 real OS producer threads and 3-4 real OS consumer threads
against one `MarketDataFeed` instance concurrently, pushing/draining tens
of thousands of records across both rings, and confirms every record is
drained exactly once with no corrupted fields -- see
`tests/test_bindings.py`'s `MarketDataFeedIntegrationTests` for the full
suite. No dedicated throughput/tail-latency benchmark has been run for
this feature yet (unlike `record_events_batch`/SPSC's Phase 11/13/14
numbers) -- treat it as functionally verified, not yet performance-
characterized.

The C++ single-header path (guide 2) works identically, with no C-ABI/
ctypes hop: `#include "animus.hpp"` and drive `animus::MarketDataFeed`
directly -- `push_l2_update`/`push_trade`/`poll_l2_updates`/`poll_trades`
are ordinary member functions, same names and same thread-safety
guarantee as the Python methods above.

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
- Shipping a commercial, node-locked build to customers? **Guide 5**
  (`proprietary-edition` branch only, layers on top of Guide 1/2's
  `spsc_init`/pinning primitives).
- Ingesting a live L2/L3 order book or trade tick feed, possibly from
  multiple venue connections at once? **Guide 6** (`proprietary-edition`
  branch only for now; portable and independent of Guide 5's licensing).
