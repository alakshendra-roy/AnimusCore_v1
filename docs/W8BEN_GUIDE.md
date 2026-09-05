# IRS Form W-8BEN — Reference Guide for US Client Payments to Animus Infrastructure

> ## ⚠️ SUPERSEDED FOR POST-INCORPORATION PAYMENTS
> Animus is incorporating as **Animus Technologies Private Limited** (see [`../LEGAL_INCORPORATION_BRIEF.md`](../LEGAL_INCORPORATION_BRIEF.md)). Once the Certificate of Incorporation issues and payments flow to the company rather than to Alakshendra Roy in his individual/sole-proprietor capacity, **US customer withholding-tax documentation must be Form W-8BEN-E (entity), not Form W-8BEN (individual)** — this is exactly the fork this guide's own §1 already anticipated. Form W-8BEN-E is a materially different form (entity classification, Chapter 3/4 status, GIIN where applicable) and is **not covered by this document**.
>
> This guide remains applicable only to: (a) payments received during the pre-incorporation sole-proprietorship period, and (b) as background on the individual-form mechanics for reference. **Do not use this guide's Form W-8BEN walkthrough for any post-incorporation US customer payment.** A separate W-8BEN-E guide is needed before the first post-incorporation US invoice goes out — see `../LEGAL_INCORPORATION_BRIEF.md` §5.3, which flags this as a required follow-up.

> ## ⚠️ NOT TAX OR LEGAL ADVICE — REFERENCE ONLY
> This is a general reference document written by an AI coding assistant, following the same disclaimer convention as this repository's other legal templates ([`PILOT_CONTRACT.md`](PILOT_CONTRACT.md), [`PILOT_AGREEMENT.md`](PILOT_AGREEMENT.md), [`LEGAL_EULA.md`](../LEGAL_EULA.md)). Form W-8BEN is a **perjury-attested IRS certification** — getting it wrong has real financial (incorrect 30% withholding, or under-withholding penalties for the US client) and compliance consequences. Treaty article numbers, the "fees for included services" vs. "royalties" vs. "business profits" characterization, and the exact treaty-capped withholding rate below are **stated as general, commonly-cited reference points, not verified current figures** — confirm every rate and characterization against the current treaty text and IRS Publication 515 / the IRS tax treaty table, with a CPA or tax attorney licensed to handle US–India cross-border taxation, before this form is ever signed and submitted. This document does not replace that review.

---

## 1. Who this is for, and why

**Alakshendra Roy** is an individual tax resident of India. When a US-based institutional client (e.g., a Proof-of-Performance counterparty under [`PILOT_CONTRACT.md`](PILOT_CONTRACT.md)) pays Animus Infrastructure for services or a software license, US tax law generally requires that client — acting as a **withholding agent** — to withhold **30% of the gross payment** as US tax on US-source income paid to a foreign person, *unless* a valid, properly-completed **Form W-8BEN** is on file certifying foreign status and (where applicable) a reduced treaty rate.

**Form W-8BEN is for individuals.** If Animus Infrastructure is (or becomes) a separate registered legal entity — an LLC, corporation, or similar — rather than Alakshendra Roy contracting in his individual/sole-proprietor capacity, the *entity* form is **W-8BEN-E**, not W-8BEN, and the analysis below (particularly IP/entity classification and treaty-eligibility) changes materially. **Confirm actual business structure with a CPA before deciding which form applies** — this guide assumes payments flow to Alakshendra Roy as an individual.

Form W-8BEN does two things:
1. **Certifies foreign (non-US) status**, which by itself avoids default backup withholding that applies to undocumented payees.
2. **Claims a reduced withholding rate under the US–India Double Taxation Avoidance Agreement (DTAA)**, where applicable — potentially reducing the 30% statutory rate substantially, or to 0% if the income qualifies as business profits with no US permanent establishment (PE).

---

## 2. The US–India DTAA — the two relevant articles

| Article | Covers | Typical outcome | When it applies |
|---|---|---|---|
| **Article 7 — Business Profits** | Profits of an Indian enterprise/individual from business activity | **0% US tax**, if there is no US permanent establishment | Applies when the payment is genuinely for independent business activity conducted from India, with no fixed place of business, dependent agent, or extended physical presence in the US |
| **Article 12 — Royalties and Fees for Included Services** | Payments for the use of (or right to use) IP — software licenses, technical know-how — and for certain technical/consultancy services that "make available" technical knowledge | **Treaty-capped withholding rate, commonly cited around 15%** (verify current rate — see disclaimer above) | Applies when the payment is characterized as a royalty (e.g., ongoing license fees for Software use) or as "fees for included services" (e.g., a PoP engagement fee where deliverables include technical know-how transfer) |

**Characterization matters and is not self-evident.** A lump-sum Proof-of-Performance engagement fee for services performed, a per-seat or per-node annual license fee, and a one-time source-code license sale may fall into different categories — this determination should be made deal-by-deal with a CPA, not assumed. Line 10 of the form (§4 below) must name the specific article and, for Article 12 claims, the type of income and rate actually being claimed.

---

## 3. Form W-8BEN, part by part

*(Line numbers below match the Rev. October 2021 revision of Form W-8BEN — always pull the current version from IRS.gov before filing, since the IRS periodically revises the form.)*

### Part I — Identification of Beneficial Owner

| Line | Field | What goes here |
|---|---|---|
| 1 | Name of individual | Alakshendra Roy (full legal name, matching PAN and passport) |
| 2 | Country of citizenship | India |
| 3 | Permanent residence address | Actual India residential address — **not** a US address or a PO box; a US address here can invalidate the foreign-status claim |
| 4 | Mailing address | Only if different from Line 3 |
| 5 | U.S. taxpayer identification number (SSN or ITIN) | Leave blank only if not required for the specific treaty claim being made on Line 10 (see §5 below — for some Article 12 claims, the IRS instructions require one; confirm with a CPA whether an ITIN application (Form W-7) is needed before this form can support the claimed rate) |
| 6a | Foreign tax identifying number (FTIN) | **PAN (Permanent Account Number)** — the 10-character alphanumeric ID issued by the Indian Income Tax Department (format: `AAAAA9999A`) |
| 6b | FTIN not legally required (checkbox) | Do **not** check this box if a PAN exists and is being provided on Line 6a — India does require/issue PAN broadly to individuals with taxable presence or financial activity, so the exemption checkbox is generally inapplicable once a PAN exists |
| 7 | Reference number(s) | Optional — the paying client's internal vendor/contract reference, if requested by their AP team |
| 8 | Date of birth | **MM-DD-YYYY** — see justification below |

> **Line 6a justification.** The IRS's own W-8BEN instructions direct a foreign individual without a US SSN/ITIN to provide the tax identifying number issued by their country of residence — PAN is the Indian government's equivalent identifier and is the field the instructions are pointing to for an Indian filer. Providing it (rather than checking the Line 6b "not legally required" exemption) is treated by most withholding agents as a precondition to accepting a Line 10 treaty claim at all: an incomplete or exemption-checked Line 6a is a common, avoidable reason an AP department defaults a payment to the 30% statutory withholding rate instead of processing the treaty claim. **Caveat:** whether Line 6a is strictly *legally* mandatory in every case (vs. Line 6b's exemption genuinely applying) is a facts-and-circumstances reading of the current IRS instructions — treat "always provide PAN, never check 6b, when a PAN exists" as the safe operational default here, not as a claim that no exemption scenario could ever exist for anyone.
>
> **Line 8 justification.** The form's own instructions specify the date-of-birth format as MM-DD-YYYY, matching standard US date convention rather than the DD/MM/YYYY convention used in India and most of the rest of the world. This is worth calling out explicitly because it is a common, silent failure point: a withholding agent's AP intake process, a bank's document-verification OCR step, or manual data entry against IRS records can misread or reject a DD/MM/YYYY-formatted date (e.g., `03-09-2026` is unambiguous only if the reader already knows which convention was used) — following the form's stated format removes that ambiguity rather than relying on a reader to infer intent correctly.

### Part II — Claim of Tax Treaty Benefits

| Line | Field | What goes here |
|---|---|---|
| 9 | Country claiming treaty benefits | India — with the certification that the individual is a resident of India within the meaning of the US–India income tax treaty |
| 10 | Special rates and conditions | Name the **specific article** (Article 7 or Article 12 — see §2), the **type of income** (e.g., "fees for included services" / "royalties" / "business profits"), and the **withholding rate being claimed** (0% under Article 7, or the current Article 12 treaty rate). This line requires an affirmative statement of *why* the reduced rate applies (e.g., no US permanent establishment) — a CPA should draft the exact wording per payment type. |

> **Line 10 justification — the mechanism, and what's actually being certified.** Absent a valid treaty claim, US Internal Revenue Code Chapter 3 imposes a flat **30% gross withholding rate** on US-source FDAP-type payments to a foreign person — that is the default Line 10 exists to displace. Article 7 ("Business Profits") of the US–India DTAA provides that an Indian resident's business profits are **not taxable in the US at all (0%)** where the Indian resident has no US permanent establishment (PE) — no fixed place of business, dependent agent, or extended physical presence in the US through which the business is carried on. For Animus specifically, the operational premise for claiming Article 7 at 0% is that software licensing, telemetry benchmarking, and technical integration services are performed entirely remotely from India, with no US PE. **This is a certification the filer makes under penalty of perjury about their own actual operating facts — this guide can explain the mechanism and the standard it depends on, but only the filer (with a CPA, if there's any doubt about the PE analysis or whether a given payment is really "business profits" versus a royalty or fee-for-included-services under Article 12 instead) can attest that those facts are true for a specific engagement.** Confirm PE status and income characterization deal-by-deal before signing, not once and assumed to carry forward automatically to every future payment type.

### Part III — Certification

Signature (handwritten or the form's accepted electronic-signature process, per the withholding agent's policy), printed name, and date. If signed by an agent under power of attorney rather than Alakshendra Roy personally, a valid POA (Form 2848 or equivalent) must be attached.

> **Part III justification.** The signed date starts the **3-full-calendar-year statutory validity horizon** detailed in §4 below (through December 31 of the third year following signing — e.g., December 31, 2029 for a form signed anytime in 2026), unless a change in circumstances ends it sooner. That validity period matters beyond just "don't let the form lapse": it is the documentary basis the withholding agent relies on to support **zero-withholding reporting on IRS Form 1042-S** for every payment made while the form is current — the withholding agent's compliance file, not just the filer's, depends on a validly executed and still-current Part III. A lapsed or improperly executed certification is a real audit-exposure item for the *paying* institution, which is exactly why an AP department will not process a reduced or zero rate without one on file (see §5 below).

---

## 4. Validity period and renewal

A properly completed Form W-8BEN is generally valid **through the last day of the third calendar year following the year it is signed** (e.g., a form signed in 2026 is valid through December 31, 2029), *unless* a change in circumstances occurs sooner — a change of permanent residence out of India, a change in PAN, or acquisition of US person status, any of which requires a new form within 30 days. Track expiry per client relationship; a lapsed form reverts the payer to default 30% withholding on the next payment.

---

## 5. Instructions for the paying client's Accounts Payable / withholding-agent team

For the US institutional client's AP or tax-compliance team processing payments under a PoP engagement or license:

1. **Collect a signed, dated Form W-8BEN before the first payment** — not after. Withholding at the reduced treaty rate cannot be applied retroactively to a payment already made without a valid form on file at the time of payment.
2. **Retain the form on file** — it is not filed with the IRS by the withholding agent, but must be retained and produced if the IRS examines the withholding determination.
3. **Verify Line 10's treaty claim is complete** (article, income type, and rate all stated, with the required explanation) before applying anything less than 30% withholding — an incomplete Line 10 does not support a reduced rate.
4. **Confirm whether a U.S. TIN (Line 5) is required** for the specific treaty claim being made — some claim types require one; applying a reduced rate without a required TIN is a compliance risk for the withholding agent, not just the payee.
5. **Report annually on Form 1042-S** (not Form 1099, which is for US persons) — showing gross payment and any tax withheld, with a copy furnished to Alakshendra Roy for his own Indian tax filing (foreign tax credit purposes) and to the IRS.
6. **Track the 3-year expiry** (§4) and request a renewed form before it lapses to avoid an unwanted reversion to 30% withholding on a payment that should qualify for treaty relief.

---

## 6. Common pitfalls

- **Name mismatch** — the name on the form, PAN, and the contract/invoice must match; discrepancies are a common reason a withholding agent rejects a treaty claim.
- **US address on Line 3** — undermines the entire foreign-status certification; use the actual India residence.
- **Checking the Line 6b FTIN-exemption box when a PAN exists** — likely incorrect for an Indian individual with a PAN; leave PAN on Line 6a instead.
- **Vague or blank Line 10** — "India — treaty benefits" with no article, income type, or rate is not sufficient to support anything less than 30% withholding.
- **Assuming Article 7 (0%) applies by default** — it only applies where there is genuinely no US permanent establishment and the income is properly characterized as business profits, not a royalty or FIS payment; this is a substantive determination, not a form-filling default.
- **Letting the form lapse** — losing treaty benefit on a payment purely because the 3-year window passed unnoticed.

---

*For the underlying commercial relationship this guide supports, see [`PILOT_CONTRACT.md`](PILOT_CONTRACT.md) (Proof-of-Performance engagement fees) and [`COMMERCIAL_OVERVIEW.md`](../COMMERCIAL_OVERVIEW.md) (ongoing license fees) — both may generate US-source payments subject to the withholding analysis above, and each payment type should be re-characterized (§2) rather than assumed to share one treaty treatment.*
