# Animus Core C++ Ingestion Engine — Desk License Term Sheet

> ## ⚠️ DRAFT TERM SHEET — NOT REVIEWED BY COUNSEL — NON-BINDING
> This is an indicative commercial term sheet, not a definitive agreement. It is intended to summarize proposed terms for discussion and does not itself grant any license, create any payment obligation, or bind either party. A definitive **Desk License Agreement ("DLA")**, consistent with [`../LEGAL_EULA.md`](../LEGAL_EULA.md) and [`PILOT_CONTRACT.md`](PILOT_CONTRACT.md), governs the actual license and must be executed before any production deployment, invoicing, or IP transfer. This document has not been reviewed by a licensed attorney and must not be treated as final or enforceable until it has been.
>
> **This is a new tier, drafted alongside — not in place of — [`ANIMUS_ENTERPRISE_TERMSHEET.md`](ANIMUS_ENTERPRISE_TERMSHEET.md).** The Desk License is a narrower-scope, lower-commitment offering than the Institutional Enterprise tier; it is not a discounted version of that tier's terms. See §2.4 for the scope boundary and §8 for how a desk outgrowing this tier upgrades. See [`../docs/ASYNC_EVALUATION_PLAYBOOK.md`](ASYNC_EVALUATION_PLAYBOOK.md) §3 for the sales process this term sheet is issued from.

---

## 1. Document Header & Identification

| Field | Detail |
|---|---|
| **Document ID** | ANM-DL-TS-2026-v1 |
| **Effective Date** | [EFFECTIVE DATE] |
| **Expiration of Offer** | This term sheet expires **thirty (30) calendar days** from the Effective Date if not countersigned by Licensee, after which it may be withdrawn or revised by Licensor at its sole discretion. |
| **Licensor** | Animus Technologies Private Limited, a company incorporated under the Companies Act, 2013, having its registered office in India (CIN: [CIN — to be inserted upon issuance of the Certificate of Incorporation; see [`../LEGAL_INCORPORATION_BRIEF.md`](../LEGAL_INCORPORATION_BRIEF.md)]), operating under the "Animus Core" brand, represented by **Alakshendra Roy, Founder & Chief Architect** ("**Licensor**," "**Animus**") |
| **Licensee** | [CLIENT LEGAL ENTITY NAME], a [ENTITY TYPE] organized under the laws of [JURISDICTION] ("**Licensee**," "**Client**") |
| **Scope Classification** | Desk License — single production node, single strategy or single telemetry/surveillance pipeline, capped core count, as defined in §2.4 |

---

## 2. Commercial Structure & Payment Schedule

**2.1 Annual Commitment.** Licensee shall pay Licensor an annual license fee of **USD $[30,000–60,000, to be fixed within this range — see §2.1.1]** per year, for the Scope defined in §2.4.

**2.1.1 Fixing the exact fee.** The $30,000–$60,000 range above is not itself a final price — before an order form is signed, Licensor will issue a short written scoping questionnaire (core count, node count, strategy/pipeline count, integration surface) and return a single fixed annual figure within this range based on Licensee's answers. **The order form and the DLA state the fixed figure, never the range** — a signed instrument must never leave this as an open interval.

**2.2 Billing Options.**

| Option | Structure | Terms |
|---|---|---|
| **Option A** | 100% upfront, annual | Net-30 from invoice date |
| **Option B** | Quarterly advance | Fixed annual fee ÷ 4 per quarter, Net-15 from invoice date |

**2.3 Currency & Settlement.** All amounts are stated and payable in **USD**, by wire transfer via **SWIFT or Fedwire**. Licensee bears all wire, correspondent-bank, and currency-conversion fees; invoiced amounts are net of any such fees or withholding, subject to applicable tax treaty documentation (see [`W8BEN_GUIDE.md`](W8BEN_GUIDE.md)).

**2.4 Scope of Use — the boundary that distinguishes this tier from Institutional Enterprise.**

| Dimension | Desk License (this tier) |
|---|---|
| **Production nodes** | **One (1)** designated production node/host |
| **Pinned/dedicated CPU cores** | Up to **[N — to be fixed per quote, typically 4–8]** physical cores dedicated to the licensed engine on that node |
| **Strategies / pipelines** | **One (1)** designated trading strategy, or one designated telemetry/surveillance pipeline — not both, and not multiple of either, under a single Desk License |
| **Packet/event volume** | Unlimited within the above node/core/strategy scope — no per-tick metering |

Use beyond any one of these limits (a second node, a second strategy, or a core count above the fixed cap) is **out of scope** for this license and requires either a scope amendment (additional fee, negotiated) or an upgrade to the Institutional Enterprise tier per §8. Licensee is responsible for notifying Licensor before scaling usage beyond the fixed scope; using the Software beyond the licensed scope without a signed amendment is a breach of the DLA, not a self-service upgrade.

---

## 3. Technical Deliverables & Deliverable Artifacts

| Artifact | Description |
|---|---|
| **Hardened compiled binary** | `libanimus.so` (Linux x86_64) or `AnimusNative.dll` (Windows), built with `-O3 -march=native -fno-rtti -fno-exceptions`; zero heap allocations on the hot path |
| **Header API** | `include/animus/engine.hpp`, Pimpl-isolated to protect proprietary lock-free ring-buffer layouts from the public interface |
| **Zero-copy Python Bridge** | Nanobind-based bridge mapping SPSC ring-buffer entries directly into NumPy `ndarray` views, no intermediate copy |
| **Benchmark Verification Harness** | `replay_bench` / `harness_benchmark` — synthetic packet driver for measuring bare-metal p50/p90/p99/p99.9 latencies on Licensee's own hardware |

All artifacts are delivered under the license grant and restrictions of the definitive DLA (§6). **No source code is delivered under this tier** — full stop, and unlike [`ANIMUS_ENTERPRISE_TERMSHEET.md`](ANIMUS_ENTERPRISE_TERMSHEET.md) §5.4, no source-code *escrow* option exists at this tier either (see §5.4 below). A desk that needs escrow as a continuity safeguard should be scoped into Institutional Enterprise (§8) instead; a desk that needs actual standing source access needs a wholly separate, more expensive arrangement — the "Custom Source License" tier in [`../COMMERCIAL_OVERVIEW.md`](../COMMERCIAL_OVERVIEW.md) §4 — which is not part of either this term sheet or the Institutional Enterprise term sheet, and is not something either grants by default.

---

## 4. Evaluation-to-Production Conversion Protocol

**Stage 1 — 30-Day Self-Serve Evaluation.**
Licensee evaluates the engine using the free [`Pilot_Kit/`](../Pilot_Kit/PILOT_README.md) (Windows) or [`eval_kit/`](../eval_kit/README.md) (Linux) kit, following the async process in [`ASYNC_EVALUATION_PLAYBOOK.md`](ASYNC_EVALUATION_PLAYBOOK.md) §1. No production use or fee obligation arises from Stage 1 alone.

**Stage 2 — Checklist-Verified Benchmark Acceptance.**
Using [`PILOT_EVAL_CHECKLIST.md`](PILOT_EVAL_CHECKLIST.md)'s §3 acceptance thresholds on Licensee's own representative hardware, Licensee independently measures and records tail latency and throughput. Failure to meet the published thresholds on Licensee's hardware does not obligate conversion to Stage 3, and does not by itself entitle Licensee to a fee reduction below §2.1's fixed figure once set.

**Stage 3 — Scoping & Conversion to Desk License Agreement.**
Upon Licensee's written interest following Stage 2, Licensor issues the scoping questionnaire (§2.1.1) and a fixed order form. Upon Licensee's countersignature, the Parties execute a definitive Desk License Agreement incorporating the commercial terms in §2, the deliverables in §3, and the IP, support, and legal terms in §§5–7 below, refined as mutually agreed.

---

## 5. Intellectual Property & Use Restrictions

**5.1 IP Retention.** Licensor retains complete and exclusive ownership of all intellectual property, algorithms, source code, and trade secrets embodied in the Animus Core engine, including the lock-free ring-buffer design, cache-line-aware memory layout, C-ABI, and benchmarking methodology. No rights transfer to Licensee beyond the limited use rights granted under the DLA, within the scope fixed in §2.4.

**5.2 Clean Room Certification.** Licensor certifies that the Animus Core engine is 100% original work product, developed without reference to, copying from, or incorporation of any GPL, copyleft, or otherwise incompatible third-party licensed source.

**5.3 Use Restrictions.** Licensee shall not reverse-engineer, decompile, disassemble, or attempt to derive source code from the delivered binary artifacts, except to the extent such restriction is unenforceable under applicable law. Redistribution, sublicensing, or provision of the Software or its outputs to any third party, or use beyond the single node/single strategy/core-count scope in §2.4, is prohibited absent Licensor's prior written consent or a signed scope amendment.

**5.4 No Source Escrow at This Tier.** Unlike [`ANIMUS_ENTERPRISE_TERMSHEET.md`](ANIMUS_ENTERPRISE_TERMSHEET.md) §5.4, **no source code escrow arrangement is offered under the Desk License.** A desk requiring escrow as a continuity safeguard should be scoped into the Institutional Enterprise tier instead (§8) rather than requesting escrow as an add-on to this one.

---

## 6. Support & Maintenance

**6.1 Support Channel.** Support is provided via **email only** (priority developer email, staffed directly by Licensor's Founder & Chief Architect) — **no dedicated Slack Connect channel is included at this tier** (contrast [`ANIMUS_ENTERPRISE_TERMSHEET.md`](ANIMUS_ENTERPRISE_TERMSHEET.md) §6.3). A dedicated channel can be added as a paid amendment if Licensee requests one.

**6.2 Response Target.** Licensor will use **commercially reasonable efforts** to provide an initial written response to a support request within **one (1) business day**. This is a best-efforts target, not a guaranteed incident-response SLA with service credits — reflecting that Licensor is a single-operator technical team. Contrast the Institutional Enterprise tier's four-hour critical-incident target (§6.1 of that term sheet), which is not offered at this tier; a desk that needs a tighter guaranteed response window should be scoped into that tier instead of requesting it as an add-on here.

**6.3 Maintenance.** Licensor will deliver quarterly optimization rollouts, including AVX-512 vectorization updates and kernel/compiler compatibility updates, at no additional charge during the license term.

---

## 7. Execution, Governing Law & Signatures

**7.1 Governing Law; Arbitration.** This term sheet and any definitive DLA entered pursuant to it are governed by the laws of **India**, without regard to conflict-of-laws principles, consistent with [`../LEGAL_EULA.md`](../LEGAL_EULA.md) §9.2 and [`PILOT_CONTRACT.md`](PILOT_CONTRACT.md) §8.1. Any dispute arising out of or relating to this term sheet or the DLA shall be referred to and finally resolved by arbitration administered by the **Singapore International Arbitration Centre ("SIAC")** under the SIAC Rules then in force, seated in **Singapore**, before **one (1) arbitrator**, in the **English** language. Either Party may seek interim or injunctive relief from a court of competent jurisdiction pending constitution of the arbitral tribunal.

**7.2 Non-Binding Effect.** Except for this §7 and any confidentiality obligations under a separate NDA referenced in §4 Stage 1, this term sheet is an indicative summary of proposed terms and does not itself create a binding license, payment obligation, or exclusivity commitment. Binding obligations arise only upon execution of the definitive DLA.

**7.3 Assignment.** Neither Party may assign this term sheet or the rights described herein without the other's prior written consent, except in connection with a merger, acquisition, or sale of substantially all assets.

---

## 8. Upgrade Path to Institutional Enterprise

If Licensee's usage outgrows the scope fixed in §2.4 — a second production node, a second strategy/pipeline, a core count above the fixed cap, or a need for source escrow (§5.4) or a dedicated support channel (§6.1) — Licensee may upgrade to the Institutional Enterprise tier under [`ANIMUS_ENTERPRISE_TERMSHEET.md`](ANIMUS_ENTERPRISE_TERMSHEET.md), at Licensor's then-current terms for that tier. At Licensor's discretion, the unused, pro-rated portion of Licensee's current Desk License term may be credited toward the first year of the Institutional Enterprise fee — this is a discretionary accommodation to be confirmed in writing at the time of upgrade, not a binding entitlement created by this term sheet.

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
> Do not send this term sheet to a prospective client or treat any term above as final until a licensed attorney has reviewed it — in particular: §2.1's fee range (make sure a signed order form or DLA always states the single fixed figure, never the $30k–$60k range itself), §2.4's scope caps (the specific core-count number is a placeholder — fix it per quote before sending), §7.1's SIAC arbitration clause (jurisdiction-sensitive against a non-Indian counterparty, same open question flagged in `ANIMUS_ENTERPRISE_TERMSHEET.md`), §8's upgrade-credit language (discretionary as written — a counsel review may want this tightened or loosened deliberately, not left ambiguous), and §1's CIN placeholder pending Certificate of Incorporation issuance.
