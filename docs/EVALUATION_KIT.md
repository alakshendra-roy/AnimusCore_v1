# Animus Core — Institutional Evaluation Kit

**Classification:** Client-Facing Evaluation & Onboarding Document
**Audience:** Heads of Trading Technology, Systems Architects, and Latency Engineering teams beginning a technical evaluation of Animus Core.
**Companion documents:** [`../BENCHMARK_DATASHEET.md`](../BENCHMARK_DATASHEET.md) (the full, multi-layer performance reference this tear-sheet summarizes one slice of) · [`EVALUATION_GUIDE.md`](EVALUATION_GUIDE.md) (self-serve reproduction of the public-ABI benchmark layers) · [`PILOT_PROGRAM.md`](PILOT_PROGRAM.md) (the paid, four-week Institutional Pilot Program this kit is the on-ramp to) · [`PILOT_CONTRACT.md`](PILOT_CONTRACT.md) (the commercial contract governing that program) · [`../COMPLIANCE_AND_RISK_MITIGATION.md`](../COMPLIANCE_AND_RISK_MITIGATION.md) (the Reference Topology Appendix that scopes which of the figures below are contractually guaranteed vs. informative).

---

## 1. Executive Benchmark Tear-Sheet

> **Environment disclosure, read before comparing these numbers to anything else.** The two runs below were captured on a general-purpose Windows development host, **not** a CPU-isolated Linux bare-metal reference machine — no `isolcpus`/`nohz_full`/`rcu_nocbs` core isolation, no dedicated hugepages, no NUMA pinning. Per `../COMPLIANCE_AND_RISK_MITIGATION.md` §1.2, that makes these **informative, best-effort figures**, not the contractually guaranteed Reference Topology numbers `../COMMERCIAL.md` ties an order form's performance commitment to. Reproduce them on your own target hardware (§2 below) before relying on them for a sizing decision.

### 1.1 High-Throughput / Overwrite Telemetry

Decoupled SPSC overwrite mode (`--mode overwrite`) — the producer never waits on a consumer, matching the design intent for a market-data feed where the newest tick matters more than every historical one.

| Metric | Result |
|---|---|
| Events | 10,000,000 |
| **Throughput** | **16.127 M events/sec** (0.620 s wall) |
| **p50 (median)** | **35.1 ns** |
| **p90** | **36.8 ns** |
| **p99** | **47.9 ns** |
| p99.9 | 684.1 ns |
| max | 389,731.0 ns |
| Dropped (overwritten before consumption) | 8,951,424 / 10,000,000 (89.51%) |

*The 89.51% drop figure is expected, not a defect: this run has no consumer attached, so once the 1,048,576-slot ring fills (after roughly 1/9.5 of the run), every subsequent push overwrites an unread slot by design. This table measures raw producer-side enqueue latency and throughput under saturation — it is not a delivery-guarantee measurement. See §1.3 for when that distinction matters.*

*Source: `./build/bench-release/harness_benchmark --events 10000000 --capacity 1048576 --mode overwrite`, reproducible end-to-end via `./scripts/run_benchmarks.sh` (default `--events 10000000`).*

### 1.2 Zero-Loss / Backpressure Telemetry

Bounded-retry backpressure mode (`--mode backpressure`) with `benchmarks/consumer.py` attached and draining concurrently — the mode built for guaranteed message delivery rather than raw producer speed.

| Metric | Result |
|---|---|
| **Events delivered** | **10,000,000 / 10,000,000** |
| **Drops** | **0** |
| **Sequence gaps** | **0** |
| Data integrity | OK (monotonic, no repeats/reversals) |
| Producer p50 / p99 enqueue latency | 3,144.4 ns / 5,187.2 ns |
| Producer throughput (this configuration) | 0.356 M events/sec |

*Read the last two rows carefully: this run's throughput and latency are bounded by the pure-Python reference consumer's decode loop (`benchmarks/consumer.py`, ~0.31 M ticks/sec on its own), not by the native engine. That is the correct reading of a backpressure run — the producer is, by construction, only as fast as whatever is required to guarantee zero loss against whatever is consuming. Do not average this table against §1.1's: they measure different things (raw saturation speed vs. a delivery guarantee against a specific consumer), exactly the kind of methodology conflation `../BENCHMARK_DATASHEET.md` §2 explicitly warns against. A native C++ consumer in place of the Python reference implementation would close most of this throughput gap while keeping the zero-loss guarantee — that reproduction is a natural next step for your own evaluation, not something this kit asserts a number for.*

*Source: `harness_benchmark --mode backpressure` run concurrently with `python3 benchmarks/consumer.py`, both attached to the same named shared-memory segment — see §2.3 for the exact reproduction steps (this specific pairing is not wired into `run_benchmarks.sh` itself).*

### 1.3 Architectural Distinction — Overwrite vs. Backpressure

These are two deliberately different contracts over the same `ShmRing<T>` transport (`include/animus/shm_ipc.hpp`), not two implementations of the same thing:

| | Overwrite (`--mode overwrite`) | Backpressure (`--mode backpressure`) |
|---|---|---|
| Producer behavior when the ring is full | Overwrites the oldest unread slot and continues — never blocks | Bounded-retry `push_spin()`: waits for the consumer to free a slot, up to a retry budget |
| Delivery guarantee | None — newest data wins, oldest unread data is silently lost | Every event is delivered, provided the consumer keeps pace within the retry budget |
| Producer speed ceiling | Bounded only by the engine's own enqueue cost (§1.1) | Bounded by whichever is slower: the engine, or the attached consumer (§1.2) |
| Intended use case | Market-data / telemetry feeds where the latest tick matters more than a complete history — an order book snapshot, a live price feed | Anything where losing a message is a correctness bug, not a tolerable staleness — trade confirmations, risk-check signals, audit/compliance event streams |
| What a dropped event means operationally | Expected and by design under load — size the ring and choose overwrite deliberately for feeds where this is acceptable | A protocol violation — investigate the consumer's pace or the retry budget, don't treat it as normal |

**The wedge for evaluation:** pick the mode that matches what you're actually building before you compare numbers against a competitor's or your own in-house transport. A vendor benchmark quoting overwrite-mode throughput against your backpressure-mode requirement (or vice versa) is not a fair comparison — this kit deliberately runs and reports both so you don't have to take that distinction on faith.

---

## 2. Evaluation Welcome & Intake Protocol

Welcome — this section gets a systems engineer from a clean checkout to both benchmark modes above running on your own hardware, plus the license activation flow if your evaluation requires the opt-in, license-gated tuning features.

### 2.1 Prerequisites

| Requirement | Detail |
|---|---|
| **Platform** | Linux x86_64 bare-metal **recommended** for any figure you intend to rely on for a sizing decision — a shared, virtualized, or Windows development host (as used for §1's figures) will reproduce correct behavior but not the tightest tail latencies. See `../COMPLIANCE_AND_RISK_MITIGATION.md` §1.1's Reference Topology Appendix for the exact reference profile. |
| **Compiler / language standard** | The core engine targets **C++17** (`CLAUDE.md`); several benchmark harnesses (`benchmarks/telemetry_benchmark.cpp`) use C++23 language features. A **C++20-or-newer toolchain** — MSVC (VS 2022+), GCC 13+, or Clang 17+ with `libstdc++` — covers everything in this kit with headroom. |
| **Build tooling** | CMake 3.20+ and, optionally, Ninja (`run_benchmarks.sh`/`.ps1` auto-detect Ninja and fall back to the platform default generator otherwise). |
| **Python** | 3.8+ for the SDK/consumer layer (stdlib only — no third-party packages; see `CLAUDE.md`). The Linux `eval_kit/` turnkey tarball specifically wants 3.10+ (`EVALUATION_GUIDE.md` §1). |
| **Core isolation** (Linux, recommended for latency-sensitive runs) | Add `isolcpus=2,3 nohz_full=2,3 rcu_nocbs=2,3` to the kernel boot command line, then pin the producer/consumer to two **physical** cores on the same NUMA node (check `lscpu -e` — hyperthread siblings share L1/L2 and under-report contention). Full rationale and step-by-step in `eval-kit/README.md`'s "Core isolation notes." |
| **Hugepages** (Linux, general low-latency host tuning) | Disable transparent hugepage defrag (`echo never > /sys/kernel/mm/transparent_hugepage/defrag`) and, for a shared-memory-heavy workload, consider reserving explicit `hugetlbfs` pages to reduce TLB pressure on the ring's backing memory. **This is standard OS-level tuning advice, not a currently exposed Animus configuration flag** — the engine does not yet have a built-in hugepage-backed allocation mode; if your evaluation needs one, raise it with your Animus contact rather than assuming it exists. |

### 2.2 Running the Decoupled Saturation Test (`--mode overwrite`)

The one-command reproduction, which also runs a live telemetry snapshot mid-flight:

```bash
./scripts/run_benchmarks.sh --events 10000000
```

(There is no `--quick` flag — the script's full interface is `[--events N] [--capacity SLOTS] [--build-dir DIR]`; pass a smaller `--events` value for a faster pass.) This builds `harness_benchmark` in Release, runs the producer standalone, samples its shared-memory telemetry via `scripts/animus_stat.py` while it's still writing, and prints the throughput/latency report — this is exactly how §1.1's figures were produced.

### 2.3 Running Attached-Consumer Validation (`--mode backpressure`)

`run_benchmarks.sh` does not launch a consumer itself, and on Windows a named shared-memory segment does not reliably outlive the producer process the way a POSIX `/dev/shm` node does — so producer and consumer must be started **concurrently**, not sequentially. Minimal reproduction:

```bash
# Terminal / step 1 — start the producer in backpressure mode, in the background
./build/bench-release/harness_benchmark --name eval_ring --events 10000000 \
    --capacity 1048576 --mode backpressure --json result.json &

# Step 2 — once the segment exists, attach the consumer (same --name)
python3 benchmarks/consumer.py --name eval_ring --events 10000000 --idle-timeout-s 5.0

# Step 3 — wait for the producer and inspect its own report
wait
```

This is exactly the pairing that produced §1.2's figures. Expect `events consumed: 10000000`, `sequence gaps seen: 0`, and `data integrity: OK` on a correct run — anything else is worth investigating before you trust a zero-loss claim on your own hardware.

### 2.4 30-Day Offline RSA Evaluation License Key — Activation Flow

Core event ingestion **never requires a license** in either mode above. A license is only needed for opt-in, hardware-gated tuning features (CPU core-affinity pinning) — and the license mechanism itself is a two-party, fully offline handoff, per `Pilot_Kit/PILOT_README.md`:

1. **You** run the read-only fingerprint utility on the machine you want licensed — no network call, no private key involved:
   ```powershell
   powershell -ExecutionPolicy Bypass -File Pilot_Kit\get_fingerprint.ps1
   ```
   Send the printed 64-hex-character fingerprint to your Animus contact.
2. **Your Animus contact** signs a 30-day, hardware-fingerprint-bound `.lic` file against that fingerprint (RSA-2048, PKCS#1 signature over a SHA-256 digest) and returns it to you.
3. **You** pass the `.lic` file to your integration (e.g. `python Pilot_Kit/animus_integration_example.py path/to/your_pilot.lic`, or `animus_verify_license` directly). Verification happens entirely offline, in-process, against the compiled-in public key — no license server, no phone-home, ever.

> **Platform note:** `animus_verify_license` — the license-gating check itself — is currently **Windows-only** (per `README.md`'s Phase 17 notes). This does not affect core ingestion or either benchmark mode above on any platform; it only matters if your evaluation specifically needs the CPU-pinning feature the license gates, on a Linux target. Raise this with your Animus contact if that combination applies to you.

---

## 3. Contractual Pilot Bridge

The path from this kit to a production decision is: **free self-serve evaluation (this kit / `Pilot_Kit/`) → paid Institutional Pilot Program (`PILOT_PROGRAM.md`) → production license order form (`../COMMERCIAL.md`)**. The middle step has a direct, standing contract template:

> ### → [`PILOT_CONTRACT.md`](PILOT_CONTRACT.md) — Master Proof-of-Performance Pilot Agreement & Mutual NDA
>
> This is the commercial contract for the paid, four-week Institutional Pilot Program (`PILOT_PROGRAM.md`) — the 50%-upfront / 50%-on-delivery, PoP-fee-bearing engagement that produces a formal Production Commercial Sign-off recommendation on your own hardware and data. It is entered into by **Animus Technologies Private Limited** (India, CIN pending issuance of the Certificate of Incorporation — see `../LEGAL_INCORPORATION_BRIEF.md`), and is the single link a qualified prospect needs to move from "we've validated the numbers ourselves" (§1–2 above) straight to a scoped, signable commercial engagement.

**Before treating that link as execution-ready, note what `PILOT_CONTRACT.md` itself already discloses:** it is an AI-drafted structural template, explicitly marked **not yet reviewed by counsel**, with open blanks (PoP fee amount, the venue/arbitration forum, the liability-cap-vs-`PILOT_AGREEMENT.md` mutuality question tracked in `LEGAL_VERIFICATION_AUDIT.md`) that must be filled in and reviewed before a real counterparty signs it. The "1-click" property this bridge provides is *navigational* — one link takes a qualified prospect straight from this evaluation kit to the exact contract governing the next commercial step, with no separate document hunt — not a claim that the contract is pre-cleared for e-signature as-is. Route any prospect ready to convert to your Animus contact for that counsel-reviewed pass before countersignature.

---

*Prepared for institutional evaluators. Figures in §1 are reproducible end-to-end via §2 — if your own numbers diverge meaningfully, see `EVALUATION_GUIDE.md` §5's interpretation table before assuming either your hardware or the engine is at fault.*
