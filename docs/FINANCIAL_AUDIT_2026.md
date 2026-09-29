# AnimusCore — 5-Year Financial Audit (₹17.87 Cr Target)

**Prepared:** 2026-09-25 · **Repriced:** 2026-09-30 (Institutional Enterprise / Tower row raised from $240k to $480k ACV; Sections 2–3 and the verdict figures recomputed; tax now applied to profit, not gross. Prop-desk and Global rows unchanged.)
**Scope:** Institutional CFO / enterprise HFT infrastructure sales audit of the ₹17.87 Cr, 5-year liquid net worth target modeled in `docs/financial_architecture/index.html`, re-tested against realistic enterprise sales-cycle dynamics for the named target account list.
**Nature of this document:** Informed industry-judgment modeling (deal-cycle norms, enterprise pricing structures, HFT security-review practices), not verified facts about the internal budgets, procurement policies, or vendor-adoption plans of any named firm. Treat all figures as planning estimates, not disclosed or confirmed data from those firms.

---

## Executive Verdict

The ₹17.87 Cr target is **not an enterprise-sales forecast — it's a personal-lifestyle-tier assumption wearing a business plan's clothes.** ₹15.70 Cr of it (88%) comes from the founder-draw waterfall in `financial_architecture/index.html`, which assumes $20k/mo of business revenue exists *today*, scaling to $120k/mo by Year 5 — with **zero contracts** behind that figure. The remaining ~₹4.32 Cr of "enterprise deals" layered on top in the prior session's five-year forecast panel were illustrative placeholders, not actual pipeline.

Re-running the numbers as a genuine B2B enterprise infrastructure sale into the actual named 9-account target list, with real sales-cycle physics (evaluation periods, security/code audits, sandboxed backpressure testing, net-60/90 payment terms) produces a materially different picture:

> **Even at a 100% win rate on every single named account, on an aggressive-but-plausible timeline, the ₹17.87 Cr target is not reached within 5 years.** Best case lands around **₹17.6 Cr** (98% of target — a near-miss, not a clean hit). A realistic (still optimistic) win rate lands around **₹2.9–5.2 Cr** — roughly 16–29% of the stated target.

---

## 1. Tier Pricing — Core License vs. Annual Production Colocation Retainer

| Segment | Accounts | Core License (Annual) | Annual Production Colo Retainer | Total ACV |
|---|---|---|---|---|
| Domestic Prop Desk | AlphaGrep, Graviton, iRage, Quadeye | $45,000 | $15,000 (33%) | **$60,000** |
| Domestic Enterprise | Tower Research (India engineering site) | $360,000 | $120,000 (33%) | **$480,000** |
| Global Market Maker | Wintermute, Flow Traders | $400,000 | $150,000 (37.5%) | **$550,000** |
| Global Tier-1 HFT/Prop | Jump Trading, Jane Street | $400,000 | $150,000 (37.5%) | **$550,000** (see risk note) |

The Retainer line is a cost center for the vendor too (dedicated benchmark/regression box access, priority patching, a named support contact) — see Section 3.

**Risk-adjustment on Jane Street:** OCaml-first, build-everything-in-house culture, no known precedent of licensing third-party native C++ trading infrastructure from an unproven single-founder vendor. Included in the pricing table for completeness; assigned near-zero realistic close probability inside a 5-year window.

---

## 2. Quarterly Revenue & Cash Collection — Best-Case Scenario

**Assumptions:** 100% win rate across all 9 named accounts. First outbound to all 5 domestic accounts (iRage, AlphaGrep, Quadeye, Graviton, Tower) went out 2026-09-24/25 — Day 0–1 as of this audit. Jump Trading and Jane Street are live in a separate, less-advanced outbound motion. A 6–9 month evaluation window plus security/code audit, sandboxed backpressure testing, and legal/procurement realistically puts the **earliest possible signature in Year 1, Quarter 4**, for the single fastest-moving small prop desk. Larger and global accounts take longer, not shorter — HFT security review does not compress under founder urgency. Cash collection is modeled one quarter behind revenue recognition (net-60/90 approximation).

| Qtr | Signing this quarter | Active ACV run-rate | Revenue recognized | Cash collected |
|---|---|---|---|---|
| Y1 Q1 | — | $0 | $0 | $0 |
| Y1 Q2 | — | $0 | $0 | $0 |
| Y1 Q3 | — | $0 | $0 | $0 |
| Y1 Q4 | iRage signs | $0 (live from Q5) | $0 | $0 |
| Y2 Q1 | — | $60k | $15k | $0 |
| Y2 Q2 | AlphaGrep signs | $60k | $15k | $15k |
| Y2 Q3 | — | $120k | $30k | $15k |
| Y2 Q4 | Quadeye signs | $120k | $30k | $30k |
| Y3 Q1 | — | $180k | $45k | $30k |
| Y3 Q2 | Graviton signs | $180k | $45k | $45k |
| Y3 Q3 | — | $240k | $60k | $45k |
| Y3 Q4 | Tower signs | $240k | $60k | $60k |
| Y4 Q1 | Wintermute signs | $720k | $180k | $60k |
| Y4 Q2 | — | $1,270k | $317.5k | $180k |
| Y4 Q3 | Flow Traders signs | $1,270k | $317.5k | $317.5k |
| Y4 Q4 | — | $1,820k | $455k | $317.5k |
| Y5 Q1 | Jump Trading signs | $1,820k | $455k | $455k |
| Y5 Q2 | — | $2,370k | $592.5k | $455k |
| Y5 Q3 | Jane Street signs | $2,370k | $592.5k | $592.5k |
| Y5 Q4 | — | $2,920k | $730k | $592.5k |

### Annual Rollup

| Year | Revenue Recognized | Cash Collected |
|---|---|---|
| Y1 | $0 | $0 |
| Y2 | $90k | $60k |
| Y3 | $210k | $180k |
| Y4 | $1,270k | $875k |
| Y5 | $2,370k | $2,095k |
| **5-Yr Total** | **$3.94M** | **$3.21M** |

Note the $730k gap between the two totals: Q20's revenue has not converted to cash by the end of Year 5 — it is still in net-60/90 transit into Year 6. **Revenue recognized and cash in hand are not the same number, and only cash compounds.**

$3.21M cash collected @ ₹90/$1 = **₹28.89 Cr gross, best case, 100% win rate on every named account.**

---

## 3. Unmodeled Opex — Costs the Original Model Never Included

`financial_architecture/index.html` was built around a solo operator with only cloud infra, a CA/compliance retainer, vehicle lease, and fuel allowance — no engineering headcount, no compliance spend. That cost structure is incompatible with actually selling into Tier-1 HFT shops.

| Item | Timing | Est. Annual Cost | 5-Yr Impact |
|---|---|---|---|
| Senior systems support engineer (fully loaded) | Hired Year 3 | ₹1.2 Cr/yr (~$135k) | ₹3.6 Cr (Y3–Y5) |
| Dedicated colo/benchmark boxes (2–4 accounts) | From Year 3 | ~₹30–50L/yr | ~₹1.5 Cr |
| Initial SOC 2 Type II audit (or equivalent) | **Required before a Year 1 close is realistic at all** | ~$50k one-time (₹45L) | ₹0.45 Cr |
| Annual SOC 2 recertification | Y3–Y5 | ~$25k/yr | ₹0.8 Cr |
| **Total unmodeled opex** | | | **~₹6.35 Cr** |

### Recomputed Realistic Grand Total

Corporate tax is applied to **profit** (cash collected less the opex above), since engineer, colo and SOC 2 costs are deductible. (An earlier draft taxed gross cash and then subtracted opex, which overstated tax by ~₹1.4 Cr.) Tax rate held at the model's 22%; the effective rate incl. surcharge/cess is nearer 25%, which would trim every retained figure below by roughly ₹0.5–0.9 Cr at best case.

```
₹28.89 Cr  (gross cash collected, 100% win rate)
− ₹6.35 Cr (unmodeled opex: engineer, colo, SOC 2)
─────────────────────────────────────────────────
  ₹22.54 Cr pre-tax profit
× 0.78     (after 22% corporate tax)
─────────────────────────────────────────────────
≈ ₹17.6 Cr  realistic BEST-CASE retained treasury
```

Against the ₹17.87 Cr target, **this is the ceiling** — 98% of target, and only if every one of the 9 named accounts closes, with zero churn, on an aggressive timeline. Any slippage in signing dates or a single lost account puts it below target.

Applying a realistic (still optimistic, given zero reference customers) **35–45% weighted win rate** to gross cash, with opex held fixed (the SOC 2 audit and support hire are needed to sell at all, whatever closes):

| Win rate | Gross cash | Pre-tax profit | Retained (×0.78) | % of target |
|---|---|---|---|---|
| 35% | ₹10.11 Cr | ₹3.76 Cr | **₹2.9 Cr** | 16% |
| 45% | ₹13.00 Cr | ₹6.65 Cr | **₹5.2 Cr** | 29% |

### What It Takes to Actually Hit ₹17.87 Cr

Working backward: retained ₹17.87 Cr ⇒ pre-tax profit ₹22.91 Cr ⇒ **gross cash ≈ ₹29.26 Cr ≈ $3.25M** collected over 5 years (after adding back ₹6.35 Cr opex).

The 9-account list at a 100% win rate collects $3.21M — **$40k short** of that. So the target is not out of reach by a wide margin on paper; it is unreachable in practice because it requires *every* account to close on schedule.

Sizing the pipeline instead: at a 35–45% win rate, the list yields only ~$1.1–1.4M cash. Hitting $3.25M needs **2.25–2.9x the current list's cash-generating capacity — roughly 20–26 similarly-sized, similarly-timed accounts in active pipeline** (vs. 9 named today), on top of the SOC 2 audit and support-engineer hire being funded and operational well before Year 3. (Supersedes the earlier "18–20 concurrent contracts at ~$300k blended ACV" figure, which mixed a run-rate ACV with 5-year cumulative cash and did not reconcile with the tax/opex math.)

**Conclusion:** as scoped to this exact 9-firm list, the ₹17.87 Cr target is reached only at a 100% close rate with no timing slippage (₹17.6 Cr, 98%). At realistic win rates it requires either (a) expanding the target list to ~20–26 similarly-sized accounts, or (b) recutting the target itself.

---

## 4. Single Biggest Point of Failure Before Year 3

**Zero reference customers.**

Every one of these 9 firms runs security/code review before any third-party component touches a production trading path. No security team at AlphaGrep, Tower, Wintermute, or Jump approves a vendor with no SOC 2 report, no existing paying enterprise logo, and a single-founder support structure — regardless of benchmark quality (sub-20ns fast path, invariant-TSC cycle counters, zero heap allocations all included).

This is not a pricing problem and not primarily a sales-cycle-length problem. It is a **trust-artifact problem that blocks the sales cycle from ever starting the clock.** Outbound to all 5 fastest-moving domestic accounts only went out in the 48 hours preceding this audit, with zero prior relationship in any case. Without landing one reference-able logo — even a small one, even at a discount — every subsequent deal in this pipeline stalls in evaluation indefinitely, and the Year 3+ dates in the schedule above do not occur at all.

---

*This audit supersedes the illustrative "Five-Year Capital Trajectory" forecast panel added to `docs/financial_architecture/index.html` for the purpose of GTM/revenue planning — that panel's enterprise-deal defaults (2 Desk License + 1 Enterprise SLA + 1 OEM-Redistribution) should be treated as placeholders, not as validated targets, until at least one signed contract exists.*
