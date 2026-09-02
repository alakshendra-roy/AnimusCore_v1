# Animus Core — Commercial Overview

**Sub-microsecond telemetry ingestion for latency-sensitive trading and fintech infrastructure**

> **Audience:** CTOs, VPs of Infrastructure, and Managing Directors evaluating low-latency telemetry and execution-monitoring infrastructure at quantitative funds and fintechs.
> **Companion document:** [`BENCHMARK_DATASHEET.md`](BENCHMARK_DATASHEET.md) — hard engineering specifications for your architecture and quant development teams.

---

## 1. Executive Summary

Every telemetry and risk pipeline built on a Python orchestration layer eventually hits the same wall: **the language boundary itself becomes the bottleneck.** REST calls, message queues, JSON serialization, and even naive `ctypes`/gRPC bridges each add microseconds of marshalling overhead *before* your business logic ever runs — overhead that compounds silently across millions of events per day.

For a fraud-detection loop, a risk check on an execution path, or a market-data-driven decision loop, that overhead isn't a rounding error. It is the difference between catching an anomaly in-flight and catching it after the trade has already cleared.

**Animus Core removes the boundary.** It is a C++17 telemetry and rule-evaluation engine exposed through a direct C-ABI shared library (`.dll` / `.so`), paired with a zero-dependency Python SDK (`ctypes`-only, no third-party packages) that talks to it through direct buffer pointers and shared memory — not IPC, not serialization, not a socket in the loop. The result is a decision loop measured in **tens to hundreds of nanoseconds**, not the milliseconds typical of message-bus or REST-mediated telemetry stacks, with an integration surface your Python team already knows how to call. That number isn't a marketing rounding — `BENCHMARK_DATASHEET.md`'s "Cross-core SPSC dispatch latency" section measures the real producer-to-consumer handoff between independent threads, at RDTSC (hardware timestamp counter) resolution, at p50 53ns / p99 65ns.

> **Bottom line:** you keep your existing Python orchestration, control plane, and tooling. You replace only the hot path — event ingestion, rule evaluation, and signal dispatch — with a native engine that was built from the ground up to never leave a CPU cache line it doesn't have to.

---

## 2. ROI & Value Proposition Matrix

| Dimension | Traditional Telemetry Stack | Animus Core |
|---|---|---|
| **Transport** | REST / gRPC / message queue between services | Direct C-ABI call or shared-memory ring — no network hop, no broker |
| **Serialization** | JSON / protobuf marshal-unmarshal on every event | Raw byte buffers passed by pointer; zero-copy on the hot path |
| **Language boundary cost** | Per-call FFI or IPC tax, often invisible until it dominates p99 | Amortized via batched ingestion (`animus_record_events_batch`); native engine cost is µs-independent of the caller's language |
| **Concurrency model** | Lock-based queues, GIL-serialized Python workers | Lock-free MPMC ring buffers (Vyukov algorithm), cache-line-padded to eliminate false sharing |
| **Rule evaluation** | Often re-implemented per service, or delegated to an external CEP engine over the network | In-process, same worker thread as ingestion — zero-copy signature matching |
| **Operational risk of adoption** | Rip-and-replace of existing pipelines | Additive: Python SDK installs alongside your current stack (`pip install -e .`), no changes to upstream producers |
| **Time to first measurable result** | Weeks of integration before a latency number exists | A single pilot script (`Pilot_Kit/animus_integration_example.py`) reports your own measured ingestion latency on day one |

**Why this matters commercially:**

* In any latency-sensitive decision loop — order risk checks, fraud scoring, market-data-triggered execution — the cost of a slow telemetry path is not "slower dashboards." It's **missed windows**: the anomaly detected after the position is already taken, the risk breach flagged after the order already routed.
* Collapsing the ingestion-to-decision loop from milliseconds to sub-microsecond doesn't just make the system faster — it changes *what the system is capable of catching in time to act*.
* Because Animus Core is additive to your existing Python control plane, the ROI conversation is narrow and concrete: **what is one avoided missed-detection window, or one recovered microsecond of decision latency, worth in your specific execution or risk workflow?** The Pilot Kit (below) is designed to let your own team answer that question with your own data, not a vendor's slide deck.

---

## 3. Enterprise Pilot Kit Framework

Adoption risk is the single biggest objection to any new hot-path infrastructure. The Pilot Kit is built to remove it.

> ### Low-risk, 30-day, hardware-locked evaluation
> No production commitment, no source escrow, no network-dependent licensing call. You evaluate Animus Core against **your own workload, on your own hardware**, with a time-boxed license that can only run on the machine you fingerprint.

**How it works:**

1. **You run `get_fingerprint.ps1`** — a read-only script that derives a hardware fingerprint (`MachineGuid` + primary NIC MAC, SHA-256). It makes no network call, writes nothing to disk, and touches no private key. You send us the fingerprint, not the other way around.
2. **We issue a `.lic` file** — RSA-2048-signed, valid for 30 days, and cryptographically bound to that fingerprint alone. Verification is entirely offline; there is no phone-home, no license server dependency, and no risk of a vendor outage affecting your evaluation.
3. **You run the integration example** — `Pilot_Kit/animus_integration_example.py` is a complete, real integration (not pseudocode): it loads the compiled native engine through the same Python SDK you'd use in production, ingests synthetic telemetry through the real C-ABI batched-ingestion call, and **prints your own measured per-event latency** on your own hardware.
4. **Core ingestion works with or without the license.** The license gates only opt-in features (e.g., CPU core pinning for tail-latency tuning) that fail closed by design — there is no unlicensed default behavior to worry about auditing. Your evaluation of the core value proposition (ingestion latency, throughput, rule evaluation) is unrestricted from day one.

| Property | Detail |
|---|---|
| Duration | 30 days, hardware-locked |
| Network dependency | None — fully offline verification |
| Production impact | Zero — additive install alongside existing stack |
| What's measured | Your own workload, your own hardware, your own numbers |
| What's gated by license | Opt-in tuning features only; core ingestion is never gated |

---

## 4. Commercial Engagement Tiers

Animus Core is offered across three engagement models, structured to match how a quantitative fund or fintech typically moves from evaluation to production to strategic ownership.

| Tier | Intended For | What's Included |
|---|---|---|
| **Pilot** | Teams evaluating fit before committing budget | 30-day hardware-locked evaluation license (Section 3), full Pilot Kit, direct technical support during the evaluation window |
| **Production Node** | Teams deploying Animus Core into a live pipeline | Per-node production license (hardware-locked, renewable), compiled binaries for your target platform(s), integration support, priority defect response |
| **Custom Source License** | Teams requiring deep customization, in-house builds, or strategic ownership of the engine | Full source access under a negotiated commercial license, architecture consultation for your specific workload (e.g., custom rule engines, cluster topologies, market-data adapters), co-development engagement for bespoke extensions |

> **Getting started:** every engagement begins with the Pilot tier — there is no fast path around your own team validating the numbers on your own hardware. Contact your Animus Core representative to scope a Production Node or Custom Source License engagement once your pilot has produced the data your team needs.

---

*For engineering specifications, benchmark methodology, and integration code, see* [`BENCHMARK_DATASHEET.md`](BENCHMARK_DATASHEET.md) *(including its "Cross-core SPSC dispatch latency" section for the RDTSC-resolution transport numbers cited in Section 1 above).*
