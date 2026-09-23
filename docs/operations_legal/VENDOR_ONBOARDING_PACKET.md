# Vendor Onboarding Packet — Response Template for Institutional AP Departments

> ## ⚠️ TEMPLATE — CONFIRM EVERY BRACKETED ITEM BEFORE SENDING
> This is a response template for when a Tier-A prospect's Accounts Payable / vendor-compliance team asks for standard onboarding information before their first payment. It assumes the open items in [`LEGAL_VERIFICATION_AUDIT.md`](LEGAL_VERIFICATION_AUDIT.md) — most importantly, **Animus Infrastructure's actual legal structure** — are resolved before this is sent, since several fields below depend on that answer. **Do not send this with placeholder values still in it**, and confirm the W-8BEN treaty position (§2) with a CPA before attaching a signed form — see [`W8BEN_GUIDE.md`](W8BEN_GUIDE.md).

---

## Suggested email / portal response

> Subject: Vendor Onboarding Information — Animus Infrastructure
>
> Hi [AP Contact Name],
>
> Thanks for the onboarding request. Below is our standard vendor packet — let me know if your system needs anything in a different format.

### 1. Corporate legal entity

| Field | Value |
|---|---|
| Legal entity name | **[Animus Infrastructure / Alakshendra Roy — confirm exact legal name per LEGAL_VERIFICATION_AUDIT.md §1]** |
| Entity structure | **[Sole Proprietorship / Private Limited / LLP — confirm per LEGAL_VERIFICATION_AUDIT.md §1; do not guess]** |
| Country of incorporation / registration | **[India / other — confirm]** |
| Registered / operational address | **[street, city, PIN, India — confirm; currently unfilled in every template in this repo]** |
| Primary contact | Alakshendra Roy, Founder & Chief Architect |
| Contact email | alakshendra@animusinfra.com |
| Contact phone | +91 9891161189 |
| Website | animusinfra.com |

### 2. US tax withholding — Form W-8BEN

Attached is a completed Form W-8BEN certifying Indian tax residency under the US–India income tax treaty. Two possible positions, **pick the one that matches how your AP team characterizes this payment** (see `W8BEN_GUIDE.md` §2 for the full explanation — this is a real determination, not interchangeable boilerplate):

- **If this payment is for services / business profits with no US permanent establishment** (e.g., the Proof-of-Performance engagement fee under `PILOT_CONTRACT.md`): Form claims **Article 7, 0% withholding**.
- **If your AP team characterizes this as a software royalty** (e.g., an ongoing production license fee): Form should instead claim **Article 12(2)**, at the treaty-capped rate — **confirm the current rate with your tax team; commonly cited around 15%, but verify against the current IRS treaty table before either side relies on it.**

> Generated locally via `scripts/generate_w8ben.py --treaty-position [article7|article12]` from this repository, using the official IRS Form W-8BEN (Rev. October 2021). **The version attached to any real email must be hand-signed (or e-signed through a proper workflow) — the generator intentionally does not apply a digital signature.** Valid through the end of the third calendar year after signing (December 31, 2029 for a form signed in 2026) — that validity window is also what backs your own zero/reduced-withholding reporting on Form 1042-S, so treat a lapsed form as your compliance exposure too, not just ours.
>
> **What each field on the attached form represents, for your compliance review** (full detail in `W8BEN_GUIDE.md` §3):
> - **Line 6a** carries the Indian PAN as the foreign tax identifying number — populated (not exempted via 6b) because most withholding agents require it before a Line 10 treaty claim will be processed at all.
> - **Line 8** (date of birth) is formatted **MM-DD-YYYY** per the form's own instructions, specifically to avoid the DD/MM/YYYY-vs-MM/DD/YYYY ambiguity that trips up automated intake and OCR-based document verification.
> - **Line 10** certifies, under penalty of perjury, that the underlying work — software licensing, telemetry benchmarking, technical integration — is performed entirely remotely from India with no US permanent establishment, supporting a 0% rate under DTAA Article 7 in place of the 30% Internal Revenue Code default. **This is our certification of our own operating facts, not a claim your team needs to independently verify** — though you're welcome to ask questions about it before relying on it for your own withholding determination.

### 3. Payment / settlement instructions

| Field | Value |
|---|---|
| Preferred rail | Cross-border ACH / Fedwire via a virtual USD collection account |
| Provider | **[Skydo virtual USD account details — placeholder, populate from actual Skydo account dashboard before sending; never paste real account/routing numbers into this repository]** |
| Beneficiary name | **[must match Form W-8BEN Line 1 and the legal entity name in §1 above exactly — mismatches are a common reason AP holds a payment]** |
| Currency | USD |
| Intermediary bank details | **[if applicable — confirm with Skydo/banking partner]** |

**Do not commit real account or routing numbers to this repository or any git history** — this repo has a public GitHub remote. Populate this section only in the actual message sent to the client, from your live Skydo (or equivalent) dashboard, never by editing this template file with real numbers.

### 4. Business / tax registration disclosure

| Field | Value |
|---|---|
| D-U-N-S Number | **[if obtained — many Indian sole proprietorships do not have one; state "Not applicable — sole proprietorship" if accurate, or provide if registered]** |
| Indian GST registration number (GSTIN) | **[15-character GSTIN, format 22AAAAA0000A1Z5 — confirm export-of-services GST treatment with a CA before stating a number; cross-border services may be zero-rated but still require correct disclosure]** |
| PAN (Permanent Account Number) | **[10-character PAN — same value entered on Form W-8BEN Line 6a; must match exactly]** |
| Bank verification / voided cheque or account letter | Provided separately via secure channel, not this document |

> Standing by for any additional forms your compliance team requires (a completed vendor questionnaire, a signed NDA under `PILOT_CONTRACT.md` §3, or a bank verification letter). Happy to jump on a call if useful.
>
> Best,
> Alakshendra Roy
> Founder & Chief Architect — Animus Infrastructure

---

## Before sending, confirm:

- [ ] §1 entity structure matches the answer resolved in `LEGAL_VERIFICATION_AUDIT.md` §1 — not left as a guess.
- [ ] §2's treaty position (Article 7 vs. 12) matches how *this specific client's* AP team will characterize the payment — ask them if unsure, don't assume.
- [ ] The attached W-8BEN is actually signed (by hand or via a real e-signature flow) — the generator script does not sign it.
- [ ] §3's payment details are populated from the live Skydo dashboard in the outgoing email only, never committed to this file or this repository.
- [ ] §4's GSTIN/PAN figures are confirmed current and correctly formatted before disclosure.
- [ ] Beneficiary name in §3 matches the legal name on the W-8BEN exactly.
