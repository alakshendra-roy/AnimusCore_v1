# Animus Core — 30-Day Pilot Evaluation & Commercial License Agreement

**Companion documents:** [`COMMERCIAL.md`](COMMERCIAL.md) (enterprise tiering and SLA guarantees) · [`COMPLIANCE_AND_RISK_MITIGATION.md`](COMPLIANCE_AND_RISK_MITIGATION.md) (operational protocol for the SLA/topology, cross-border tax, and IP-isolation terms referenced in Sections 7 and 9.4 below).

---

**This Pilot Evaluation & Commercial License Agreement** ("**Agreement**") is entered into as of **[EFFECTIVE DATE]** ("**Effective Date**") by and between:

- **Animus Technologies Private Limited**, a company incorporated under the Companies Act, 2013, having its registered office in India (CIN: **[CIN — to be inserted upon issuance of the Certificate of Incorporation; see `LEGAL_INCORPORATION_BRIEF.md`]**), operating under the "Animus Core" brand and formerly conducting this business as a sole proprietorship under its Founder, Alakshendra Roy ("**Animus Core**," "**Vendor**," "**we**," "**us**"); and
- **[CUSTOMER LEGAL ENTITY NAME]**, a [ENTITY TYPE] organized under the laws of [JURISDICTION] ("**Customer**," "**you**"),

each individually a "**Party**" and together the "**Parties**."

---

## 1. Definitions

| Term | Meaning |
|---|---|
| "**Software**" | The Animus Core telemetry engine, including compiled binaries (`AnimusNative.dll` / `libanimus_native.so` / `libanimus_native.dylib` / `AnimusCore_v1.dll`), the accompanying Python SDK, sample/integration code, and all associated documentation. |
| "**Licensed Hardware**" | The single physical machine identified by the Hardware Fingerprint submitted by Customer under Section 2.2, against which a License File is cryptographically bound. |
| "**Hardware Fingerprint**" | The SHA-256 digest of that machine's `MachineGuid` and primary network adapter MAC address, as produced by the Vendor-supplied fingerprinting utility. |
| "**License File**" | The RSA-2048-signed `.lic` file issued by Vendor, binding a specific license grant (evaluation or production) to one Hardware Fingerprint, verified entirely offline by the Software with no network dependency. |
| "**Evaluation Period**" | The thirty (30) calendar days beginning on the date Vendor issues Customer's evaluation License File, as further described in Section 2. |
| "**Documentation**" | All user guides, quickstart materials, benchmark reports, and API references Vendor makes available with the Software. |

---

## 2. Evaluation License Grant

### 2.1 Grant

Subject to Customer's compliance with this Agreement, Vendor grants Customer a **non-exclusive, non-transferable, non-sublicensable, revocable** license to install and use the Software, solely:

(a) during the Evaluation Period;
(b) on the single Licensed Hardware machine identified in Customer's evaluation License File;
(c) for the sole purpose of internal evaluation and testing of the Software's suitability for Customer's business, and not for production use, resale, or use in delivering any service to a third party.

### 2.2 Hardware Fingerprint Binding

Customer shall generate a Hardware Fingerprint using Vendor's supplied read-only utility (`get_fingerprint.ps1` or equivalent) and transmit it to Vendor. Vendor shall issue a License File bound to that Hardware Fingerprint alone. **A License File issued under this Agreement will not verify successfully, and the gated features it controls will not function, on any machine other than the Licensed Hardware.** Customer shall not attempt to spoof, falsify, or otherwise misrepresent a Hardware Fingerprint to obtain a License File for hardware other than the machine actually fingerprinted.

### 2.3 Core Functionality Not Gated

For the avoidance of doubt, core telemetry event ingestion is not gated by the License File; only certain opt-in tuning features (e.g., CPU core-affinity pinning) require a valid, unexpired License File bound to the Licensed Hardware, and such features fail closed (disabled) rather than defaulting to any unlicensed behavior.

### 2.4 Expiration

Upon expiration of the Evaluation Period, the evaluation License File shall cease to verify as valid, and Customer's rights under Section 2.1 terminate automatically without notice. Continued use of the Software after expiration requires an executed production order form under Section 3, or a written extension executed by both Parties.

---

## 3. Conversion to Commercial License

Following the Evaluation Period, Customer may elect to enter a commercial engagement under one of the four production tiers described in Vendor's then-current commercial materials (see `COMMERCIAL.md` §2):

- **Desk License** — single production node, single strategy or pipeline, capped core count (`docs/DESK_LICENSE_TERMSHEET.md`);
- **Institutional Enterprise (Desk ELA)** — single designated trading desk, multi-node (`docs/ANIMUS_ENTERPRISE_TERMSHEET.md`; order form `docs/CONTRACT_ENTERPRISE_DESK_240K.md`);
- **OEM / Redistribution** — redistribution into a third party's product, under a separate OEM Distribution Agreement;
- **Global Strategic Master Agreement (Global Enterprise ELA)** — uncapped global deployment across Customer and its Affiliates, subject to Section 3.1 (order form `docs/CONTRACT_GLOBAL_STRATEGIC_480K.md`).

Any such commercial license shall be governed by a separate written order form or license schedule executed by both Parties, which shall incorporate the general terms of this Agreement (Sections 3.2, 3.3 and 4–9) except as expressly modified by that order form. Where an order form grants production rights, Sections 4(d) and 4(e) apply by reference to the licensed scope stated in that order form rather than to the single Licensed Hardware and Evaluation Period of Section 2. **No commercial license, fee obligation, or production right is created by this Agreement standing alone** — this Agreement, absent an executed order form, governs the Evaluation Period only.

### 3.1 Global Strategic Master Agreement — Enterprise Master License Grant

Where Customer's order form designates a **Global Strategic Master Agreement**, the following terms apply in addition to, and where inconsistent supersede, the general Desk License / Institutional Enterprise / OEM-Redistribution production terms referenced above (see `COMMERCIAL.md` §2 for the full reconciled tier ladder and its entitlements):

(a) **Affiliate scope.** The uncapped, global deployment right granted under a Global Strategic Master Agreement order form extends only to Customer and its **Affiliates** — meaning any entity that, directly or indirectly, owns or controls more than fifty percent (50%) of the voting securities or equivalent voting interest of Customer, or of which Customer owns or controls more than fifty percent (50%) of such voting interest, for so long as that ownership or control relationship continues. An entity ceases to be an Affiliate, and its license rights under the Global Strategic Master Agreement grant terminate automatically, upon the relationship falling below that threshold (e.g., divestiture, spin-off, change of control). Customer shall maintain and, upon Vendor's written request no more than once per calendar year, provide a current list of entities relying on the Global Strategic Master Agreement grant as Affiliates.

(b) **Deployment scope.** Subject to (a), the Global Strategic Master Agreement grant covers deployment across all of Customer's and its Affiliates' data centers, co-location racks, and edge platforms globally, without the per-node or per-core ceilings applicable to Desk License, Institutional Enterprise, or OEM / Redistribution, and without requiring per-node or per-core license tracking across distributed clusters — entitlement is tracked at the master-agreement level, not the individual node level, though each deployed node still issues and verifies its own RSA-2048-signed, hardware-fingerprint-bound `.lic` file per Section 2.2's mechanism.

(c) **Escrow / read-only audit access.** Vendor shall make available to Customer's internal compliance, risk, or security audit function **read-only** access to the Software's source code, either via a mutually agreed third-party source-code escrow arrangement or a controlled read-only inspection environment specified in the order form, solely for the purpose of internal security, memory-safety, and concurrency verification consistent with `COMMERCIAL.md` §3.2. Such access:

&nbsp;&nbsp;(i) does not include any right to copy, extract, compile, modify, or create derivative works from the inspected source, except as strictly necessary to generate an internal audit finding for Customer's own compliance records;
&nbsp;&nbsp;(ii) is subject to the non-derivation and non-disclosure covenants in (d) below in addition to the general confidentiality obligation in Section 9.1; and
&nbsp;&nbsp;(iii) may be conditioned by Vendor on a facility, personnel, or export-control screening consistent with Section 9.4, where the order form so specifies.

(d) **Anti-fork, non-derivation, non-compete, and non-disclosure covenants.** In consideration of the escrow/audit access granted under (c), Customer covenants that it shall not, and shall not permit any Affiliate, contractor, or third party to: (i) fork, derive, adapt, or produce any competing or substantially similar work from the inspected source code ("non-derivation"); (ii) use knowledge gained from source inspection to build, procure, or commission a product or service competing with the Software, beyond the general restriction already stated in Section 4(f); or (iii) disclose the inspected source, in whole or in part, to any person other than those of Customer's employees or Affiliates' employees with a demonstrated need to know for the audit purpose stated in (c), each of whom shall be bound by confidentiality obligations no less protective than this Agreement's. These covenants, together with Vendor's underlying copyright in the Software, are without prejudice to and do not limit Vendor's rights and remedies under applicable law, including the assignment and licensing provisions of Sections 18 and 19 of the (Indian) Copyright Act, 1957, governing the scope of any rights in the Software that Vendor has or has not assigned or licensed to Customer — for the avoidance of doubt, no assignment of copyright in the Software occurs under a Global Strategic Master Agreement order form, only the escrow/inspection license described in (c).

(e) **Export control.** All source and binary materials made available under this Section 3.1 remain subject to Section 9.4; where Customer or an Affiliate is located in, or the materials would transit through, a jurisdiction subject to applicable export control or sanctions restrictions, Vendor may condition or decline escrow/audit access accordingly.

(f) **Precedence.** Nothing in this Section 3.1 expands Customer's rights beyond a license — it does not transfer ownership of, or any copyright interest in, the Software, which remains governed by Section 5.

### 3.2 Proprietary Technology, Trade Secrets & Clean-Room Protection

This Section 3.2 applies under every order form and, to the extent Customer receives any Confidential Information of Vendor, during the Evaluation Period.

(a) **Proprietary Technology.** "**Proprietary Technology**" means the Software and all of Vendor's designs, implementations, and know-how embodied in or relating to it, including without limitation: (i) the lock-free single-producer/single-consumer and multi-producer ring-buffer data structures, their memory-ordering protocols, and their index and sequence-number schemes; (ii) the invariant-TSC timestamping routines and TSC calibration and cross-core drift-compensation methods; (iii) the cache-line-aligned (`alignas(64)`) memory layouts, padding schemes, false-sharing elimination techniques, and shared-memory segment formats; (iv) the zero-copy C-ABI and Python bridge interfaces; (v) the license-verification and hardware-fingerprinting mechanism; and (vi) Vendor's benchmarking methodology, tuning profiles, and unpublished performance data.

(b) **Trade Secrets.** Those elements of the Proprietary Technology that Vendor has not published in its public repository — including production build configurations and compiler/tuning profiles, non-public source branches, source made available under Section 3.1(c), workload-specific latency characterizations, and unreleased features and roadmap — are Vendor's trade secrets and Confidential Information, protected for as long as they qualify as trade secrets under applicable law and in any event for not less than five (5) years after the end of the last order form. The publication of any portion of the Software's source code under Vendor's public dual-license (`LICENSE`) grants Customer no right beyond that license and this Agreement, and does not diminish Vendor's copyright in, or the contractual restrictions of this Agreement on, that portion.

(c) **No reverse engineering or extraction.** In addition to Section 4(a), Customer shall not (i) instrument, trace, memory-dump, or side-channel-probe the Software for the purpose of reconstructing its data-structure layouts, memory-ordering protocols, or timing routines; (ii) disassemble or diff successive binary releases to derive unpublished changes; or (iii) remove, patch, bypass, or emulate the license-verification mechanism.

(d) **Clean-room covenant.** Customer shall not use the Proprietary Technology or any Confidential Information of Vendor — including information retained in the unaided memory of personnel who accessed Vendor's non-public materials — to design, develop, specify, or commission a shared-memory ingestion engine, lock-free queue, or timestamping subsystem that substitutes for the Software. Any such development undertaken by or for Customer during the term of any order form and for eighteen (18) months after it ends shall be performed only by personnel who have not accessed Vendor's non-public materials, and Customer shall, on Vendor's reasonable written request, certify that separation in writing. This covenant restricts the use of Vendor's information; it does not restrict any individual's employment or use of general skills and publicly available knowledge.

(e) **Equitable relief.** A breach of this Section 3.2 or of Section 4 would cause Vendor irreparable harm for which damages would be an inadequate remedy. Vendor may seek injunctive relief under Section 9.2, without posting a bond to the extent permitted by law, in addition to all other remedies.

### 3.3 Committed Term, No Termination for Convenience & Fee Acceleration

This Section 3.3 applies to every production order form unless that order form expressly states otherwise.

(a) **Firm commitment.** The Initial Term stated in an order form (twelve (12) months unless otherwise stated) is a **firm, non-cancellable commitment**. The total fees for the entire Initial Term (the "**Committed Fees**") are fully committed and become an unconditional payment obligation of Customer upon execution of the order form. Any quarterly installment schedule is an accommodation as to the **timing of payment only** and does not reduce, condition, or divide Customer's obligation to pay the Committed Fees in full.

(b) **No termination for convenience.** Customer has **no right to terminate** an order form or this Agreement for convenience, and no right to reduce the licensed scope, during the Initial Term or any Renewal Term. Customer's non-use or reduced use of the Software; discontinuation or cancellation of the project, desk, strategy, or business line for which the Software was licensed; change in Customer's budget or strategy; or change of control of Customer does not suspend, reduce, or excuse payment of the Committed Fees.

(c) **Fees non-refundable.** All fees paid are non-refundable and non-creditable, except solely as provided in Section 3.3(f).

(d) **Acceleration Events.** Each of the following is an "**Acceleration Event**": (i) Customer gives notice of, or purports to effect, termination or cancellation of an order form other than under Section 3.3(f); (ii) Customer gives written notice that it will cease or has ceased using the Software, or that the project, desk, or deployment for which the Software was licensed has been discontinued; (iii) Customer fails to pay any undisputed installment within ten (10) business days after written notice of non-payment; (iv) Vendor terminates an order form under Section 8.2 for Customer's uncured material breach; or (v) Customer becomes subject to any insolvency, winding-up, administration, or analogous proceeding.

(e) **Acceleration.** Upon an Acceleration Event, **all unpaid installments of the Committed Fees for the remainder of the then-current term immediately accelerate and become due and payable in full within ten (10) business days** after Vendor's written demand, without further notice. No annual-prepayment discount applies to accelerated amounts. The Parties acknowledge that accelerated amounts are the agreed price of a committed term, payable on an accelerated timetable, and are not a penalty or an estimate of damages. Where the Acceleration Event arises under Section 3.3(d)(i) or (ii) and Customer is not otherwise in breach, Customer's license continues for the remainder of the paid term once the accelerated amount is paid.

(f) **Sole exception — Vendor's uncured material breach.** If Customer terminates an order form under Section 8.2 because of Vendor's material breach that Vendor has failed to cure within thirty (30) days after Customer's detailed written notice (this thirty-day period replacing, for a Vendor breach under a production order form, the ten-day period in Section 8.2), no Acceleration Event occurs, Customer is released from installments falling due after the termination date, and Vendor shall refund any prepaid fees attributable to the period after the termination date. This is Customer's sole remedy for recovery of fees on such termination.

(g) **Late payment; suspension; costs.** Undisputed amounts not paid when due bear interest at the lesser of one and one-half percent (1.5%) per month and the maximum rate permitted by law. On ten (10) business days' written notice, Vendor may suspend issuance of License Files, updates, and support until overdue amounts are paid; suspension does not suspend Customer's payment obligations. Customer shall reimburse Vendor's reasonable costs of collecting amounts not paid when due, including arbitration and legal fees. Customer shall pay all amounts without set-off, counterclaim, or deduction, other than tax withholding required by law.

(h) **Renewal.** Unless an order form states otherwise, it renews for successive twelve (12)-month Renewal Terms unless either Party gives written notice of non-renewal at least ninety (90) days before the end of the then-current term. Each Renewal Term is a committed term subject to this Section 3.3. Vendor may change the fees for a Renewal Term by written notice given at least one hundred twenty (120) days before that Renewal Term begins.

---

## 4. Restrictions

Customer shall not, and shall not permit any third party to:

(a) **Reverse engineer, decompile, disassemble, or otherwise attempt to derive the source code, algorithms, or underlying structure of** the compiled Software binaries, except to the limited extent such restriction is expressly prohibited by applicable law notwithstanding this limitation;

(b) **Redistribute, sublicense, sell, rent, lease, lend, or otherwise make the Software (in whole or in part, including any compiled binary, License File, or Documentation) available to any third party**, whether or not for consideration;

(c) Remove, obscure, or alter any proprietary notice, trademark, or licensing/attribution marking contained in or on the Software;

(d) Use the Software on any hardware other than the Licensed Hardware, or attempt to circumvent, disable, or tamper with the Hardware Fingerprint verification or License File signature-verification mechanism;

(e) Use the Software beyond the scope of the license expressly granted in Section 2 (including any production, revenue-generating, or third-party-facing use during the Evaluation Period);

(f) Use the Software to build a product or service that competes with the Software;

(g) Publish or disclose to any third party the results of any benchmark or performance test of the Software without Vendor's prior written consent, except to Customer's own auditors and regulators under a duty of confidentiality;

(h) Breach the protections of the Proprietary Technology set out in Section 3.2.

---

## 5. Intellectual Property

### 5.1 Ownership

**Animus Core (or its licensors) retains all right, title, and interest in and to the Software, including all Intellectual Property Rights therein.** No rights are granted to Customer other than the limited license expressly set forth in Section 2. This Agreement does not constitute a sale of the Software or any copy thereof.

### 5.2 Definition

"Intellectual Property Rights" means all patent, copyright, trade secret, trademark, and other proprietary rights, worldwide, whether or not registered.

### 5.3 No Implied Rights; Feedback

All rights not expressly granted to Customer are reserved by Vendor. Any feedback, suggestions, or improvement ideas Customer voluntarily provides regarding the Software may be used by Vendor without restriction or obligation to Customer.

### 5.4 Customer Data

As between the Parties, Customer retains all rights in the telemetry data, event streams, and other content Customer processes through the Software. Vendor claims no ownership interest in Customer's data.

---

## 6. Warranty Disclaimer

> **THE SOFTWARE IS PROVIDED "AS IS" AND "AS AVAILABLE," WITHOUT WARRANTY OF ANY KIND.**

TO THE MAXIMUM EXTENT PERMITTED BY APPLICABLE LAW, VENDOR EXPRESSLY DISCLAIMS ALL WARRANTIES, WHETHER EXPRESS, IMPLIED, STATUTORY, OR OTHERWISE, INCLUDING WITHOUT LIMITATION ANY IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, TITLE, AND NON-INFRINGEMENT, AND ANY WARRANTIES ARISING OUT OF COURSE OF DEALING OR USAGE OF TRADE. VENDOR DOES NOT WARRANT THAT THE SOFTWARE WILL BE UNINTERRUPTED, ERROR-FREE, OR FREE OF HARMFUL COMPONENTS, OR THAT ANY PARTICULAR LATENCY, THROUGHPUT, OR OTHER PERFORMANCE FIGURE PUBLISHED IN VENDOR'S BENCHMARK MATERIALS WILL BE ACHIEVED ON CUSTOMER'S HARDWARE OR WORKLOAD. THIS SECTION 6 IS ESPECIALLY APPLICABLE DURING THE EVALUATION PERIOD, WHICH IS PROVIDED SOLELY FOR CUSTOMER'S OWN TESTING AT NO WARRANTY AND, UNLESS OTHERWISE STATED IN AN EXECUTED ORDER FORM, AT NO FEE.

---

## 7. Limitation of Liability

TO THE MAXIMUM EXTENT PERMITTED BY APPLICABLE LAW:

(a) **IN NO EVENT SHALL EITHER PARTY BE LIABLE FOR ANY INDIRECT, INCIDENTAL, SPECIAL, CONSEQUENTIAL, OR PUNITIVE DAMAGES**, including without limitation lost profits, lost revenue, lost data, or business interruption, arising out of or relating to this Agreement or the Software, however caused and under any theory of liability (contract, tort, negligence, or otherwise), even if advised of the possibility of such damages;

(b) **VENDOR'S TOTAL AGGREGATE LIABILITY** arising out of or relating to this Agreement or the Software shall not exceed the total fees actually paid by Customer to Vendor under this Agreement in the twelve (12) months preceding the event giving rise to the claim, or **[US]$[CAP AMOUNT]** if no fees have been paid (including during the Evaluation Period);

(c) The limitations in this Section 7 apply regardless of the failure of essential purpose of any limited remedy and shall not apply to (i) either Party's indemnification obligations, if any are added by a subsequent order form, (ii) Customer's breach of Sections 3.2, 4 or 5, (iii) Customer's obligation to pay fees, including Committed Fees accelerated under Section 3.3, or (iv) either Party's gross negligence or willful misconduct, in each case only to the extent such exclusion is required by applicable law.

(d) The Reference Topology Appendix and staging/canary UAT protocol that scope Vendor's performance commitments for production order forms are documented in `COMPLIANCE_AND_RISK_MITIGATION.md` §1.

---

## 8. Term & Termination

**8.1 Term.** This Agreement commences on the Effective Date and continues until the earlier of (a) expiration of the Evaluation Period without conversion under Section 3, or (b) termination under this Section 8.

**8.2 Termination for Cause.** Either Party may terminate this Agreement immediately upon written notice if the other Party materially breaches this Agreement and fails to cure such breach within ten (10) days of notice (or immediately, without a cure period, for breaches of Section 4).

**8.3 Effect of Termination.** Upon termination or expiration, Customer shall immediately cease all use of the Software, uninstall all copies, and destroy or return any License File and Confidential Information in its possession, and shall certify the same in writing within ten (10) business days on Vendor's request. Termination does not relieve Customer of any payment obligation accrued or accelerated under Section 3.3. Sections 1, 3.2, 3.3 (as to accrued and accelerated amounts), 4, 5, 6, 7, 8.3, 8.4, and 9 survive termination.

**8.4 Production Order Forms.** While an order form is in effect, this Agreement continues for as long as that order form remains in effect, and the only termination rights available to either Party during its Initial Term or any Renewal Term are those in Section 8.2, subject to Section 3.3.

---

## 9. General Provisions

**9.1 Confidentiality.** Each Party shall protect the other's non-public technical and business information disclosed under this Agreement using at least the same degree of care it uses for its own similarly sensitive information, and shall not disclose such information to third parties except as necessary to exercise its rights or perform its obligations hereunder.

**9.2 Governing Law; Arbitration.** This Agreement shall be governed by the laws of **India**, without regard to its conflict-of-laws principles. Any dispute, controversy, or claim arising out of or relating to this Agreement, including any question regarding its existence, validity, or termination, shall be referred to and finally resolved by arbitration administered by the **Singapore International Arbitration Centre ("SIAC")** in accordance with the Arbitration Rules of the Singapore International Arbitration Centre ("**SIAC Rules**") for the time being in force, which rules are deemed to be incorporated by reference into this Section 9.2. The seat of the arbitration shall be **Singapore**. The tribunal shall consist of **one (1) arbitrator**, unless the Parties otherwise agree or the SIAC Rules require otherwise given the amount in dispute. The language of the arbitration shall be **English**. Nothing in this Section 9.2 prevents either Party from seeking interim or injunctive relief from a court of competent jurisdiction pending constitution of the arbitral tribunal — matching the arbitration clause in `docs/PILOT_CONTRACT.md` §8.1 and `docs/PILOT_AGREEMENT.md` §9.1, so the dispute-resolution mechanism is the same across the free evaluation, paid pilot, and production license stages of the relationship.

**9.3 Assignment.** Customer shall not assign or transfer this Agreement, in whole or in part, without Vendor's prior written consent. Vendor may assign this Agreement in connection with a merger, acquisition, or sale of substantially all its assets.

**9.4 Export Compliance.** Customer shall comply with all applicable export control and economic sanctions laws in its use of the Software. Vendor's cross-border invoicing, DTAA characterization, and RBI/FEMA export-compliance pipeline are documented in `COMPLIANCE_AND_RISK_MITIGATION.md` §2.

**9.5 Entire Agreement.** This Agreement (together with any executed order form under Section 3) constitutes the entire agreement between the Parties regarding its subject matter and supersedes all prior or contemporaneous agreements, whether written or oral, on that subject.

**9.6 Severability.** If any provision of this Agreement is held unenforceable, the remaining provisions shall remain in full force and effect, and the unenforceable provision shall be reformed to the minimum extent necessary to make it enforceable.

**9.7 Notices.** All notices under this Agreement shall be in writing and delivered to the addresses specified in the signature block below (or such other address as a Party designates in writing).

**9.8 No Waiver.** No failure or delay by either Party in exercising any right under this Agreement shall operate as a waiver of that right.

---

## Signature Block

| | Vendor | Customer |
|---|---|---|
| **Entity Name** | Animus Technologies Private Limited (Animus Core) | [CUSTOMER LEGAL ENTITY NAME] |
| **Signature** | ___________________________ | ___________________________ |
| **Name** | Alakshendra Roy | [PRINTED NAME] |
| **Title** | Founder & Director | [TITLE] |
| **Date** | [DATE] | [DATE] |
| **Notice Address** | inquiries@animusinfra.com (email notice; see also GitHub Issues at https://github.com/alakshendra-roy/AnimusCore_v1/issues for technical notices — registered office address to be added upon issuance of the Certificate of Incorporation, per `LEGAL_INCORPORATION_BRIEF.md`) | [CUSTOMER ADDRESS] |

---

> ## ⚠️ REMINDER
> Do not distribute this document to any prospective customer, and do not treat it as binding, until it has been reviewed and approved by a qualified attorney licensed in the governing jurisdiction selected in Section 9.2. In particular, the liability cap (Section 7), warranty disclaimer (Section 6), export/sanctions language (Section 9.4), and the SIAC arbitration clause (Section 9.2 — a founder-selected draft, not counsel-vetted; confirm forum-fit and Singapore-seated-award enforceability against actual customer jurisdictions) are jurisdiction-dependent and may require material revision to be enforceable where Vendor and Customer are located. Counsel should also specifically confirm: (i) that the Section 3.3 fee acceleration will be treated under Indian law as a debt for a committed term rather than a penalty under Section 74 of the Indian Contract Act, 1872, particularly where Vendor has terminated the license (Section 3.3(d)(iv)); (ii) that the Section 3.2(d) clean-room covenant is framed as a restriction on use of information and does not operate as a restraint of trade under Section 27 of that Act; and (iii) the scope of trade-secret protection that remains available given that portions of the Software's source are published in Vendor's public repository (Section 3.2(b)).
