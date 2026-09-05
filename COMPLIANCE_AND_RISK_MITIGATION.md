# Animus Core — Institutional Compliance & Risk Mitigation Protocols

**Classification:** Internal Legal, Financial & Technical Operating Protocol
**Audience:** Founder, General Counsel, Procurement counterparties, cross-border tax advisors, and contracted engineering personnel.
**Companion documents:** [`LEGAL_EULA.md`](LEGAL_EULA.md) (governing license terms this document operationalizes) · [`COMMERCIAL.md`](COMMERCIAL.md) (tiering, SLA, and audit guarantees) · [`ARCHITECTURE.md`](ARCHITECTURE.md) (technical design of the shared-memory IPC layer referenced in Section 3) · [`COMMERCIAL_OVERVIEW.md`](COMMERCIAL_OVERVIEW.md) (business case and pilot mechanics).

---

This document does not create, modify, or supersede any contractual right or obligation. Where it restates a term also found in `LEGAL_EULA.md` or `COMMERCIAL.md`, the executed order form and that agreement's text govern; this document exists to make the *operational protocol* around those terms precise, repeatable, and auditable, not to substitute for legal drafting review.

---

## 1. Production SLA & Performance Topology Appendices

Sub-40ns latency is a property of a specific, documented hardware and OS configuration — not a universal guarantee. Every production order form must attach the Reference Topology Appendix defined below and scope the performance commitment to it explicitly.

### 1.1 Reference Topology Appendix

The appendix is a mandatory exhibit to every Enterprise Production License order form (`LEGAL_EULA.md` §3). It specifies the exact environment against which the sub-40ns p50 enqueue-latency figure (`COMMERCIAL.md` §2, `BENCHMARK_DATASHEET.md`) is contractually guaranteed:

| Requirement | Specification |
|---|---|
| Core isolation | Producer and consumer threads pinned to dedicated physical cores, excluded from the OS scheduler via `isolcpus` / `taskset` (Linux) or equivalent processor-affinity reservation (Windows) |
| NUMA topology | Producer, consumer, and the shared-memory ring buffer's backing pages resident on a single NUMA node; no cross-socket memory access on the hot path |
| Power management | CPU C-states above C0/C1 disabled; `intel_pstate`/`amd_pstate` frequency scaling disabled or pinned to maximum; Turbo Boost behavior fixed for run-to-run determinism |
| Network path | Kernel-bypass NIC (DPDK- or equivalent user-space driver-capable) where the deployment's data path includes network I/O; standard kernel networking stack is out of scope for the guaranteed figure |
| Virtualization | Bare metal, or a hypervisor configuration with CPU pinning, NUMA passthrough, and SR-IOV/PCI passthrough for the NIC — a shared, oversubscribed, or steal-time-exposed virtualized host does not qualify |
| Validation | Reproducible on the customer's own hardware via `scripts/run_benchmarks.sh` / `.ps1` against the methodology in `AnimusCore_v1/BENCHMARKS.md`, before the 30-day UAT clock in Section 1.3 starts |

The specific values above are the reference profile customers are onboarded against. The appendix executed with each order form additionally records the customer's actual measured configuration (core map, NUMA layout, NIC driver mode) at UAT sign-off, so "reference topology" is a verified fact of that deployment, not a boilerplate assertion.

### 1.1a Tier 4 Custom Hardware Environments — Engineering Sprint Addendum

Tier 4 order forms (`COMMERCIAL.md` §2, `LEGAL_EULA.md` §3.1) commonly involve custom hardware environments outside the reference profile above — customer-owned FPGA bitstreams, proprietary SmartNIC drivers, or bespoke kernel-bypass stacks built as part of the two dedicated annual engineering sprints included at that tier. The following governs the sub-40ns commitment in that context:

- The sub-40ns p50 enqueue-latency guarantee remains **strictly tied to the documented Reference Topology Appendix (Section 1.1)** regardless of contract tier. Tier 4's global deployment scope and dedicated engineering sprints do not, by themselves, extend that guarantee to a customer's custom FPGA/SmartNIC environment.
- A custom hardware environment is only brought within a contractual performance commitment when it has been **co-designed and validated under an executed engineering sprint addendum** — a scoped exhibit, signed by both Parties, that documents the specific FPGA bitstream, SmartNIC driver, or kernel-bypass configuration measured, the methodology used, and the resulting latency figure warranted for that environment specifically.
- Absent such an addendum, performance on a Tier 4 customer's custom hardware is governed by the Section 1.2 best-effort operational band below, identical to any non-reference topology at any tier — Tier 4 pricing and support SLA do not implicitly upgrade an unvalidated custom environment to a guaranteed figure.
- Each engineering sprint addendum executed under a Tier 4 order form is retained alongside that customer's Reference Topology Appendix and UAT Sign-Off exhibit (Section 1.3), so the full set of hardware environments under contractual guarantee for that customer is a single auditable record, not scattered across sprint engagement notes.

### 1.2 Non-Reference & Virtualized Topology — Best-Effort Operational Band

Where a customer's production environment deviates from Section 1.1 — shared cores, cross-socket NUMA, standard kernel networking, or a virtualized host without CPU/NIC passthrough — the following contract language applies:

> Performance on any deployment topology other than the Reference Topology Appendix executed under this order form is provided on a **commercially reasonable, best-effort basis** and is not a Service Level commitment. Latency, jitter, or throughput figures observed on a non-reference topology falling outside the operational band separately documented for that topology **do not, standing alone, constitute a breach of this Agreement** and are excluded from the SLA remedies in the order form's Support & SLA schedule.

This clause is what prevents the single most common infrastructure-vendor failure mode: a lab number becoming an implicit universal promise. It does not weaken the guarantee on qualifying hardware — it scopes it to hardware capable of delivering it.

### 1.3 Mandatory 30-Day Staging/Canary UAT Protocol

No production order form authorizes live trading execution or live robotic control on first deployment. The following gate is mandatory contract language, cross-referenced from `LEGAL_EULA.md` §3 (Conversion to Commercial License):

1. **Staging deployment (Days 1–20).** The engine is deployed against replayed or synthetic data on the customer's actual target hardware, validating the Reference Topology Appendix measurements in a non-live environment.
2. **Canary/shadow execution (Days 21–30).** The engine runs in parallel with the customer's existing production path — receiving live data, producing live output — without its output being the system of record for trading or actuation decisions.
3. **Sign-off.** Both parties execute a UAT Sign-Off exhibit recording the measured topology, observed p50/p99 latencies, and any deviations from Section 1.1, before the order form's SLA clock and liability terms in Section 1.4 become operative for live execution.

A customer may waive Phase 2 in writing for a well-understood, low-risk deployment, but the waiver must be an affirmative signed exhibit — never an implied consequence of scheduling pressure.

### 1.4 Absolute Liability Cap

Per `LEGAL_EULA.md` §7(b), Vendor's cumulative aggregate liability arising out of any order form is capped at **100% of license fees actually paid by Customer in the twelve (12) months preceding the event giving rise to the claim**. This document does not create a separate or additional cap — it restates the existing EULA term as a mandatory, non-negotiable line item in every order form's risk-allocation section, so it is never silently dropped during redlines. Any customer-proposed removal or uncapping of this term is treated as a material deviation requiring founder and counsel sign-off before countersignature, not a standard negotiation point.

---

## 2. Cross-Border Tax & Cross-Jurisdiction Invoicing Protocols (India ↔ US/EU)

### 2.1 Payment Characterization — "Standardized Commercial Software License Fee"

Every order form and invoice issued to a US or EU customer characterizes the payment as a fee for a **standardized, non-exclusive, non-transferable right to use** a defined copy of the Software (`LEGAL_EULA.md` §2.1, §5.1) — explicitly **not**:

- a technical service or fee for included services, and
- a transfer or licensing of copyright (reproduction, adaptation, distribution, or public-performance rights) in the underlying work.

This characterization tracks the Indian Supreme Court's holding in *Engineering Analysis Centre of Excellence Pvt. Ltd. v. CIT* (2021) — payments for the use of a copyrighted *article* (a licensed copy of software) rather than the underlying *copyright* itself do not constitute "royalty" under India's tax treaties, including the India–US DTAA. Correct characterization is a drafting decision made at contract execution, not a position taken retroactively at audit time:

- Order forms and invoices use "License Fee" terminology exclusively, never "consulting," "services," "royalty," or "IP transfer."
- No order form grants source-code modification, sublicensing, or redistribution rights outside the explicit Tier 3 OEM Distribution Agreement carve-out (`COMMERCIAL.md` §2) — those instruments require separate tax analysis before execution, as a distribution right shifts the characterization risk.
- This position is a starting framework, not a substitute for a jurisdiction-specific opinion from qualified Indian and counterparty-country tax counsel before the first cross-border invoice of a given fact pattern is issued.

### 2.2 Mandatory Pre-Invoice Compliance Filings

No invoice is issued to a foreign customer before the following are on file:

| Filing | Purpose | Filed by | Cadence |
|---|---|---|---|
| **Form 10F** | Indian-prescribed self-declaration supplementing the Tax Residency Certificate, required to claim DTAA benefit on Indian-source cross-border receipts | Vendor (India) | Annually, per Indian financial year |
| **Tax Residency Certificate (TRC)** | Evidence of Indian tax residency, required by the counterparty's withholding agent to apply treaty (not statutory) withholding rates | Vendor, obtained from Indian tax authority | Annually |
| **Form W-8BEN-E** (US customers only) | Certifies foreign-entity status and claims India–US DTAA benefit to the US customer's withholding agent, avoiding default 30% NRA withholding under IRC Chapter 3 (§§1441/1442) on FDAP income | Vendor, delivered to each US customer before first payment | Valid 3 calendar years from signature, or until a change in circumstances |
| **EU counterparty equivalent** | Certificate/self-declaration required under the applicable bilateral India–[Member State] DTAA (e.g., India–Germany, India–Netherlands, India–Ireland) — instrument varies by country | Vendor, per counterparty jurisdiction | Per that treaty's renewal terms |

For a properly characterized License Fee (Section 2.1) with a filed TRC/Form 10F/W-8BEN-E, most India–US/EU treaties apply a reduced royalty article rate or, where the fee is properly sourced as business profits absent a US/EU permanent establishment, no withholding at all — as opposed to the 30% default US statutory rate that applies when no treaty claim is on file. This is the single highest-leverage filing in the entire cross-border pipeline and is treated as a pre-invoice blocker, not a follow-up item.

### 2.3 RBI Export Compliance Pipeline

Every export invoice to a non-resident customer moves through this pipeline, tracked to closure per invoice — an open item here blocks recognition of that invoice's revenue in the waterfall accounting the founder's financial model depends on:

1. **LUT under GST (Rule 96A).** A Letter of Undertaking is filed and renewed annually with GST authorities, permitting export invoices to be issued **zero-rated** (no IGST charged, no upfront tax-then-refund cycle) — mandatory before the first cross-border invoice of each financial year.
2. **SOFTEX filing.** Each software export is declared via SOFTEX (through STPI or the self-certification route, as applicable), the RBI-mandated declaration mechanism specific to software exports, independent of the general goods/services export documentation.
3. **EDPMS entry.** The SOFTEX declaration feeds the RBI's Export Data Processing and Monitoring System, which tracks the invoice against realized payment. An invoice with no corresponding realization entry ages into an RBI compliance exception.
4. **FIRC generation.** Upon receipt of payment, the Authorized Dealer (AD) bank issues a Foreign Inward Remittance Certificate, which closes the EDPMS entry and is the documentary proof of export-proceeds realization required for both RBI compliance and the Section 2.2 tax filings above.
5. **Realization window.** Export proceeds must be realized (steps 3–4 completed) within the FEMA-prescribed period from invoice date — currently nine (9) months, subject to RBI's then-current regulations — or an extension must be sought from the AD bank before the window lapses.

A standing CA/FEMA compliance retainer sits inside the Corporate Reserve & Legal/Compliance Pool of the founder's capital-allocation waterfall specifically to run this pipeline continuously per invoice, rather than reactively once a customer's finance team or the AD bank raises an exception.

---

## 3. Source Code Isolation & Contractor IP Protection

### 3.1 Zero-Trust Repository Architecture

The core lock-free ring buffer and low-latency hot-path implementation (`include/animus/shm_ipc.hpp`'s `ShmRing<T>` / `SpmcRing<T>` and their supporting concurrency primitives, per `ARCHITECTURE.md`) is segmented into a **protected, private submodule** with access restricted to the founder and any core engineer under a full IP-assignment agreement (Section 3.2). No contractor or integrator is granted access to this submodule as a matter of course, regardless of project deadline pressure.

External contractors and integrators — including those retained for the Tier 2/3 wire-schema and integration engineering described in `COMMERCIAL.md` §2 — receive access strictly to:

- The **public ABI headers** (`AnimusCore_v1/animus.hpp`) defining the stable C-ABI surface;
- **Integration stubs and mock implementations** sufficient to build and test against the public interface without the hot-path implementation;
- **Test harnesses** (`scripts/run_benchmarks.sh` / `.ps1`, `tests/`) that exercise the compiled binary rather than the source of the ring buffer itself.

| Access tier | Repository scope | Personnel |
|---|---|---|
| Core | Private submodule (ring buffer, lock-free hot path, license-verification internals) | Founder; core engineers under Section 3.2 IP assignment, reviewed quarterly |
| Integration | Public ABI headers, stubs, test harnesses, public repository | Retained contractors, integration partners |
| Evaluation | Compiled binaries + `.lic` file only | Prospective customers under `LEGAL_EULA.md` §2 |

Git access at the Core tier is logged and reviewed quarterly; a contractor never transits from Integration to Core tier without an executed Section 3.2 agreement predating, not following, any access grant.

### 3.2 Contractor IP Assignment & NDA — Standard Template

Every contractor signs the following before repository access of any kind is granted — access provisioning and agreement execution are sequenced so the former is technically impossible without the latter:

> **Intellectual Property Assignment & Non-Disclosure Agreement**
>
> This Agreement is entered into between **Alakshendra Roy**, sole proprietor of Animus Core ("**Company**"), and **[CONTRACTOR NAME]** ("**Contractor**").
>
> **1. Assignment of Work Product.** Contractor irrevocably assigns to Company, effective upon creation, all right, title, and interest — including all copyright and other intellectual property rights, worldwide — in and to any code, design, documentation, or other work product created in connection with engagement by Company, whether or not such work product is ultimately incorporated into a released version of the Software. This assignment is made under Sections 18 and 19 of the Copyright Act, 1957 (India), covering both present and future works, and is executed in writing as those sections require for a valid assignment.
>
> **2. Waiver of Moral Rights.** To the extent permitted by applicable law, Contractor waives any moral rights in the assigned work product as against Company and its successors and licensees.
>
> **3. Confidentiality.** Contractor shall hold in confidence, and not disclose to any third party, any source code, architecture, benchmark methodology, or business information of Company disclosed in the course of the engagement, using no less than a reasonable standard of care, for so long as such information remains non-public.
>
> **4. Repository Access Scope.** Contractor acknowledges that repository access is granted strictly per the tier defined in the engagement's statement of work, and that accessing, copying, or retaining any repository content outside that scope — including the Core-tier submodule described in Section 3.1 of `COMPLIANCE_AND_RISK_MITIGATION.md` — is a material breach of this Agreement independent of any separate breach of the underlying services agreement.
>
> **5. Return/Destruction on Termination.** Upon termination of the engagement for any reason, Contractor shall immediately cease all access to Company repositories, and shall destroy or return, at Company's election, all copies of any Company source code, credentials, or confidential materials in Contractor's possession or control.
>
> **6. Governing Law.** This Agreement is governed by the laws of India.
>
> *Executed by:* ___________________________ (Contractor) &nbsp;&nbsp; *Date:* ___________
> *Accepted by:* ___________________________ (Alakshendra Roy, Company) &nbsp;&nbsp; *Date:* ___________

This template is a starting instrument only — it must be reviewed by qualified counsel before use with a specific contractor, particularly where the contractor is engaged through a foreign entity or is not an Indian tax resident, which introduces additional cross-border IP-assignment enforceability questions beyond the scope of this document.

### 3.3 Evaluation Binary & RSA Key Watermarking

Every issued evaluation artifact is traceable to the specific recipient it was issued to, without relying on the recipient's cooperation to establish provenance if it leaks:

- **License payload binding.** Per `LEGAL_EULA.md` §2.2 and `COMMERCIAL.md` §3.3, every `.lic` file is already RSA-2048-signed and bound to a specific Hardware Fingerprint; this binding is extended so the signed payload also carries an opaque per-recipient identifier correlated, in an internal issuance ledger only, to the customer name and issuance date. The identifier is not derivable from the license file alone, so it survives a leak without handing the leaker a map of the traceability mechanism itself.
- **Binary build tagging.** Evaluation binaries issued under `LEGAL_EULA.md` §2 are built with a unique, per-issuance build identifier embedded in a non-obvious location in the compiled artifact (not a visible `--version` string), logged against the same issuance ledger, so a binary recovered outside its authorized recipient's environment can be traced to the specific evaluation grant it originated from.
- **Issuance ledger.** The founder (or, at scale, the designated compliance function) maintains a single source-of-truth ledger mapping: recipient identity → Hardware Fingerprint → issuance date → per-recipient identifier → binary build tag. This ledger is the forensic reference point for any leak investigation and is retained for no less than the term of the applicable `LEGAL_EULA.md` Agreement plus three (3) years.
- **Non-disclosure of mechanism.** The specific watermarking technique in use is not documented in any customer-facing material, including this document's companions — a leak-traceability mechanism whose method is public is materially weaker than one that is not, and this section is deliberately schematic rather than an implementation specification for that reason.

---

## 4. Relationship to Governing Agreements

This document is an internal operating protocol, not a contract. It governs how `LEGAL_EULA.md` and `COMMERCIAL.md` terms are implemented consistently across deployments, customers, and jurisdictions. Where any provision here appears to conflict with the executed text of `LEGAL_EULA.md`, a signed order form, or a jurisdiction-specific legal or tax opinion obtained under Sections 1–2 above, **the executed agreement and the qualified professional opinion govern**, and this document is updated to match — never the reverse.

---

> ## ⚠️ REMINDER
> This document is a founder-authored operational framework, not legal, tax, or regulatory advice. Before relying on any protocol above in an actual customer engagement:
> - Section 1 (SLA/topology/liability language) must be reviewed by counsel licensed in the governing jurisdiction selected under `LEGAL_EULA.md` §9.2 before being incorporated into a binding order form.
> - Section 2 (DTAA characterization, withholding, and FEMA/RBI pipeline) must be reviewed by a practicing Indian Chartered Accountant and, for a given customer's jurisdiction, local tax counsel in that jurisdiction — treaty positions and withholding mechanics are fact-specific and change with each counterparty's country and evolving case law.
> - Section 3.2's template is a drafting starting point only and must be reviewed by counsel before execution with any specific contractor, particularly a non-Indian-resident contractor.
