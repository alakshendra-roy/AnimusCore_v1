# CA Monthly Accounting & Compliance Retainer — SOP

**Classification:** Founder-Drafted Operational SOP — Not Tax or Legal Advice
**Audience:** Alakshendra Roy (Promoter/Director) and the retained online CA firm.
**Companion documents:** [`CORPORATE_INCORPORATION_CHECKLIST.md`](CORPORATE_INCORPORATION_CHECKLIST.md) / [`CORPORATE_SETUP_PACKAGE.md`](CORPORATE_SETUP_PACKAGE.md) (the entity this SOP assumes is already incorporated, GST-registered, and LUT-filed) · [`CORPORATE_FLEET_&_EXPENSE_STRUCTURE.md`](CORPORATE_FLEET_%26_EXPENSE_STRUCTURE.md) (the source of the `/Car_Lease_Fuel/` paper trail referenced in §2) · [`../../COMPLIANCE_AND_RISK_MITIGATION.md`](../../COMPLIANCE_AND_RISK_MITIGATION.md) (cross-border invoicing/FEMA compliance this SOP's bookkeeping must stay consistent with).

---

> ## ⚠️ READ FIRST
> **This is an operational checklist for running a CA retainer, not a substitute for one.** Every due date, rate, and section number below should be confirmed against the CBDT's official calendar and the finalized Income-tax Rules, 2026 text for the specific assessment year in play before being relied on — see the note on the Income-tax Act, 2025 transition in [`CORPORATE_FLEET_&_EXPENSE_STRUCTURE.md`](CORPORATE_FLEET_%26_EXPENSE_STRUCTURE.md)'s banner, which applies equally here. Dates below are the long-standing structural pattern (advance-tax installment dates, TDS due-date cadence) that has been stable across recent years; they are not guaranteed unchanged for every future year and should be re-confirmed at the start of each financial year, not assumed permanent.

---

## 1. Standard Monthly Deliverables

| # | Deliverable | Who does what |
|---|---|---|
| 1 | **Bookkeeping entry** — all invoices raised, all expenses incurred, categorized against the chart of accounts | Founder uploads source documents to the Drive structure (§2); CA (or CA's bookkeeping staff) enters them into the accounting system |
| 2 | **Bank reconciliation** | CA reconciles the month's bank statement (`/Bank_Statements/`) against booked entries; founder resolves any unexplained line item flagged back |
| 3 | **Invoice/receivables reconciliation** | CA confirms each `/Invoices/` entry has either been paid (matched to a bank credit / e-FIRC per `CORPORATE_SETUP_PACKAGE.md` Artifact 4) or is correctly aged as outstanding |
| 4 | **TDS working** | CA computes TDS liability on the month's payments requiring deduction (contractor/professional payments, rent above threshold, etc.) and prepares the challan for the founder to pay by the due date (§4) |
| 5 | **GST return prep** | CA prepares GSTR-1 (outward supplies) and GSTR-3B (summary return + tax payment) for the month/quarter, reflecting zero-rated export invoices under the LUT on file |
| 6 | **Monthly MIS snapshot** | CA provides a one-page P&L/cash-position snapshot for the month — not a statutory requirement, but the actual reason a monthly (not just annual) retainer is worth paying for |

## 2. Google Drive Folder Structure

```
/Animus [Entity Name] — Accounts/
│
├── /Invoices/
│   └── /FY2026-27/
│       ├── /04-April/
│       ├── /05-May/
│       └── ... (one subfolder per month)
│
├── /Server_Cloud_OPEX/
│   └── /FY2026-27/
│       └── /<month>/         — hosting, domain, SaaS tooling, compute invoices
│
├── /Car_Lease_Fuel/
│   └── /FY2026-27/
│       └── /<month>/         — lease invoice, XTRAPOWER statement, any
│                                maintenance/insurance receipt — see
│                                CORPORATE_FLEET_&_EXPENSE_STRUCTURE.md §2–3
│                                for why this paper trail matters beyond
│                                bookkeeping (it's the evidentiary basis for
│                                both the Company's OPEX deduction and the
│                                Director's Rule 3(2) perquisite figure)
│
└── /Bank_Statements/
    └── /FY2026-27/
        └── /<month>/         — corporate current account statement,
                                 downloaded directly from net banking, not
                                 forwarded/retyped
```

**Naming convention:** `YYYY-MM-DD_<counterparty-or-description>_<amount>.pdf` for every file (e.g. `2026-10-05_XTRAPOWER-Statement_INR-14200.pdf`) — consistent naming is what lets the CA's own reconciliation tooling (and a future audit) machine-match filenames to ledger entries without opening each one. Upload as the expense is incurred, not batched at month-end — a same-day upload habit is what actually keeps the monthly close fast; a folder that fills up in the last three days of the month defeats the point of a monthly (vs. annual) retainer.

## 3. Quarterly Advance Tax

A company must pay advance tax in four installments through the financial year if total tax liability for the year exceeds ₹10,000. The long-standing cumulative-percentage schedule:

| Installment due by | Cumulative % of estimated annual tax liability |
|---|---|
| 15 June | 15% |
| 15 September | 45% |
| 15 December | 75% |
| 15 March | 100% |

The CA should recompute the estimated annual liability each quarter based on actual year-to-date results (not just a flat 1/4 split of a stale annual estimate), since a first-year company's revenue ramp is unlikely to be linear across the four quarters — confirm this recompute happens each quarter as part of the retainer, not only once at the start of the year.

## 4. TDS Filings

| Cadence | Action | Due date pattern |
|---|---|---|
| Monthly | Deposit TDS deducted during the month | 7th of the following month (30 April for March-deducted TDS — the one exception to the "7th" pattern) |
| Quarterly | File TDS return (Form 24Q/26Q as applicable) | 31 July / 31 October / 31 January / 31 May (Q1–Q4 respectively) |
| Annual | Issue Form 16/16A to deductees | Following the applicable quarterly filing, per the standard post-filing issuance window |

Confirm current applicability thresholds (which payment categories trigger a TDS obligation at all, and at what rate) each year with the CA — these are exactly the kind of figures that move with each Finance Act and are not safe to carry forward from a prior year's SOP without re-confirmation.

## 5. Annual Corporate Compliance Calendar

| Item | What | Notes |
|---|---|---|
| **Concessional tax rate election** | Section 115BAA (old-Act numbering — confirm 2025-Act section with CA) — 22% base rate, ~25.17% effective with surcharge + cess, in exchange for foregoing specified deductions/exemptions (§10AA, §32AD, §35AD, §80-IA, etc. under old-Act numbering) | Exercised via **Form 10-IC**, filed by the due date for filing the return of income for the first year it's claimed; **once exercised, it is irrevocable** for that company. Decide deliberately, not by default, and confirm the 2025-Act's equivalent form/section before relying on the old citation. |
| **ROC annual filing — financial statements** | Form **AOC-4** | Filed within 30 days of the AGM (or the applicable OPC-specific timeline — an OPC is not required to hold an AGM the way a Private Limited company is; confirm the OPC-specific filing deadline directly rather than assuming the standard AGM-linked one, per `CORPORATE_INCORPORATION_CHECKLIST.md` §1's OPC-specific mechanics). |
| **ROC annual filing — annual return** | Form **MGT-7A** (small company/OPC variant of MGT-7) | Filed within 60 days of the AGM/applicable deadline. |
| **Income tax return** | **ITR-6** (companies not claiming §11 exemption) | Standard due date 31 October where a tax audit applies (or 30 November if transfer-pricing provisions apply); 31 July-class deadlines do not apply to companies. Confirm whether a tax audit (Section 44AB old-Act numbering) actually applies at the Company's current turnover before assuming the audit-linked (later) due date — a first-year company well under the audit threshold may face the earlier, non-audit due date instead. |
| **Tax audit applicability check** | Section 44AB (old-Act numbering) turnover thresholds | Re-check every year, not just at incorporation — crossing the threshold mid-growth is exactly the kind of change a monthly retainer should catch before year-end, not discover at filing time. |
| **GST annual return** | Form **GSTR-9** (and GSTR-9C reconciliation statement if turnover exceeds the applicable threshold) | Confirm current turnover threshold for GSTR-9C applicability with the CA. |
| **LUT renewal** | Fresh Form GST RFD-11 for the new financial year | Per `CORPORATE_INCORPORATION_CHECKLIST.md` Step 11 — **not** an amendment of the prior year's LUT; must be re-filed, ideally by 31 March preceding the FY it covers, to avoid any gap in zero-rated export invoicing. |

---

*This SOP is a drafting starting point prepared by the founder — it has not been reviewed by a CA, CS, or advocate. Every due date, rate, section number, and threshold above should be confirmed against the current, finalized Income-tax Rules/Act text and the CBDT's official calendar for the specific financial year in play before being relied on for an actual filing.*
