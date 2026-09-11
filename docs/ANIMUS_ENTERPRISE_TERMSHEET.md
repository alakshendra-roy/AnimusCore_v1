# Animus Core C++ Ingestion Engine — Institutional Master Software License Term Sheet

> ## ⚠️ DRAFT TERM SHEET — NOT REVIEWED BY COUNSEL — NON-BINDING
> This is an indicative commercial term sheet, not a definitive agreement. It is intended to summarize proposed terms for discussion and does not itself grant any license, create any payment obligation, or bind either party. A definitive Production Master Software License Agreement ("**MSLA**"), consistent with [`../LEGAL_EULA.md`](../LEGAL_EULA.md) and [`PILOT_CONTRACT.md`](PILOT_CONTRACT.md), governs the actual license and must be executed before any production deployment, invoicing, or IP transfer. This document has not been reviewed by a licensed attorney and must not be treated as final or enforceable until it has been.

---

## 1. Document Header & Identification

| Field | Detail |
|---|---|
| **Document ID** | ANM-TS-2026-v1 |
| **Effective Date** | [EFFECTIVE DATE] |
| **Expiration of Offer** | This term sheet expires **thirty (30) calendar days** from the Effective Date if not countersigned by Licensee, after which it may be withdrawn or revised by Licensor at its sole discretion. |
| **Licensor** | Animus Technologies Private Limited, a company incorporated under the Companies Act, 2013, having its registered office in India (CIN: [CIN — to be inserted upon issuance of the Certificate of Incorporation; see [`../LEGAL_INCORPORATION_BRIEF.md`](../LEGAL_INCORPORATION_BRIEF.md)]), operating under the "Animus Core" brand, represented by **Alakshendra Roy, Founder & Chief Architect** ("**Licensor**," "**Animus**") |
| **Licensee** | [CLIENT LEGAL ENTITY NAME], a [ENTITY TYPE] organized under the laws of [JURISDICTION] ("**Licensee**," "**Client**") |
| **Scope Classification** | Institutional Desk-Level Enterprise License — single designated trading unit/desk, as defined in §2.4 |

---

## 2. Commercial Structure & Payment Schedule

**2.1 Annual Commitment.** Licensee shall pay Licensor an annual license fee of **USD $240,000** per year, equivalent to **USD $20,000/month**, for the Scope defined in §2.4.

**2.2 Billing Options.**

| Option | Structure | Terms |
|---|---|---|
| **Option A** | 100% upfront, annual | Net-30 from invoice date |
| **Option B** | Quarterly advance | USD $60,000 per quarter, Net-15 from invoice date |

**2.3 Currency & Settlement.** All amounts are stated and payable in **USD**, by wire transfer via **SWIFT or Fedwire**. Licensee bears all wire, correspondent-bank, and currency-conversion fees; invoiced amounts are net of any such fees or withholding, subject to applicable tax treaty documentation (see [`W8BEN_GUIDE.md`](W8BEN_GUIDE.md)).

**2.4 Scope of Use.** The license covers **unlimited CPU cores, threads, and packet volumes** within Licensee's designated trading unit/desk, as identified in the definitive MSLA. There are no per-core, per-thread, or per-tick metering penalties within that scope. Use outside the designated trading unit/desk requires a separate license or scope amendment.

---

## 3. Technical Deliverables & Deliverable Artifacts

| Artifact | Description |
|---|---|
| **Hardened compiled binary** | `libanimus.so` (Linux x86_64), built with `-O3 -march=native -fno-rtti -fno-exceptions`; zero heap allocations on the hot path |
| **Header API** | `include/animus/engine.hpp`, Pimpl-isolated to protect proprietary lock-free ring-buffer layouts from the public interface |
| **Zero-copy Python Bridge** | Nanobind-based bridge mapping SPSC ring-buffer entries directly into NumPy `ndarray` views, no intermediate copy |
| **Benchmark Verification Harness** | `replay_bench` — synthetic packet driver for measuring bare-metal p50 / p90 / p99 / p99.9 latencies on Licensee's own hardware |

All artifacts are delivered under the license grant and restrictions of the definitive MSLA (§5). No source code is delivered under a Production license absent a separate, individually negotiated source-access and escrow arrangement.

---

## 4. Evaluation-to-Production Conversion Protocol

**Stage 1 — 30-Day Sandbox Proof-of-Performance (PoP).**
Licensee evaluates the engine using `animus_eval_v1.zip` under a Mutual Non-Disclosure Agreement, consistent with [`PILOT_AGREEMENT.md`](PILOT_AGREEMENT.md) / [`PILOT_CONTRACT.md`](PILOT_CONTRACT.md). No production use or fee obligation arises from Stage 1 alone.

**Stage 2 — Hardware-Verified Benchmark Acceptance.**
Using the `replay_bench` harness (§3), Licensee independently measures tail latency on its own representative hardware. Acceptance criterion: **p99 tail latency < 65 nanoseconds** under the agreed synthetic workload. Failure to meet this threshold on Licensee's hardware does not obligate conversion to Stage 3.

**Stage 3 — Conversion to Production MSLA.**
Upon Licensee's written acceptance following Stage 2, the Parties execute a definitive Production Master Software License Agreement incorporating the commercial terms in §2, the deliverables in §3, and the IP, support, and legal terms in §§5–7 below, refined as mutually agreed.

---

## 5. Intellectual Property, Source Protection & Escrow

**5.1 IP Retention.** Licensor retains complete and exclusive ownership of all intellectual property, algorithms, source code, and trade secrets embodied in the Animus Core engine, including the lock-free ring-buffer design, cache-line-aware memory layout, C-ABI, and benchmarking methodology. No rights transfer to Licensee beyond the limited use rights granted under the MSLA.

**5.2 Clean Room Certification.** Licensor certifies that the Animus Core engine is 100% original work product, developed without reference to, copying from, or incorporation of any GPL, copyleft, or otherwise incompatible third-party licensed source.

**5.3 Use Restrictions.** Licensee shall not reverse-engineer, decompile, disassemble, or attempt to derive source code from the delivered binary artifacts, except to the extent such restriction is unenforceable under applicable law. Redistribution, sublicensing, or provision of the Software or its outputs to any third party outside Licensee's designated trading unit/desk is prohibited absent Licensor's prior written consent.

**5.4 Source Code Escrow.** At Licensee's request and as a term of the definitive MSLA, Licensor will negotiate a source code escrow arrangement with an independent, mutually agreed escrow agent. Escrowed source would be releasable to Licensee solely upon (a) Licensor's bankruptcy, insolvency, or cessation of business, or (b) Licensor's uncured, material failure to provide contracted maintenance obligations under §6, following written notice and a cure period as defined in the MSLA. No escrow account exists as of the Effective Date of this term sheet; establishing one is subject to a separate escrow agreement and may carry additional fees to be agreed in the MSLA.

---

## 6. Service Level Agreement (SLA) & Production Support

**6.1 Incident Response.** Licensor will use **commercially reasonable efforts** to respond to critical trading-day incidents within **four (4) hours** during the operating windows of CME, Eurex, and LSE. This is a best-efforts target, not a guaranteed response-time SLA with associated service credits, reflecting that Licensor is a single-operator technical team; a tiered or credit-backed SLA can be scoped separately in the MSLA if Licensee requires one, potentially at additional cost to support backup/on-call coverage.

**6.2 Maintenance.** Licensor will deliver quarterly optimization rollouts, including AVX-512 vectorization updates and kernel/compiler compatibility updates, at no additional charge during the license term.

**6.3 Support Channel.** Support is provided via a dedicated private Slack Connect channel and priority developer email, staffed directly by Licensor's Founder & Chief Architect.

---

## 7. Execution, Governing Law & Signatures

**7.1 Governing Law; Arbitration.** This term sheet and any definitive MSLA entered pursuant to it are governed by the laws of **India**, without regard to conflict-of-laws principles, consistent with [`../LEGAL_EULA.md`](../LEGAL_EULA.md) §9.2 and [`PILOT_CONTRACT.md`](PILOT_CONTRACT.md) §8.1. Any dispute arising out of or relating to this term sheet or the MSLA shall be referred to and finally resolved by arbitration administered by the **Singapore International Arbitration Centre ("SIAC")** under the SIAC Rules then in force, seated in **Singapore**, before **one (1) arbitrator**, in the **English** language. Either Party may seek interim or injunctive relief from a court of competent jurisdiction pending constitution of the arbitral tribunal.

**7.2 Non-Binding Effect.** Except for this §7 and any confidentiality obligations under a separate NDA referenced in §4 Stage 1, this term sheet is an indicative summary of proposed terms and does not itself create a binding license, payment obligation, or exclusivity commitment. Binding obligations arise only upon execution of the definitive MSLA.

**7.3 Assignment.** Neither Party may assign this term sheet or the rights described herein without the other's prior written consent, except in connection with a merger, acquisition, or sale of substantially all assets.

---

## Signature Block

| | Licensor | Licensee |
|---|---|---|
| **Entity / Individual** | Animus Technologies Private Limited, by Alakshendra Roy | [CLIENT LEGAL ENTITY NAME] |
| **Signature** | ___________________________ | ___________________________ |
| **Name** | Alakshendra Roy | [PRINTED NAME] |
| **Title** | Founder & Chief Architect | [TITLE] |
| **Date** | ________ | ________ |
| **Notice Address** | [ANIMUS ADDRESS — registered office address to be added upon issuance of the Certificate of Incorporation, per `../LEGAL_INCORPORATION_BRIEF.md`] | [CLIENT ADDRESS] |

---

> ## ⚠️ REMINDER
> Do not send this term sheet to a prospective client or treat any term above as final until a licensed attorney has reviewed it — in particular §5.4's escrow mechanics (no escrow agent is currently under contract), §6.1's incident-response commitment (best-efforts language chosen deliberately given single-operator coverage — see below), §7.1's SIAC arbitration clause (jurisdiction-sensitive against a non-Indian counterparty), and §1's CIN placeholder pending Certificate of Incorporation issuance.
