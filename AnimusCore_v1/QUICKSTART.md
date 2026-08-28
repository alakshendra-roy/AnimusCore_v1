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
