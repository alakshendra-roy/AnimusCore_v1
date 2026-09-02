# Legal & Corporate Identity Verification Audit

> ## ⚠️ THIS IS A DOCUMENT-CONSISTENCY CHECK, NOT A LEGAL OPINION
> This audit cross-reads the legal/tax templates already in this repository ([`PILOT_CONTRACT.md`](PILOT_CONTRACT.md), [`PILOT_PROGRAM.md`](PILOT_PROGRAM.md), [`W8BEN_GUIDE.md`](W8BEN_GUIDE.md), [`PILOT_AGREEMENT.md`](PILOT_AGREEMENT.md), [`../LEGAL_EULA.md`](../LEGAL_EULA.md)) for internal consistency and open placeholders. It reports what those documents say and where they conflict or leave gaps — it does **not** independently verify any fact about Animus Infrastructure's actual legal existence, registration, or tax status, none of which is established anywhere in this codebase. Every finding below that says "unresolved" or "not established in this repo" needs an actual answer from counsel/a CPA, not an assumption.

---

## 1. Operating status: Sole Proprietorship vs. incorporated entity

**Finding: not established anywhere in this repository — this is the single most important open item, and everything downstream depends on it.**

- `PILOT_CONTRACT.md` §Parties deliberately left this as `[entity type / jurisdiction of organization — to be confirmed]` when the document was drafted, precisely because this fact was not known.
- `W8BEN_GUIDE.md` §1 explicitly assumes "payments flow to Alakshendra Roy as an individual" and flags: *"If Animus Infrastructure is (or becomes) a separate registered legal entity... rather than Alakshendra Roy contracting in his individual/sole-proprietor capacity, the entity form is W-8BEN-E, not W-8BEN."* That conditional was never resolved to a fact — it's still a live branch.
- No corporate registration document, incorporation certificate, GST registration, or similar exists anywhere in this repository to confirm either answer.

**This determines which IRS form applies (W-8BEN vs. W-8BEN-E), which in turn determines the treaty-eligibility mechanics in §2 below.** Do not proceed with either form as a final filing until this is resolved with a CA/company-secretary (India) or CPA (US-facing).

## 2. Jurisdiction and operational-address mapping

| Document | Entity jurisdiction | Governing law | Venue |
|---|---|---|---|
| `PILOT_CONTRACT.md` | `[to be confirmed]` (blank) | **Delaware** (explicitly set, §8.1) | `[to be set by counsel]` (blank) |
| `PILOT_AGREEMENT.md` | n/a (uses generic "Licensor Legal Name") | `[Jurisdiction]` (blank) | not addressed |
| `LEGAL_EULA.md` | `[JURISDICTION OF INCORPORATION]` (blank) | `[GOVERNING JURISDICTION]` (blank) | `[VENUE]` (blank) |
| `W8BEN_GUIDE.md` | Individual, India tax residency (Line 3 = actual India address) | n/a (tax form, not a contract) | n/a |

**Inconsistency flagged:** `PILOT_CONTRACT.md` is the only template with a concrete governing-law selection (Delaware); the two older templates (`PILOT_AGREEMENT.md`, `LEGAL_EULA.md`) still carry unset jurisdiction placeholders. If Delaware is the intended choice going forward, those two should be updated to match — or, if they're meant to stay India-governed (consistent with the W-8BEN's India-residency certification and a sole-proprietorship structure), `PILOT_CONTRACT.md`'s Delaware selection should be revisited instead. **This is a decision for counsel, not something to default silently in either direction** — a Delaware choice-of-law clause for an India-resident individual with no stated US entity is exactly the kind of clause a reviewing attorney flagged as needing work when `PILOT_CONTRACT.md` was drafted.

**Operational address:** the only address referenced anywhere is the India permanent-residence address `W8BEN_GUIDE.md` Line 3 calls for — and even that is described generically ("actual India residential address"), not populated with a real value in this repo. `PILOT_CONTRACT.md`'s signature block still carries `[ANIMUS ADDRESS]` as an unfilled placeholder. There is currently no single source of truth in this repo for Animus Infrastructure's operational or notice address.

## 3. Cross-document alignment: liability cap, venue/arbitration, IP ringfencing

### 3.1 Liability cap

| Document | Cap structure | Mutual or one-sided? |
|---|---|---|
| `PILOT_CONTRACT.md` §6 | Capped at "the PoP Fee actually paid" | **Mutual** (applies to both Animus and Client) |
| `PILOT_AGREEMENT.md` §5 | `[USD $______ / the fees paid by Evaluator, if any]` | One-sided (Licensor's liability only) |
| `LEGAL_EULA.md` §7 | Fees paid in preceding 12 months, or `[CAP AMOUNT]` if none | One-sided (Vendor's liability only), with named carve-outs for gross negligence/willful misconduct and IP/confidentiality breaches |

**Conflict flagged:** the new paid-engagement contract uses a *mutual* liability cap; both older free-evaluation templates cap only the vendor's liability, leaving the customer's liability to Animus theoretically uncapped. This is a substantive, not cosmetic, difference — worth a deliberate decision (paid engagements might reasonably warrant symmetry that a free eval doesn't need), not an oversight to silently carry forward.

### 3.2 Venue / arbitration

None of the three templates has a filled-in venue or arbitration clause — `PILOT_CONTRACT.md` §8.1 explicitly marks it `[to be set by counsel]`, `LEGAL_EULA.md` §9.2 uses `[VENUE]`, and `PILOT_AGREEMENT.md` doesn't address venue/arbitration at all (only a bare governing-law line). No conflict to reconcile here since none is decided yet — but note `PILOT_AGREEMENT.md`'s silence on venue/arbitration is itself a gap relative to the other two, which at least have a placeholder for it.

### 3.3 IP ringfencing (Animus core IP vs. client alphas/data)

| Document | Vendor/Animus IP ownership | Client/Customer IP ownership |
|---|---|---|
| `PILOT_CONTRACT.md` §4 | §4.1: Animus owns the C++ core, ring buffer, C-ABI, Python SDK, and derivative improvements | §4.2: Client retains **100%** ownership of trading strategies, alpha signals, models, schemas, and any output produced from running its data through the Software — explicit, named, and mutual |
| `LEGAL_EULA.md` §5 | §5.1: Vendor retains all IP in the Software | §5.4: "Customer retains all rights in the telemetry data, event streams, and other content Customer processes through the Software" — same principle, more generic wording (no explicit "alphas/models/schemas" language) |
| `PILOT_AGREEMENT.md` §3 | Licensor owns all Software IP, including modifications/improvements | **No corresponding customer-IP-retention clause at all** |

**Gap flagged:** `PILOT_AGREEMENT.md` — the free-evaluation template — never affirmatively states that the evaluator retains ownership of their own data or output, unlike both other documents. For a prop-trading or market-maker evaluator running their own data through the engine even during a *free* 30-day evaluation, this silence is a real gap, not a neutral default: an institutional counterparty's legal team is likely to flag it. Recommend adding a clause mirroring `LEGAL_EULA.md` §5.4 / `PILOT_CONTRACT.md` §4.2 to `PILOT_AGREEMENT.md` before it's ever used with an institutional evaluator.

**No conflict found** between `PILOT_CONTRACT.md` and `LEGAL_EULA.md` on the core ownership split itself — both agree Animus/Vendor owns the engine and the counterparty owns their own data/output. The difference is specificity of wording, not substance.

### 3.4 Entity naming consistency

`PILOT_CONTRACT.md` names the vendor party as **"Animus Infrastructure"** (with Alakshendra Roy as Founder & Chief Architect). Both older templates still use generic bracketed placeholders (`[VENDOR LEGAL ENTITY NAME]`, `[Licensor Legal Name]`) rather than that name. Once §1 above (operating status) is resolved and an actual legal entity name is confirmed, all three templates should be updated to reference the same name consistently — right now only one of the three names a party at all.

---

## 4. Summary checklist for counsel / CPA

- [ ] **Confirm Animus Infrastructure's actual legal structure** (sole proprietorship under Alakshendra Roy vs. an incorporated entity, and in which jurisdiction) — blocks the W-8BEN vs. W-8BEN-E decision, the entity-name fields across all three contract templates, and the governing-law reconciliation below.
- [ ] **Reconcile governing-law choice** across `PILOT_CONTRACT.md` (Delaware, set) vs. `PILOT_AGREEMENT.md` / `LEGAL_EULA.md` (unset) — decide whether all three should align, and whether Delaware is actually appropriate given no US entity is currently established in this repo's own documents.
- [ ] **Decide whether the liability cap should be mutual or one-sided** across paid vs. free-evaluation templates, and make it consistent within each category.
- [ ] **Add a client-IP-retention clause to `PILOT_AGREEMENT.md`** to close the gap identified in §3.3.
- [ ] **Populate the operational/notice address** once the legal entity question is resolved — currently blank everywhere it's referenced.
- [ ] **Fill the venue/arbitration clause** in `PILOT_CONTRACT.md` §8.1 and `LEGAL_EULA.md` §9.2.

None of these are defaults this audit can safely choose on its own — each is flagged as an open item for the reasons stated above.
