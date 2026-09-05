# Legal & Corporate Identity Verification Audit

> ## ⚠️ THIS IS A DOCUMENT-CONSISTENCY CHECK, NOT A LEGAL OPINION
> This audit cross-reads the legal/tax templates already in this repository ([`PILOT_CONTRACT.md`](PILOT_CONTRACT.md), [`PILOT_PROGRAM.md`](PILOT_PROGRAM.md), [`W8BEN_GUIDE.md`](W8BEN_GUIDE.md), [`PILOT_AGREEMENT.md`](PILOT_AGREEMENT.md), [`../LEGAL_EULA.md`](../LEGAL_EULA.md)) for internal consistency and open placeholders. It reports what those documents say and where they conflict or leave gaps — it does **not** independently verify any fact about Animus's actual legal existence, registration, or tax status beyond what those documents themselves now state. Every finding below that still says "unresolved" or "open" needs an actual answer from counsel/a CPA, not an assumption.
>
> **Status update:** the decision to incorporate as **Animus Technologies Private Limited** (India Private Limited company) has since been made and documented in [`../LEGAL_INCORPORATION_BRIEF.md`](../LEGAL_INCORPORATION_BRIEF.md), and propagated into `../LEGAL_EULA.md`, `../COMMERCIAL.md`, `../COMPLIANCE_AND_RISK_MITIGATION.md`, and `PILOT_CONTRACT.md`. This resolves §1 and most of §2 below. The Certificate of Incorporation and CIN have **not yet issued** — every reference below is to the *decided structure*, not a completed filing. Sections are annotated **[RESOLVED]** / **[PARTIALLY RESOLVED]** / **[OPEN]** accordingly; original findings are left in place below each annotation for the historical record.

---

## 1. Operating status: Sole Proprietorship vs. incorporated entity — **[RESOLVED]**

> **Resolution:** the entity will be **Animus Technologies Private Limited**, a Private Limited Company under the Companies Act, 2013, incorporated in India — see `../LEGAL_INCORPORATION_BRIEF.md`. This is now reflected consistently in `../LEGAL_EULA.md` (Vendor party block and signature block), `../COMMERCIAL.md` §4.4 (Licensing entity), `../COMPLIANCE_AND_RISK_MITIGATION.md` §3.2 (Contractor IP Assignment template), and `PILOT_CONTRACT.md` §Parties/signature block — all four now name the same entity, with a `[CIN — to be inserted upon issuance of the Certificate of Incorporation]` placeholder pending actual filing. **`PILOT_AGREEMENT.md` was deliberately left on its generic `[Licensor Legal Name]` placeholder** and has not been updated to the new name — see the entity-naming-consistency note in §3.4 below, now the one remaining gap on this point.
>
> This also resolves the W-8BEN vs. W-8BEN-E fork: `W8BEN_GUIDE.md` now carries a superseded-notice stating that post-incorporation US payments require Form W-8BEN-E, not the individual W-8BEN that guide walks through. **A dedicated W-8BEN-E guide has not yet been written** — flagged as an open follow-up in `../LEGAL_INCORPORATION_BRIEF.md` §5.3.

**Original finding (historical):** not established anywhere in this repository — this is the single most important open item, and everything downstream depends on it.

- `PILOT_CONTRACT.md` §Parties deliberately left this as `[entity type / jurisdiction of organization — to be confirmed]` when the document was drafted, precisely because this fact was not known.
- `W8BEN_GUIDE.md` §1 explicitly assumes "payments flow to Alakshendra Roy as an individual" and flags: *"If Animus Infrastructure is (or becomes) a separate registered legal entity... rather than Alakshendra Roy contracting in his individual/sole-proprietor capacity, the entity form is W-8BEN-E, not W-8BEN."* That conditional was never resolved to a fact — it's still a live branch.
- No corporate registration document, incorporation certificate, GST registration, or similar exists anywhere in this repository to confirm either answer.

**This determines which IRS form applies (W-8BEN vs. W-8BEN-E), which in turn determines the treaty-eligibility mechanics in §2 below.** Do not proceed with either form as a final filing until this is resolved with a CA/company-secretary (India) or CPA (US-facing).

## 2. Jurisdiction and operational-address mapping — **[PARTIALLY RESOLVED]**

> **Resolution:** `PILOT_CONTRACT.md` §8.1 was switched from Delaware to India, aligning it with `LEGAL_EULA.md` §9.2, which was already set to India (that field was resolved before this audit's original findings below were written, so the "blank `[GOVERNING JURISDICTION]`" claim under it is stale). Current state:

| Document | Entity jurisdiction | Governing law | Venue |
|---|---|---|---|
| `PILOT_CONTRACT.md` | Animus Technologies Private Limited, India (CIN pending) | **India** (§8.1, updated) | **SIAC arbitration, seated in Singapore, 1 arbitrator, English** (§8.1, filled in as a founder-selected draft — see resolution note below) |
| `PILOT_AGREEMENT.md` | n/a (still uses generic "Licensor Legal Name" — **OPEN**, see §3.4) | `[Jurisdiction]` (still blank — **OPEN**) | not addressed (**OPEN**) |
| `LEGAL_EULA.md` | Animus Technologies Private Limited, India (CIN pending) | **India** (§9.2 — already resolved) | "courts of competent jurisdiction in India" (§9.2 — resolved to country level; no specific city/court named) |
| `W8BEN_GUIDE.md` | Superseded notice added; individual W-8BEN walkthrough retained for pre-incorporation reference only | n/a (tax form, not a contract) | n/a |

**Remaining open items under this section:**
- `PILOT_CONTRACT.md` §8.1's venue/arbitration forum is now filled in — **SIAC arbitration, seated in Singapore, one arbitrator, English language**, with a carve-out for interim/injunctive relief in court pending tribunal constitution — but this is a founder-selected draft, not counsel-vetted. Still open: whether SIAC is the right forum for the actual deal sizes this contract sees (vs. India-seated arbitration or straight litigation), and whether a Singapore-seated award is enforceable against the specific client jurisdictions actually in play.
- `PILOT_AGREEMENT.md` was intentionally left untouched (it never named a specific entity to begin with) and still carries a fully generic, unset `[Jurisdiction]` placeholder and no entity name at all — it has not been brought in line with the India-incorporation decision.

**Operational address:** still genuinely open. `LEGAL_EULA.md`'s signature block and `PILOT_CONTRACT.md`'s signature block both now consistently state that the registered-office/notice address will be added upon issuance of the Certificate of Incorporation (per `../LEGAL_INCORPORATION_BRIEF.md`), rather than the previous generic "formal entity registration" phrasing — but no real address exists yet, since the registered-office state itself is still an open `[STATE — to be confirmed by Promoter]` field in `../LEGAL_INCORPORATION_BRIEF.md` §1. There is still no populated source of truth for Animus's operational or notice address anywhere in this repo.

## 3. Cross-document alignment: liability cap, venue/arbitration, IP ringfencing

### 3.1 Liability cap

| Document | Cap structure | Mutual or one-sided? |
|---|---|---|
| `PILOT_CONTRACT.md` §6 | Capped at "the PoP Fee actually paid" | **Mutual** (applies to both Animus and Client) |
| `PILOT_AGREEMENT.md` §5 | `[USD $______ / the fees paid by Evaluator, if any]` | One-sided (Licensor's liability only) |
| `LEGAL_EULA.md` §7 | Fees paid in preceding 12 months, or `[CAP AMOUNT]` if none | One-sided (Vendor's liability only), with named carve-outs for gross negligence/willful misconduct and IP/confidentiality breaches |

**Conflict flagged:** the new paid-engagement contract uses a *mutual* liability cap; both older free-evaluation templates cap only the vendor's liability, leaving the customer's liability to Animus theoretically uncapped. This is a substantive, not cosmetic, difference — worth a deliberate decision (paid engagements might reasonably warrant symmetry that a free eval doesn't need), not an oversight to silently carry forward.

### 3.2 Venue / arbitration — **[PARTIALLY RESOLVED]**

> **Correction to the original finding below:** `LEGAL_EULA.md` §9.2 no longer uses a `[VENUE]` placeholder — it currently reads "the Parties submit to the exclusive jurisdiction of the courts of competent jurisdiction in India," which resolves venue to the country level (no specific city/court named, but no longer blank). This was already the case before the current incorporation-related updates; the claim below predates that fix and is left only for history.
>
> **Further resolution:** `PILOT_CONTRACT.md` §8.1 now specifies **SIAC arbitration, seated in Singapore, one arbitrator, English language**, with an interim-relief carve-out — a founder-selected draft, flagged for counsel confirmation on forum-fit and cross-border award enforceability rather than left blank.

**Original finding (historical):** none of the three templates has a filled-in venue or arbitration clause — `PILOT_CONTRACT.md` §8.1 explicitly marks it `[to be set by counsel]` (**superseded — see resolution above**), `LEGAL_EULA.md` §9.2 uses `[VENUE]` (**superseded — see correction above**), and `PILOT_AGREEMENT.md` doesn't address venue/arbitration at all (only a bare governing-law line — **still true today**). No conflict to reconcile here since none is decided yet — but note `PILOT_AGREEMENT.md`'s silence on venue/arbitration is itself a gap relative to the other two, which at least have a placeholder for it.

**Remaining gap:** `PILOT_AGREEMENT.md` still has no venue/arbitration provision at all (not even a placeholder), and now diverges further from `PILOT_CONTRACT.md`/`LEGAL_EULA.md`, both of which have a filled-in position. Worth a decision on whether the free-evaluation template should get a matching SIAC clause, or stay silent by design given its lower stakes.

### 3.3 IP ringfencing (Animus core IP vs. client alphas/data) — **[RESOLVED]**

> **Resolution:** `PILOT_AGREEMENT.md` §3 was split into §3.1 (Software IP, unchanged) and a new §3.2 (Evaluator Data and Output), mirroring `PILOT_CONTRACT.md` §4.2's language — Evaluator now retains 100% ownership of trading strategies, alpha signals, models, algorithms, market-data schemas, and any output produced by running that data through the Software during the Pilot Term, with Licensor acquiring no license or ownership interest in it.

| Document | Vendor/Animus IP ownership | Client/Customer IP ownership |
|---|---|---|
| `PILOT_CONTRACT.md` §4 | §4.1: Animus owns the C++ core, ring buffer, C-ABI, Python SDK, and derivative improvements | §4.2: Client retains **100%** ownership of trading strategies, alpha signals, models, schemas, and any output produced from running its data through the Software — explicit, named, and mutual |
| `LEGAL_EULA.md` §5 | §5.1: Vendor retains all IP in the Software | §5.4: "Customer retains all rights in the telemetry data, event streams, and other content Customer processes through the Software" — same principle, more generic wording (no explicit "alphas/models/schemas" language) |
| `PILOT_AGREEMENT.md` §3 | §3.1: Licensor owns all Software IP, including modifications/improvements | §3.2 (**new**): Evaluator retains 100% ownership of trading strategies, alpha signals, models, algorithms, schemas, and output produced during the Pilot Term |

**Original finding (historical):** `PILOT_AGREEMENT.md` — the free-evaluation template — never affirmatively stated that the evaluator retains ownership of their own data or output, unlike both other documents. For a prop-trading or market-maker evaluator running their own data through the engine even during a free 30-day evaluation, that silence was a real gap, not a neutral default.

**No conflict found** between the three templates on the core ownership split itself — all three now agree Animus/Vendor/Licensor owns the engine and the counterparty owns their own data/output.

### 3.4 Entity naming consistency — **[PARTIALLY RESOLVED]**

> **Resolution:** `PILOT_CONTRACT.md`, `LEGAL_EULA.md`, `COMMERCIAL.md` §4.4, and `COMPLIANCE_AND_RISK_MITIGATION.md` §3.2 now all consistently name **Animus Technologies Private Limited** (operating under the "Animus Core" / "Animus Infrastructure" brand names as used in each respective document), with a shared `[CIN — pending]` placeholder convention. That closes the original gap of "only one of the three names a party at all."

**Remaining open item:** `PILOT_AGREEMENT.md` was deliberately left on its generic `[Licensor Legal Name]` placeholder (it never named a specific entity, so there was no existing reference to update) and has **not** been brought forward to name Animus Technologies Private Limited. It remains the one template still fully genericized — worth a deliberate decision on whether to name the entity there too or keep it as an intentionally anonymized public template.

**Original finding (historical):** `PILOT_CONTRACT.md` named the vendor party as "Animus Infrastructure" (with Alakshendra Roy as Founder & Chief Architect) while both older templates used generic bracketed placeholders rather than that name.

---

## 4. Summary checklist for counsel / CPA

- [x] ~~Confirm Animus's actual legal structure~~ — **RESOLVED:** Animus Technologies Private Limited, a Private Limited Company under the Companies Act, 2013, incorporated in India. See `../LEGAL_INCORPORATION_BRIEF.md`. CIN issuance still pending — treat as decided-but-not-yet-filed.
- [x] ~~Reconcile governing-law choice~~ — **RESOLVED:** `PILOT_CONTRACT.md` §8.1 switched from Delaware to India; now aligned with `LEGAL_EULA.md` §9.2 (already India) and `../COMPLIANCE_AND_RISK_MITIGATION.md`.
- [ ] **Decide whether the liability cap should be mutual or one-sided** across paid vs. free-evaluation templates, and make it consistent within each category — still open; not addressed by the incorporation decision.
- [x] ~~Add a client-IP-retention clause to `PILOT_AGREEMENT.md`~~ — **RESOLVED:** new §3.2 (Evaluator Data and Output) added, mirroring `PILOT_CONTRACT.md` §4.2 / `LEGAL_EULA.md` §5.4.
- [ ] **Populate the operational/notice address** — still open. The legal-entity question is resolved, but the registered-office state itself is still an open field in `../LEGAL_INCORPORATION_BRIEF.md` §1, so no real address exists yet to populate into `LEGAL_EULA.md` / `PILOT_CONTRACT.md`.
- [x] ~~Fill the venue/arbitration clause in `PILOT_CONTRACT.md` §8.1~~ — **RESOLVED (founder draft, not counsel-vetted):** SIAC arbitration, seated in Singapore, one arbitrator, English, with an interim-relief court carve-out. Confirm forum-fit and Singapore-award enforceability against actual client jurisdictions before relying on it. `PILOT_AGREEMENT.md` still has no venue/arbitration provision at all — new gap, see §3.2.
- [ ] **Decide whether to name Animus Technologies Private Limited in `PILOT_AGREEMENT.md`**, or keep that template intentionally generic — new item, surfaced by the entity-naming rollout in §3.4.
- [ ] **Write a Form W-8BEN-E guide** for post-incorporation US customer payments — `W8BEN_GUIDE.md` now carries a superseded-notice for this reason, but the replacement guide itself doesn't exist yet. See `../LEGAL_INCORPORATION_BRIEF.md` §5.3.
- [ ] **Insert the actual CIN** into every `[CIN — to be inserted upon issuance of the Certificate of Incorporation]` placeholder (`LEGAL_EULA.md`, `COMMERCIAL.md`, `COMPLIANCE_AND_RISK_MITIGATION.md`, `PILOT_CONTRACT.md`) once the Certificate of Incorporation issues.

None of the still-open items above are defaults this audit can safely choose on its own — each remains flagged for the reasons stated in the sections above.
