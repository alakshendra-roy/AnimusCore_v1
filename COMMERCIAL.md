# Animus Core — Enterprise Commercial Licensing & Production Deployment

**Classification:** Enterprise Commercial & Licensing Specification
**Audience:** General Counsel, Procurement, Heads of Infrastructure, and Engineering Leadership evaluating Animus Core for production deployment.
**Companion documents:** [`COMMERCIAL_OVERVIEW.md`](COMMERCIAL_OVERVIEW.md) (business case, ROI, and self-serve pilot mechanics) · [`LEGAL_EULA.md`](LEGAL_EULA.md) (governing license terms) · [`BENCHMARK_DATASHEET.md`](BENCHMARK_DATASHEET.md) / [`AnimusCore_v1/BENCHMARKS.md`](AnimusCore_v1/BENCHMARKS.md) (verified performance) · [`ARCHITECTURE.md`](ARCHITECTURE.md) (technical design of the shared-memory IPC layer this document licenses) · [`docs/PILOT_PROGRAM.md`](docs/PILOT_PROGRAM.md) (paid Institutional Pilot Program).

---

## 1. Dual-Licensing Architecture

Animus Core is distributed under two distinct entitlement models. The dividing line is not the size of the deployment — it is whether the deployment is production and revenue-bearing. A single-node evaluation and a single-node production execution path place identical load on the engine; only their licensing obligation differs.

### 1.1 Community Edition / Eval Kit

The Community Edition — distributed as the [`Pilot_Kit/`](Pilot_Kit/) source evaluation and the [`eval_kit/`](eval_kit/) turnkey Linux tarball (see [`COMMERCIAL_OVERVIEW.md` §3](COMMERCIAL_OVERVIEW.md)) — is a **permissive, hardware-locked evaluation grant**, not a production entitlement. It is intended for:

- Local research and architectural due diligence — reading, building, and running the engine against synthetic or replayed data on your own hardware.
- Non-production academic and benchmarking use — reproducing the throughput/latency figures in `BENCHMARK_DATASHEET.md` and `AnimusCore_v1/BENCHMARKS.md` via the deterministic harness at [`scripts/run_benchmarks.sh`](scripts/run_benchmarks.sh) / [`scripts/run_benchmarks.ps1`](scripts/run_benchmarks.ps1), coursework, and internal engineering evaluation.
- Time-boxed technical proof-of-concept work ahead of a licensing decision (Section 4 below).

It is issued as a 30-day, hardware-fingerprint-bound `.lic` file (RSA-2048-signed, offline-verified — Section 3.3), per [`LEGAL_EULA.md` §2](LEGAL_EULA.md). Core telemetry ingestion is unrestricted under this grant; only opt-in tuning features (e.g., CPU core-affinity pinning) are license-gated, and they fail closed rather than falling back to unlicensed default behavior. What the Community Edition **does not** grant — per `LEGAL_EULA.md` §2.1(c) and §4(e) — is production use, resale, redistribution, or use in delivering any service to a third party.

### 1.2 Enterprise Production License

An Enterprise Production License is legally required — not merely recommended — wherever Animus Core moves from evaluation into any of the following:

- **Live execution nodes** — any deployment where the engine's rule evaluation, signal dispatch, or shared-memory ring is on the critical path of a real decision loop (order risk checks, execution monitoring, fraud/anomaly response) rather than a replay or shadow harness.
- **Proprietary order flow routing** — any use where `animus::ExecutionClient` / the broker-execution interop path (`ARCHITECTURE.md`, `AnimusCore_v1/animus.hpp`) touches real order flow, own-account or client-facing.
- **Commercial robotics telemetry pipelines** — any use of the `ShmRing<T>` / `SpmcRing<T>` cross-process transport (`include/animus/shm_ipc.hpp`) as the telemetry or control backbone of a commercial or customer-deployed robotics or autonomous-systems platform.
- **Any revenue-generating multi-process IPC runtime** — the general case the three bullets above instantiate: if the shared-memory ring is carrying data that revenue, a customer commitment, or a regulatory obligation depends on, the deployment has left the scope of Section 1.1.

Enterprise Production Licenses are node-bound (Section 3.3), tiered by deployment scale and integration depth (Section 2), and governed by a signed order form under `LEGAL_EULA.md` §3 — the Evaluation Agreement alone does not create a production right.

### 1.3 Edition Comparison

| Dimension | Community Edition / Eval Kit | Enterprise Production License |
|---|---|---|
| Permitted use | Internal evaluation, research, benchmarking, academic use | Live production execution, order flow, revenue-generating IPC |
| Term | 30 days, non-renewable without conversion | Annual or multi-year, renewable, per signed order form |
| Cost | No fee | Per Section 2 tiering; scoped per order form |
| Ring buffer entitlement | Unrestricted for evaluation workloads | Tiered — see Section 2 |
| Redistribution rights | None | None below Tier 3; Tier 3 grants a separate redistribution right |
| Source access | Public repository / `eval_kit` binaries only | Available under NDA at Tier 2+, or a negotiated Custom Source License |
| Support | Community / best-effort | Tiered SLA — Section 2 |
| License mechanism | RSA-2048 node-bound `.lic`, 30-day expiry | RSA-2048 node-bound `.lic`, term per order form |

---

## 2. Enterprise Tiering & Commercial Packaging

Production entitlement is segmented into three tiers by deployment scale and engineering scope. Each tier is cumulative: Tier 2 includes everything in Tier 1 at greater scale, and Tier 3 adds a distribution right that neither Tier 1 nor Tier 2 grants.

| | **Tier 1 — Production Core** | **Tier 2 — Institutional Scale** | **Tier 3 — OEM / Embedded** |
|---|---|---|---|
| **Deployment scope** | Single firm, single legal entity | Single firm, unlimited internal deployment scale | Redistribution into a third party's product |
| **Ring buffer entitlement** | Up to 8 dedicated SHM rings per licensed node | Uncapped ring buffers, uncapped licensed nodes | As negotiated in the OEM Distribution Agreement |
| **Engineering scope** | Standard integration support against the documented API | Custom C++/Python wire schema engineering (`ANIMUS_DEFINE_SCHEMA`-registered schemas, `ARCHITECTURE.md` §3.1) beyond `ExecutionEvent`; high-throughput FPGA/NIC kernel-bypass architecture consulting | Integration engineering for embedding the runtime into a proprietary appliance or platform image |
| **Support SLA** | Standard business-hours response | Direct 4-hour critical-issue SLA | Per OEM order form; typically matched to Tier 2 for the OEM's own engineering team |
| **Performance commitment** | Sub-40ns verified per-event enqueue latency (p50) on the shared-memory producer path — reproducible via `scripts/run_benchmarks.sh` / `.ps1` against `AnimusCore_v1/BENCHMARKS.md` Phase 29's methodology, not a figure taken on faith | Same verified baseline, plus a workload-specific tail-latency characterization as part of the engineering engagement | Per OEM order form and target hardware profile |
| **Redistribution rights** | None — internal production use only (`LEGAL_EULA.md` §4(b)) | None — internal production use only | **Granted** — redistributable binary runtime for embedding into proprietary trading appliances or autonomous robotics platforms, under a separately negotiated OEM Distribution Agreement that supersedes the standard no-redistribution restriction for the scope defined in that agreement |
| **Source access** | Not included; available as a separate Custom Source License (`COMMERCIAL_OVERVIEW.md` §4) | Available under NDA for internal security audit purposes (Section 3.2) | Available under NDA as required for OEM integration and audit |
| **Typical customer profile** | A single desk or engineering team deploying one production pipeline | A firm running Animus Core across multiple desks, strategies, or production lines, or requiring bespoke wire-format engineering | An appliance vendor, prime, or platform integrator shipping Animus Core inside their own product |

All three tiers issue as RSA-2048-signed, hardware-fingerprint-bound `.lic` tokens (Section 3.3); the tier and its entitlements (core count, ring-buffer ceiling, expiry) are encoded in the signed token and enforced fail-closed by `animus_verify_license` — there is no unlicensed default behavior for a gated feature to fall back to.

---

## 3. SLA, Compliance & Audit Guarantees

### 3.1 Zero Telemetry Callback Guarantee

**The engine runs fully offline, air-gapped, with no outbound network call anywhere in its operation.** This is a structural property of the design, not a configuration flag that could be silently disabled:

- The transport layer itself (`ShmRing<T>` / `SpmcRing<T>`, `include/animus/shm_ipc.hpp`) is OS-local shared memory (`/dev/shm` on Linux, a named file mapping on Windows — `ARCHITECTURE.md` §1.1). There is no socket, no message broker, and no remote endpoint anywhere in the hot path by construction.
- License verification (`animus_verify_license`, `AnimusCore_v1/animus.hpp` / `animus_engine.cpp`) validates the signed `.lic` file entirely in-process against the compiled-in public key (`AnimusCore_v1/animus_license_pubkey.hpp`) — RSA-2048 PKCS#1 signature verification over a SHA-256 digest, performed locally, with no license server dependency and no network call at any point in the verification path (`LEGAL_EULA.md` §2.2).
- The SDK ships no analytics, crash-reporting, or usage-telemetry callback of its own.

This guarantee is a direct requirement of the deployment contexts Section 1.2 describes: an execution node or an air-gapped robotics platform cannot depend on infrastructure that phones home.

### 3.2 Deterministic Release Validation

Release artifacts are **SHA-256 checksummed and independently reproducible**, not asserted on trust:

- Every tagged release publishes a SHA-256 checksum for its distribution artifact — see `AnimusCore_v1/BENCHMARKS.md`'s v1.2.0 release entry, where the published checksum was independently recomputed from a fresh download and matched byte-for-byte before being recorded.
- The performance figures cited throughout this document and its companions are reproducible on demand, not held as internal-only numbers: `scripts/run_benchmarks.sh` (Linux/macOS) and `scripts/run_benchmarks.ps1` (Windows) build the benchmark harness in Release configuration and reproduce the throughput/latency/telemetry figures in one command, against the current source tree, on the customer's own hardware.
- **Source access for internal security audit** is available to Tier 2 and Tier 3 customers under NDA, and more broadly under a negotiated Custom Source License (`COMMERCIAL_OVERVIEW.md` §4) — a customer deploying Animus Core on an execution or robotics-control path is not required to take the engine's memory-safety and concurrency claims (`ARCHITECTURE.md` §2) on faith; the source is available to verify them directly.

### 3.3 License Keying

Production entitlement is enforced by **cryptographic, node-bound, offline license tokens — no phone-home, ever**:

- A `.lic` file is bound to a **Hardware Fingerprint**: the SHA-256 digest of the licensed machine's `MachineGuid` and primary network adapter MAC address, generated by a read-only, no-network-call utility the customer runs and controls (`get_fingerprint.ps1` or equivalent) — the fingerprint is sent to the vendor for signing, never the reverse.
- The token itself is **RSA-2048-signed** and carries the entitlement it grants (core count, tier, expiry) in its signed payload; verification is a local signature check against the embedded public key, with no license server, no periodic re-validation call, and no network dependency of any kind.
- Enforcement is **fail-closed**: a missing, expired, or fingerprint-mismatched license disables the gated feature rather than falling back to an unlicensed default — there is no silent degraded-but-functional state to audit for.

| Guarantee | Mechanism | Verifying Artifact |
|---|---|---|
| Zero telemetry callback | OS-local shared memory transport; fully offline license verification | `ARCHITECTURE.md` §1.1; `LEGAL_EULA.md` §2.2 |
| Deterministic release validation | SHA-256 checksums per release; reproducible benchmark harness; NDA source access | `AnimusCore_v1/BENCHMARKS.md` (v1.2.0 checksum); `scripts/run_benchmarks.{sh,ps1}` |
| License keying | RSA-2048-signed, hardware-fingerprint-bound `.lic` tokens; fail-closed enforcement | `AnimusCore_v1/animus_license_pubkey.hpp`; `AnimusCore_v1/license_tools/` |

---

## 4. Enterprise Procurement & Contact

### 4.1 Structured Procurement Intake

A production licensing engagement begins with a short technical and commercial intake, not a sales call. Prospective customers should be prepared to describe:

1. **Deployment context** — which of the Section 1.2 categories applies (execution node, order flow routing, robotics telemetry, or another revenue-generating IPC runtime).
2. **Scale** — expected node count and ring-buffer count per node, to pre-qualify a Tier 1 vs. Tier 2 fit (Section 2).
3. **Integration surface** — C++ direct integration, the Python SDK, or a custom wire schema requirement (a Tier 2 signal).
4. **Redistribution intent** — whether Animus Core will be embedded into a product shipped to a third party (a Tier 3 / OEM signal, triggering the separate OEM Distribution Agreement track).
5. **Timeline and target hardware** — to scope the POC engagement below.

### 4.2 Rapid POC Engagement Framework — 2-Week Scoped Evaluation

Intake routes into a **two-week, scoped technical engagement** — distinct from, and a precursor to, both the free self-serve Pilot Kit (Section 1.1) and the paid four-week Institutional Pilot Program (`docs/PILOT_PROGRAM.md`):

| Week | Activity |
|---|---|
| **Week 1** | Intake finalized; hardware fingerprint collected and a scoped evaluation `.lic` issued (Section 3.3); Community Edition installed against the customer's target integration surface. |
| **Week 2** | Integration validated against a representative (not necessarily production) workload; tier fit assessed against Section 2; a formal tiering and pricing proposal delivered. |

The output of this engagement is a decision point, not a production commitment: customers requiring the deeper, data-driven validation described in `docs/PILOT_PROGRAM.md` (historical replay, shadow-live evaluation, tail-latency characterization on target hardware) proceed into that paid four-week program before any production order form is executed. This two-week engagement exists specifically to make that next decision — self-serve Pilot Kit, full Institutional Pilot Program, or direct-to-order-form for a well-scoped Tier 1 deployment — an informed one on both sides.

### 4.3 NDA & Custom Pilot Terms

- A mutual NDA is available, and standard, prior to any source-access review or architecture deep-dive under Section 3.2.
- Custom pilot terms (data handling for historical/shadow-live evaluation, on-site vs. remote engagement, and specific deliverables) are memorialized in a pilot order form that incorporates the general terms of `LEGAL_EULA.md` by reference, per that agreement's Section 3 — no fee, production, or redistribution obligation is created by intake or scoping conversations alone.

### 4.4 Contact

| | |
|---|---|
| **Procurement / commercial inquiries** | royrichie006@gmail.com |
| **Security / audit inquiries (Section 3.2)** | royrichie006@gmail.com |
| **Technical inquiries / issues** | [GitHub Issues](https://github.com/alakshendra-roy/AnimusCore_v1/issues) — the repository's issue tracker |
| **Licensing entity** | Alakshendra Roy, Founder — India; see `LEGAL_EULA.md` for the governing definition |
| **OEM / Tier 3 distribution inquiries** | Route through Procurement above; scoped separately per Section 2 |

*Contact and entity details above are current. The substantive commercial license terms they route into — pricing, SLA response times, and the liability/warranty terms in `LEGAL_EULA.md` — are established by a signed order form under that agreement.*
