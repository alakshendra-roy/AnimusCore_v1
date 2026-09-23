# Animus Core — Institutional Enterprise Desk Order Form (Desk ELA)

> ## ⚠️ DRAFT ORDER FORM — NOT REVIEWED BY COUNSEL — DO NOT SIGN AS-IS
> This order form is structurally complete and priced for issuance, but it has not been reviewed by a licensed attorney. It must not be executed, and no invoice may be raised against it, until counsel has reviewed it together with the governing master agreement ([`../LEGAL_EULA.md`](../LEGAL_EULA.md)) and the Licensor's Certificate of Incorporation has been issued (see the CIN placeholder below and [`../LEGAL_INCORPORATION_BRIEF.md`](../LEGAL_INCORPORATION_BRIEF.md)).

---

## 1. Order Form Identification

| Field | Detail |
|---|---|
| **Order Form ID** | ANM-OF-2026-ENT-DESK-[NNN] |
| **Commercial Tier** | Institutional Enterprise — Desk Enterprise License Agreement ("**Desk ELA**") |
| **Order Effective Date** | [ORDER EFFECTIVE DATE] |
| **Offer Expiry** | This order form lapses if not countersigned by Licensee within thirty (30) calendar days of the date Licensor issues it. |
| **Licensor** | Animus Technologies Private Limited, a company incorporated under the Companies Act, 2013, having its registered office in India (CIN: [CIN — to be inserted upon issuance of the Certificate of Incorporation; see [`../LEGAL_INCORPORATION_BRIEF.md`](../LEGAL_INCORPORATION_BRIEF.md)]), operating under the "Animus Core" brand ("**Licensor**") |
| **Licensee** | [CLIENT LEGAL ENTITY NAME], a [ENTITY TYPE] organized under the laws of [JURISDICTION] ("**Licensee**") |
| **Master Agreement** | Animus Core Pilot Evaluation & Commercial License Agreement between Licensor and Licensee dated [MASTER AGREEMENT DATE] ([`../LEGAL_EULA.md`](../LEGAL_EULA.md)) (the "**Master Agreement**") |
| **Indicative Term Sheet** | [`ANIMUS_ENTERPRISE_TERMSHEET.md`](ANIMUS_ENTERPRISE_TERMSHEET.md) (non-binding; superseded by this order form on execution) |

---

## 2. Incorporation & Order of Precedence

**2.1** This order form is issued under, and incorporates, Section 3 of the Master Agreement, including Sections 3.2 (Proprietary Technology, Trade Secrets & Clean-Room Protection), 3.3 (Committed Term, No Termination for Convenience & Fee Acceleration), and 4–9. Capitalized terms not defined here have the meanings given in the Master Agreement.

**2.2** If this order form conflicts with the Master Agreement, this order form governs for the licensed scope below, except that nothing in this order form weakens Sections 3.2, 3.3, 4, or 5 of the Master Agreement unless it expressly says so by section number.

---

## 3. License Grant & Licensed Scope

**3.1 Grant.** Subject to payment of the Fees and compliance with the Master Agreement, Licensor grants Licensee a **non-exclusive, non-transferable, non-sublicensable** license during the Term to install and use the Software in object-code form, for Licensee's internal production trading and market-data operations, solely within the Licensed Scope.

**3.2 Licensed Scope.**

| Dimension | Entitlement |
|---|---|
| **Designated Asset Class** | One (1) asset class, identified in Schedule A |
| **Designated Desk** | One (1) dedicated trading desk or trading unit within the Designated Asset Class, identified in Schedule A |
| **Production nodes** | Unlimited, provided every node is operated by or for the Designated Desk |
| **CPU cores / threads / packet volume** | Unlimited within the Designated Desk — no per-core, per-thread, per-tick, or per-node metering |
| **Locations** | Any data center or co-location site used by the Designated Desk, listed in Schedule A and updated by written notice |
| **Redistribution** | None. Internal production use only (Master Agreement §4(b)) |
| **Source code access** | None (see §7) |

**3.3 Out-of-scope use.** Use of the Software by or for any other desk, asset class, business unit, or Affiliate — including a second desk within the Designated Asset Class — requires a signed scope amendment or a separate order form (for firm-wide or multi-region deployment, the Global Strategic Master Agreement order form, [`CONTRACT_GLOBAL_STRATEGIC_480K.md`](CONTRACT_GLOBAL_STRATEGIC_480K.md)). Out-of-scope use is a material breach of the Master Agreement.

**3.4 License Files.** Each production node receives its own RSA-2048-signed, hardware-fingerprint-bound License File under the mechanism in Master Agreement §2.2, encoding this order form's term. Licensee shall keep a current register of licensed node fingerprints for the Designated Desk and provide it to Licensor on request, no more than once per calendar quarter.

---

## 4. Fees & Payment

**4.1 Fees.**

| Item | Amount (USD) |
|---|---|
| Monthly equivalent | **$20,000** |
| Annual Contract Value (ACV) — Committed Fees for the Initial Term | **$240,000** |

**4.2 Billing.** Unless Licensee elects Option A at signature, Licensee is billed under the default Monthly Billing Schedule (Schedule B):

| | Option | Payment | Terms |
|---|---|---|---|
| ☐ | **Standard — Monthly in advance** | **$20,000** per contract month × 12 = $240,000 | See Schedule B |
| ☐ | **Option A — Annual prepayment** | **$228,000** (the $240,000 ACV less a 5% prepayment discount of $12,000), in one payment | Invoiced on the Order Effective Date; due Net-30 |

The 5% discount under Option A is conditional on payment in full by its due date; if Option A is not paid when due, the Monthly Billing Schedule applies to the Initial Term and Licensor may re-invoice accordingly.

**4.3 Delivery on payment.** Licensor will issue production License Files within two (2) business days after receipt of the first payment (Option A payment, or the first monthly installment). The Term, and Licensee's obligation to pay the Committed Fees, begin on the Order Effective Date regardless of when Licensee requests or installs License Files.

**4.4 Currency, settlement & taxes.** All amounts are in **USD**, payable by wire transfer via **SWIFT or Fedwire** to the account stated on Licensor's invoice. Fees exclude all taxes. Licensee bears all wire, correspondent-bank, and currency-conversion charges. If Licensee is required by law to withhold tax, it shall withhold only at the applicable treaty rate on receipt of Licensor's treaty documentation ([`W8BEN_GUIDE.md`](W8BEN_GUIDE.md)), furnish official receipts, and increase the payment so that Licensor receives the full invoiced amount.

**4.5 No contingency.** Licensee confirms that it has completed its evaluation of the Software. The Fees are not contingent on any acceptance test, benchmark result, latency threshold, future functionality, or Licensee's deployment timetable.

---

## 5. Term, Non-Cancellation & Fee Acceleration

This §5 restates, and is in addition to, Master Agreement §3.3.

**5.1 Initial Term.** Twelve (12) months from the Order Effective Date (the "**Initial Term**"). The Initial Term is **firm, non-cancellable, and non-refundable**. The full $240,000 Committed Fees (or $228,000 under Option A) is an unconditional payment obligation of Licensee from the moment this order form is executed; the Monthly Billing Schedule governs only the timing of payment.

**5.2 No termination for convenience.** Licensee may not terminate this order form, or reduce the Licensed Scope, for convenience at any time. Non-use or reduced use of the Software, discontinuation of the Designated Desk or its project or strategy, a change in budget or strategy, or a change of control of Licensee does not reduce, suspend, or excuse any payment.

**5.3 Fee acceleration.** If Licensee (a) gives notice of, or purports to effect, early termination or cancellation; (b) gives written notice that it will cease or has ceased using the Software, or that the Designated Desk's project has been discontinued; (c) fails to pay an undisputed installment within ten (10) business days after written notice of non-payment; (d) has this order form terminated by Licensor for Licensee's uncured material breach; or (e) becomes subject to insolvency or analogous proceedings — then **every unpaid monthly installment for the entire remaining Initial Term (or then-current Renewal Term) immediately accelerates and becomes due and payable in full within ten (10) business days** of Licensor's written demand. Master Agreement §3.3(e) applies to the accelerated amount.

**5.4 Sole exception.** Acceleration does not apply, and Licensee receives a refund of prepaid fees for the period after termination, only where Licensee terminates for Licensor's material breach left uncured for thirty (30) days after Licensee's detailed written notice, as set out in Master Agreement §3.3(f).

**5.5 Renewal.** After the Initial Term, this order form renews for successive twelve (12)-month Renewal Terms under Master Agreement §3.3(h), unless either Party gives written notice of non-renewal at least ninety (90) days before the end of the then-current term. Each Renewal Term is itself firm and non-cancellable.

---

## 6. Deliverables, Support & Maintenance

**6.1 Deliverables.** Production builds of the Animus Core engine (`libanimus.so`, Linux x86_64), the public C-ABI header interface (Pimpl-isolated from internal ring-buffer layouts), the nanobind-based zero-copy Python bridge, and the `replay_bench` benchmark verification harness, each as described in [`ANIMUS_ENTERPRISE_TERMSHEET.md`](ANIMUS_ENTERPRISE_TERMSHEET.md) §3. No source code is delivered.

**6.2 Engineering scope.** Custom C++/Python wire-schema engineering (`ANIMUS_DEFINE_SCHEMA`-registered schemas) for the Designated Desk, and kernel-bypass/NIC architecture consulting, as scoped in writing between the Parties during the Term.

**6.3 Support.** Dedicated Slack Connect channel and priority email, handled directly by Licensor's Founder & Chief Architect. Licensor will use **commercially reasonable efforts** to respond to critical trading-day incidents within **four (4) hours** during the regular trading hours of CME, Eurex, and LSE. This is a best-efforts target, not a guaranteed service level, and carries no service credits.

**6.4 Maintenance.** Quarterly optimization and compatibility releases (compiler, kernel, and instruction-set updates) during the Term at no additional charge.

---

## 7. Proprietary Technology & Source Access

**7.1 Protections.** Master Agreement §§3.2, 4, and 5 apply in full. Without limiting them, Licensee acknowledges that the lock-free ring-buffer data structures, invariant-TSC timestamping routines, and cache-line memory layouts of the Software are Licensor's Proprietary Technology; that the non-public elements of it are Licensor's trade secrets; and that the clean-room covenant in Master Agreement §3.2(d) binds Licensee and its contractors during the Term and for eighteen (18) months after it ends.

**7.2 No source access.** No source code, and no read-only inspection access, is provided under this tier.

**7.3 Escrow (optional).** On Licensee's written request, the Parties will negotiate a source-code escrow arrangement with an independent escrow agent, releasable only on Licensor's insolvency or cessation of business, or Licensor's uncured material failure to provide maintenance under §6.4. Escrow is a continuity safeguard only, not audit access, and requires a separate escrow agreement, which will allocate any escrow agent fees. No escrow account exists as of the Order Effective Date.

---

## 8. Governing Law & Disputes

Master Agreement §9.2 applies: laws of **India**; SIAC arbitration seated in **Singapore**, one arbitrator, English language; either Party may seek interim or injunctive relief from a court of competent jurisdiction, including to enforce §5 or §7.

---

## Schedule A — Designated Desk

| Field | Detail |
|---|---|
| Designated Asset Class | [e.g., US Equities / Listed Futures / FX / Rates] |
| Designated Desk (name and internal identifier) | [DESK NAME / ID] |
| Desk owner (business contact) | [NAME, TITLE, EMAIL] |
| Technical contact | [NAME, TITLE, EMAIL] |
| Deployment locations | [e.g., Equinix NY4 — Secaucus, NJ] |
| Expected production node count at start | [N] (informational only; node count is unlimited within the Designated Desk) |

## Schedule B — Monthly Payment Schedule

| Installment | Contract Month | Invoice issued | Due | Amount (USD) |
|---|---|---|---|---|
| 1 | Month 1 | Order Effective Date | Net-15 from invoice | $20,000 |
| 2 | Month 2 | 15 days before Month 2 begins | Net-15 from invoice | $20,000 |
| 3 | Month 3 | 15 days before Month 3 begins | Net-15 from invoice | $20,000 |
| 4 | Month 4 | 15 days before Month 4 begins | Net-15 from invoice | $20,000 |
| 5 | Month 5 | 15 days before Month 5 begins | Net-15 from invoice | $20,000 |
| 6 | Month 6 | 15 days before Month 6 begins | Net-15 from invoice | $20,000 |
| 7 | Month 7 | 15 days before Month 7 begins | Net-15 from invoice | $20,000 |
| 8 | Month 8 | 15 days before Month 8 begins | Net-15 from invoice | $20,000 |
| 9 | Month 9 | 15 days before Month 9 begins | Net-15 from invoice | $20,000 |
| 10 | Month 10 | 15 days before Month 10 begins | Net-15 from invoice | $20,000 |
| 11 | Month 11 | 15 days before Month 11 begins | Net-15 from invoice | $20,000 |
| 12 | Month 12 | 15 days before Month 12 begins | Net-15 from invoice | $20,000 |
| | | | **Committed Fees** | **$240,000** |

Each invoice is issued fifteen (15) calendar days before the contract month it covers begins (Month 1 is invoiced on the Order Effective Date) and is due Net-15 from the invoice date. All installments are subject to acceleration under §5.3.

---

## Signature Block

By signing, each Party agrees to this order form and the Master Agreement it incorporates. Licensee specifically acknowledges §5 (firm, non-cancellable twelve-month term and fee acceleration).

| | Licensor | Licensee |
|---|---|---|
| **Entity** | Animus Technologies Private Limited | [CLIENT LEGAL ENTITY NAME] |
| **Signature** | ___________________________ | ___________________________ |
| **Name** | Alakshendra Roy | [PRINTED NAME] |
| **Title** | Founder & Director | [TITLE] |
| **Date** | ________ | ________ |
| **Billing option selected** | — | ☐ Standard — Monthly ($20,000/month) ☐ Option A ($228,000 annual prepay) |
| **Notice Address** | [ANIMUS ADDRESS — registered office address to be added upon issuance of the Certificate of Incorporation]; inquiries@animusinfra.com | [CLIENT ADDRESS] |

---

> ## ⚠️ REMINDER
> Do not issue or execute until a licensed attorney has reviewed this order form with the Master Agreement — in particular §5's acceleration mechanics (penalty analysis under Section 74 of the Indian Contract Act, 1872), §4.4's withholding gross-up, §7.1's clean-room covenant, and the SIAC clause's fit against Licensee's jurisdiction — and the CIN has been issued.
