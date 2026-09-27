# AnimusCore — 5-Year Financial Audit (₹17.87 Cr Target)

**Prepared:** 2026-09-25
**Scope:** Institutional CFO / enterprise HFT infrastructure sales audit of the ₹17.87 Cr, 5-year liquid net worth target modeled in `docs/financial_architecture/index.html`, re-tested against realistic enterprise sales-cycle dynamics for the named target account list.
**Nature of this document:** Informed industry-judgment modeling (deal-cycle norms, enterprise pricing structures, HFT security-review practices), not verified facts about the internal budgets, procurement policies, or vendor-adoption plans of any named firm. Treat all figures as planning estimates, not disclosed or confirmed data from those firms.

---

## Executive Verdict

The ₹17.87 Cr target is **not an enterprise-sales forecast — it's a personal-lifestyle-tier assumption wearing a business plan's clothes.** ₹15.70 Cr of it (88%) comes from the founder-draw waterfall in `financial_architecture/index.html`, which assumes $20k/mo of business revenue exists *today*, scaling to $120k/mo by Year 5 — with **zero contracts** behind that figure. The remaining ~₹4.32 Cr of "enterprise deals" layered on top in the prior session's five-year forecast panel were illustrative placeholders, not actual pipeline.

Re-running the numbers as a genuine B2B enterprise infrastructure sale into the actual named 9-account target list, with real sales-cycle physics (evaluation periods, security/code audits, sandboxed backpressure testing, net-60/90 payment terms) produces a materially different picture:

> **Even at a 100% win rate on every single named account, on an aggressive-but-plausible timeline, the ₹17.87 Cr target is not reached within 5 years.** Best case lands around **₹13.2 Cr**. A realistic (still optimistic) win rate lands around **₹4–7 Cr** — 25–40% of the stated target.

---

## 1. Tier Pricing — Core License vs. Annual Production Colocation Retainer

| Segment | Accounts | Core License (Annual) | Annual Production Colo Retainer | Total ACV |
|---|---|---|---|---|
| Domestic Prop Desk | AlphaGrep, Graviton, iRage, Quadeye | $45,000 | $15,000 (33%) | **$60,000** |
| Domestic Enterprise | Tower Research (India engineering site) | $180,000 | $60,000 (33%) | **$240,000** |
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
| Y4 Q1 | Wintermute signs | $480k | $120k | $60k |
| Y4 Q2 | — | $1,030k | $257.5k | $120k |
| Y4 Q3 | Flow Traders signs | $1,030k | $257.5k | $257.5k |
| Y4 Q4 | — | $1,580k | $395k | $257.5k |
| Y5 Q1 | Jump Trading signs | $1,580k | $395k | $395k |
| Y5 Q2 | — | $2,130k | $532.5k | $395k |
| Y5 Q3 | Jane Street signs | $2,130k | $532.5k | $532.5k |
| Y5 Q4 | — | $2,680k | $670k | $532.5k |

### Annual Rollup

| Year | Revenue Recognized | Cash Collected |
|---|---|---|
| Y1 | $0 | $0 |
| Y2 | $90k | $60k |
| Y3 | $210k | $180k |
| Y4 | $1,030k | $695k |
| Y5 | $2,130k | $1,855k |
| **5-Yr Total** | **$3.46M** | **$2.79M** |

Note the $670k gap between the two totals: Q20's revenue has not converted to cash by the end of Year 5 — it is still in net-60/90 transit into Year 6. **Revenue recognized and cash in hand are not the same number, and only cash compounds.**

$2.79M cash collected @ ₹90/$1 = **₹25.11 Cr gross, best case, 100% win rate on every named account.**

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

Applying 22% corporate tax to the ₹25.11 Cr gross (best case) and subtracting the unmodeled opex above:

```
₹25.11 Cr  (gross cash collected, 100% win rate)
− ₹5.5  Cr (22% corporate tax)
− ₹6.35 Cr (unmodeled opex: engineer, colo, SOC 2)
─────────────────────────────────────────────────
≈ ₹13.2 Cr  realistic BEST-CASE retained treasury
```

Against the ₹17.87 Cr target, **this is the ceiling** — the outcome only if every one of the 9 named accounts closes, with zero churn, on an aggressive timeline.

Applying a realistic (still optimistic, given zero reference customers) **35–45% weighted win rate** instead of 100% collapses this to **roughly ₹4–7 Cr** — 25–40% of the stated target.

### Exact Contract Count Required to Actually Hit ₹17.87 Cr

Working backward: hitting the cash-collected equivalent of ₹17.87 Cr (after the same tax/opex drag, this requires roughly **$5.5–6M gross cash collected**) at a blended realistic ACV (~$300k across the tier mix) requires approximately **18–20 concurrent, fully-active, non-churned enterprise contracts by Year 5** — more than **double the entire current 9-account named target list**, all closed, all retained, on top of the SOC 2 audit and support-engineer hire being funded and operational well before Year 3.

**Conclusion:** as scoped to this exact 9-firm list, the ₹17.87 Cr target is not reachable in 5 years even at a 100% close rate. Reaching it requires either (a) expanding the target account list to 18–20+ similarly-sized firms, or (b) recutting the target itself.

---

## 4. Single Biggest Point of Failure Before Year 3

**Zero reference customers.**

Every one of these 9 firms runs security/code review before any third-party component touches a production trading path. No security team at AlphaGrep, Tower, Wintermute, or Jump approves a vendor with no SOC 2 report, no existing paying enterprise logo, and a single-founder support structure — regardless of benchmark quality (sub-20ns fast path, invariant-TSC cycle counters, zero heap allocations all included).

This is not a pricing problem and not primarily a sales-cycle-length problem. It is a **trust-artifact problem that blocks the sales cycle from ever starting the clock.** Outbound to all 5 fastest-moving domestic accounts only went out in the 48 hours preceding this audit, with zero prior relationship in any case. Without landing one reference-able logo — even a small one, even at a discount — every subsequent deal in this pipeline stalls in evaluation indefinitely, and the Year 3+ dates in the schedule above do not occur at all.

---

*This audit supersedes the illustrative "Five-Year Capital Trajectory" forecast panel added to `docs/financial_architecture/index.html` for the purpose of GTM/revenue planning — that panel's enterprise-deal defaults (2 Desk License + 1 Enterprise SLA + 1 OEM-Redistribution) should be treated as placeholders, not as validated targets, until at least one signed contract exists.*
