# Animus Core — Institutional Pilot Program

**30-Day Paid Proof-of-Performance (PoP) for proprietary trading desks and latency-sensitive market makers**

> **Audience:** Heads of Trading Technology, Quant Desk Leads, and Latency Engineering evaluating Animus Core for a production execution or risk-monitoring path.
> **Companion documents:** [`../COMMERCIAL_OVERVIEW.md`](../COMMERCIAL_OVERVIEW.md) — business case and engagement tiers; [`../BENCHMARK_DATASHEET.md`](../BENCHMARK_DATASHEET.md) — the measured figures cited throughout this program; [`../Pilot_Kit/PILOT_README.md`](../Pilot_Kit/PILOT_README.md) — the free, self-serve 30-day evaluation kit this program is *not* (see below); [`../eval_kit/README.md`](../eval_kit/README.md) — the same free, self-serve tier, but for the Linux shared-memory transport specifically, as a turnkey tarball rather than a source checkout.

---

## 1. What This Is — and Isn't

The [Pilot Kit](../Pilot_Kit/PILOT_README.md) is a free, self-serve, hardware-locked evaluation license: install the SDK, run the integration example, and see your own measured ingestion latency on day one. It's the right starting point for an engineering team doing its own technical due diligence.

**This program is different.** The Institutional Pilot Program is a **paid, white-glove, four-week engagement** run jointly with your engineering team, structured specifically to answer the question a desk actually needs answered before a production commitment: *not* "does the engine work," but **"does it perform, on our own historical data, our own market shape, and our own hardware, to a level our execution or risk workflow can depend on."**

| | Pilot Kit (free) | Institutional Pilot Program (this document) |
|---|---|---|
| Cost | Free | Paid engagement — scoped per desk, see §5 |
| Duration | 30 days, self-serve | 30 days, structured weekly with our engineering team |
| Data | Synthetic telemetry | Your own historical and (optionally) shadow-live market data |
| Deliverable | A measured latency number on your machine | A signed-off performance report + a licensing decision |
| Best for | "Let me kick the tires" | "We're deciding whether to put this in production" |

---

## 2. Eligibility

This program is scoped for desks with a genuine latency-sensitive workload to validate against — proprietary trading desks, market makers, and execution-infrastructure teams evaluating Animus Core for an order-risk check, market-data-triggered decision loop, or telemetry/surveillance path where microseconds are a real budget line, not a nice-to-have. If you're earlier in evaluation than that, start with the [Pilot Kit](../Pilot_Kit/PILOT_README.md) instead — free, no commercial commitment, and the natural on-ramp into this program once your team has its own baseline numbers in hand.

---

## 3. Four-Week Timeline

Each week builds on the last: correctness and baseline throughput first, then non-invasive live validation, then the tail-latency characterization that actually determines production fitness, then a formal decision point. No week is skipped or reordered — a tail-latency number without a correctness baseline underneath it isn't a number worth trusting.

### Week 1 — Historical Replay

**Goal: prove correctness and baseline throughput against your own data shapes, not synthetic telemetry.**

* Your team provides a representative slice of historical tick, order, or telemetry data (format and volume scoped in advance — no live system access required for this week).
* We replay it through the engine at full historical rate and at accelerated rate, validating field-for-field correctness (every event in, every event or matched signal out, byte-for-byte) and establishing a throughput baseline specific to your event shapes and sizes — not the synthetic 64-byte payloads in [`benchmarks/telemetry_benchmark.cpp`](../benchmarks/telemetry_benchmark.cpp).
* **Exit criterion:** 100% correctness on the replayed dataset, and a documented throughput baseline your team has independently verified, not just been shown.

### Week 2 — Shadow Tick Evaluation

**Goal: validate against live market data with zero risk to your production path.**

* Animus Core runs in shadow mode alongside your existing production pipeline, fed the same live (or near-live) market data feed, writing to nothing your execution path reads from.
* We compare signal timing, detection accuracy, and any registered rule/threshold behavior against your existing system's output on the same real-time data, in parallel — not as a replacement, and not touching your order path.
* **Exit criterion:** a shadow-mode comparison report your team can independently audit against your own production system's logs for the same window.

### Week 3 — Tail-Latency Analysis

**Goal: characterize p50/p99/p99.9/p99.99 on your own hardware, with the same rigor this project holds itself to internally.**

* We run the RDTSC-resolution, cross-core SPSC latency harness (the same one behind the [Cross-core SPSC dispatch latency](../BENCHMARK_DATASHEET.md#2-latency--throughput-profile) figures in the datasheet) directly on your target deployment hardware — not a reference machine.
* This includes the CPU-pinning tuning pass **and its honest limits**: [`BENCHMARKS.md` Phase 14](../AnimusCore_v1/BENCHMARKS.md) found, on real hardware, that thread pinning improves p50/p90 consistently but does *not* reliably improve p99.99 without OS-level core isolation — we bring that finding into your environment rather than promising a number pinning alone can't guarantee, and scope real isolation (`isolcpus`/`nohz_full`, Windows CPU Sets) if your SLA needs it.
* **Exit criterion:** a full percentile latency report against your own hardware and your own workload shape, with methodology fully disclosed — reproducible by your team, not taken on faith.

### Week 4 — Production Commercial Sign-off

**Goal: a formal decision point, not a sales call.**

* Joint review of all three weeks' results against the goals your team set at kickoff.
* If the numbers clear your bar: a proposed SLA (see §4) and a transition plan into a [Production Node license](../COMMERCIAL_OVERVIEW.md#4-commercial-engagement-tiers) — compiled binaries for your target platform(s), integration support, and priority defect response.
* If they don't: a written account of exactly where the gap is and why, so your team leaves with a real answer either way. A pilot that ends in "no, and here's precisely why" is a successful pilot, not a failed one.

---

## 4. SLA Reference Benchmarks

The figures below are what this project has **measured and published**, reproducibly, from source — see [`BENCHMARK_DATASHEET.md`](../BENCHMARK_DATASHEET.md) for full methodology and reproduction commands for every row. They are the reference thresholds this program is structured to validate — or refute — **against your own hardware in Week 3**, not a guarantee made in advance of that week. Per-hardware, per-workload variance is expected and is exactly what a Proof-of-Performance engagement exists to characterize honestly, not paper over.

| Metric | Reference figure | Source |
|---|---|---|
| Cross-core SPSC dispatch latency — p50 | 53.3 ns | `benchmarks/telemetry_benchmark.cpp`, RDTSC-resolution |
| Cross-core SPSC dispatch latency — p99 | 64.9 ns | same |
| Cross-core SPSC dispatch latency — p99.9 | 111.6 ns | same |
| Sustained SPSC throughput | 47.3M msgs/sec | same |
| Native decision-loop latency (tick-to-trade) — p50/p99 | 100 ns | `AnimusCore_v1/animus_benchmark_suite.cpp` |
| Python zero-copy interop — drain() only | ~4.5 ns/event (amortized) | `bindings/animus_py.cpp` (nanobind) |
| Python zero-copy interop — full decode | ~560 ns/event (amortized) | `animus/consumer.py` |

A production SLA proposed at Week 4 is written against **the numbers your own Week 3 run actually produced**, with the methodology and any tuning applied (core pinning, isolation, batch sizing) documented alongside it — not against the reference table above, which exists to set expectations going in, not to substitute for your own measurement.

---

## 5. Commercial Structure

* **Engagement fee:** scoped per desk based on data volume, hardware footprint, and integration surface (order-risk path vs. telemetry/surveillance-only). Contact your Animus Core representative for a quote — see [`COMMERCIAL_OVERVIEW.md`](../COMMERCIAL_OVERVIEW.md) for how to reach us.
* **What's included:** all four weeks of joint engineering time, the Week 3 hardware-specific latency report, and a written Week 4 recommendation regardless of outcome.
* **What happens next:** a successful sign-off transitions directly into one of the engagement tiers already defined in [`COMMERCIAL_OVERVIEW.md` §4](../COMMERCIAL_OVERVIEW.md#4-commercial-engagement-tiers) — typically **Production Node** (per-node hardware-locked license, compiled binaries, priority support) or, for desks needing deep customization or in-house builds, **Custom Source License**. The Institutional Pilot Program's engagement fee is credited toward the first year of whichever tier you move into.
* **No lock-in from the program itself:** if Week 4 doesn't clear your bar, the engagement ends there — there is no production commitment implied by having run the program.

---

*This document describes program structure and cites this repository's own published, reproducible benchmark figures — it is not itself a licensing agreement. A production engagement is governed by a separate commercial license agreement, not this document.*
