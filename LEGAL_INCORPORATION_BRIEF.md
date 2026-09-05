# Animus — Corporate Incorporation Brief & CA/CS Engagement Docket

**Classification:** Internal Corporate Formation Instruction — For Direct Handoff to Engaged Chartered Accountant (CA) / Company Secretary (CS)
**Audience:** The practicing CA/CS firm retained to execute incorporation, and the Promoter/Founder reviewing the mandate before engagement.
**Companion documents:** [`COMPLIANCE_AND_RISK_MITIGATION.md`](COMPLIANCE_AND_RISK_MITIGATION.md) (cross-border tax/RBI pipeline this brief's Section 5 hands off into) · [`LEGAL_EULA.md`](LEGAL_EULA.md) and [`COMMERCIAL.md`](COMMERCIAL.md) (customer-facing license terms whose "Licensing entity" field this incorporation resolves) · [`docs/LEGAL_VERIFICATION_AUDIT.md`](docs/LEGAL_VERIFICATION_AUDIT.md) (the open-items audit that flagged "sole proprietorship vs. incorporated entity" as the single most important unresolved fact in this repository's legal documents — this brief is the resolution of that item).

---

> ## ⚠️ READ BEFORE ACTING
> This document is a **founder-authored engagement brief and instruction docket**, not a filed statutory instrument, and not legal, tax, or accounting advice in itself. It exists to give the engaged CA/CS everything needed to scope, quote, and execute incorporation without a discovery round-trip. All bracketed `[placeholder]` fields require promoter-supplied facts before any form is filed. The engaged CA/CS is expected to:
> - Verify every statutory reference (form numbers, NIC codes, RBI purpose codes, MCA fee schedules, stamp duty rates) against the **currently applicable** rules at the time of filing — company-law and FEMA procedure both change on a rolling basis, and nothing below should be treated as frozen.
> - Independently confirm the proposed main-objects language in Section 2 against the MCA object-clause matching tool used in SPICe+ Part A, and revise wording as needed to pass automated scrutiny.
> - Flag to the Promoter, before filing, any structural choice below (capital structure, NOC form, purpose-code selection) that the CA/CS's professional judgment would set differently.

---

## 1. Executive Summary & Corporate Profile

| Field | Detail |
|---|---|
| **Proposed entity name (primary)** | Animus Technologies Private Limited |
| **Proposed entity name (secondary/alternate)** | Animus Core Systems Private Limited |
| **Entity type** | Private Limited Company, incorporated under the Companies Act, 2013 |
| **Promoter / Founder** | Alakshendra Roy |
| **Jurisdiction of incorporation** | India (Registrar of Companies to be confirmed per registered-office state — Section 4) |
| **Primary business activity** | Systems software architecture and engineering — specifically, the design, development, and commercial licensing of low-latency inter-process communication (IPC) and telemetry-processing engines (C++/Python), and cross-border enterprise software licensing and technical support services built on that engine |
| **Commercial operations** | Export of standardized software production licenses (tiered enterprise entitlement — see `COMMERCIAL.md`) and associated technical support/engineering agreements to institutional counterparties — proprietary trading firms, quantitative hedge funds, market makers, and autonomous robotics platform developers — in the United States, European Union, and Asia-Pacific |
| **NIC Code classification** | Section **J** — Information and Communication; Division **62** — Computer Programming, Consultancy and Related Activities; Group **620**; Class/Sub-class **6201** — Computer Programming Activities (primary), with **6209** — Other Information Technology and Computer Service Activities as a secondary code covering the technical-support/consultancy component. *(Final sub-class selection to be confirmed by the CA/CS against the current NIC 2008 codebook and the MCA CIN-allotment activity list at filing time.)* |
| **Registered office state** | `[STATE — to be confirmed by Promoter; determines jurisdictional RoC, stamp duty, and Professional Tax applicability]` |

### 1.1 Why incorporation now

Animus currently operates as a sole proprietorship under Alakshendra Roy (per the existing `LEGAL_EULA.md` signature block and the licensing-entity field in `COMMERCIAL.md` §4.4). This incorporation resolves the single largest open item flagged in `docs/LEGAL_VERIFICATION_AUDIT.md` §1 — "sole proprietorship vs. incorporated entity" — which currently blocks a clean W-8BEN-E determination for US customers and leaves jurisdiction/governing-law fields blank across `LEGAL_EULA.md` and related pilot templates. Once the Certificate of Incorporation issues, those companion documents require a conforming amendment pass (entity name, CIN, registered office, governing-law jurisdiction) — tracked as a follow-on task, out of scope for this brief.

---

## 2. Statutory Filing Scope (SPICe+ Integrated Filing Mandate)

This is the complete turnkey mandate requested of the CA/CS. Each step below is sequenced; do not begin a later step before the prior step's output (name approval, DSC, DIN) is in hand.

### Step 1 — Name Reservation (RUN / SPICe+ Part A)

- File SPICe+ Part A (or RUN, at the CA/CS's discretion based on current MCA portal guidance) with the two proposed names above, in order of preference.
- Confirm trademark-class and existing-company-name clearance for both names before filing, to avoid a resubmission cycle.
- Output: Name reservation approval (valid 20 days from approval for a fresh incorporation, per current MCA rules — confirm validity window at filing time).

### Step 2 — Digital Signature Certificates (Class 3 DSC)

- Procure **Class 3 DSC** for each proposed Director (minimum two Directors required for a Private Limited Company under the Companies Act, 2013).
- Required from each Director: PAN, Aadhaar, a passport-size photograph, a live mobile number and email for OTP-based e-KYC, and video verification per the current Certifying Authority process.
- Output: DSC token/USB dongle (or cloud-based signing credential, per the issuing Certifying Authority's current offering) for each Director, needed to sign all subsequent e-forms.

### Step 3 — Drafting Memorandum of Association (MOA) & Articles of Association (AOA)

Draft the MOA's main-objects clause (Object Clause 3(1)(a)) and the AOA to cover, at minimum, the following scope — language below is a drafting starting point for the CA/CS to conform to the MCA's object-clause matching tool:

> **Main Objects (drafting basis for CA/CS conformance):**
>
> 1. To carry on in India and abroad the business of designing, developing, testing, licensing, and commercially distributing proprietary and dual-licensed (community and enterprise) computer software, including without limitation low-latency inter-process communication (IPC) engines, telemetry ingestion and event-processing systems, and allied systems software, for deployment in financial trading, autonomous robotics, and other latency-sensitive computing environments.
> 2. To provide technical support, consultancy, systems integration, benchmarking, and custom engineering services in relation to the software and systems referred to in clause (1) above, to customers in India and abroad, whether under a standard support agreement, an enterprise service-level agreement, or a bespoke engineering engagement.
> 3. To export computer software, computer software licenses (including single-node, institutional-scale, OEM/embedded, and global enterprise master license structures), and associated technical support and maintenance services to persons resident outside India, in accordance with the Foreign Exchange Management Act, 1999, and the rules, regulations, and directions made or issued thereunder from time to time.
>
> *(The CA/CS should run this language through the SPICe+ Part A object-clause selection/matching step and adjust wording to the closest matching approved objects, or file as free-text where the portal permits, per current MCA practice.)*

- AOA: Table F (Schedule I to the Companies Act, 2013) as the base template, with standard private-company restrictions on share transfer and a cap on the number of members (per Section 2(68)).

### Step 4 — SPICe+ Part B Integrated Filing

File SPICe+ Part B as a single integrated application covering:

| Component | Detail |
|---|---|
| **Director Identification Number (DIN)** | Allotment for both proposed Directors (up to 3 DINs can be applied for through SPICe+ for first-time directors) |
| **Corporate PAN** | Auto-generated through the SPICe+ integrated filing |
| **Corporate TAN** | Auto-generated through the SPICe+ integrated filing |
| **AGILE-PRO-S registrations** | Filed as part of the same integrated form: **EPFO** registration, **ESIC** registration, **Professional Tax** registration (state-dependent — applicable per the registered-office state selected in Section 1), and the **integrated corporate bank current account** opening application with the CA/CS's empanelled or the Promoter's preferred bank |

### Step 5 — Post-Incorporation Compliance

Sequenced immediately after the Certificate of Incorporation issues:

1. **Form INC-20A** — Declaration of Commencement of Business, filed within the statutory window from incorporation (confirm current deadline — 180 days as of the last Companies Act amendment cycle, subject to change) after the subscribed capital has actually been received into the company's bank account.
2. **GST Registration** — under the export-of-services classification, given the company's declared business of exporting software licenses and technical support to non-resident customers.
3. **Letter of Undertaking (LUT)** — filed on the GST portal under Rule 96A of the CGST Rules, 2017, immediately upon GST registration, to enable zero-rated export invoicing without the upfront-IGST-then-refund cycle (this operationalizes the same LUT requirement already documented at the individual/proprietorship level in `COMPLIANCE_AND_RISK_MITIGATION.md` §2.3(1) — the LUT must be re-filed under the new corporate GSTIN; the proprietorship's LUT does not carry over).
4. **DGFT Import Export Code (IEC)** — applied for in the company's name (a fresh IEC is required; an IEC previously held by the proprietorship, if any, does not transfer to the new corporate entity).

---

## 3. Shareholding & Capital Structure

| Item | Detail |
|---|---|
| **Authorized share capital** | ₹1,00,000 (Rupees One Lakh), divided into 10,000 equity shares of ₹10 each |
| **Subscribed / paid-up capital at incorporation** | ₹10,000 to ₹1,00,000 — exact figure **as advised by the CA/CS** based on the current bank's minimum initial funding requirement for corporate current-account activation and the Promoter's preference on upfront capital lock-in |
| **Face value per share** | ₹10 |

### 3.1 Proposed Equity Split

| Shareholder | Role | Equity % | Shares (at full ₹1,00,000 paid-up) | Amount |
|---|---|---|---|---|
| Alakshendra Roy | Founder, Director | 99.0% | 9,900 | ₹99,000 |
| `[SECOND DIRECTOR / NOMINEE NAME]` | Second Director / Nominee Shareholder | 1.0% | 100 | ₹1,000 |
| **Total** | | **100%** | **10,000** | **₹1,00,000** |

**Note for CA/CS:** A Private Limited Company requires a minimum of two Directors and two Members (shareholders) under the Companies Act, 2013 — the second shareholder above cannot be omitted. If the Promoter has not yet identified a natural second Director/shareholder, flag this as a blocking item before Step 2 (DSC procurement) proceeds, since the second Director's DSC and DIN are on the incorporation-day critical path.

---

## 4. Registered Office & Compliance Documentation

### 4.1 No Objection Certificate (NOC) for Registered Office — Ready-to-Print Templates

**Template A — Residential Premises**

```
NO OBJECTION CERTIFICATE (NOC) FOR USE OF PREMISES AS REGISTERED OFFICE

To,
The Registrar of Companies,
[Jurisdictional RoC / State]

Subject: No Objection Certificate for use of premises as Registered Office of
         [Company Name] Private Limited

I/We, [Owner's Full Name], son/daughter of [Father's Name], residing at
[Complete Address of Owner], being the absolute and lawful owner(s) of the
premises situated at [Complete Property Address, including PIN code]
("the Premises"), do hereby state and confirm as follows:

1. That I/We am/are the sole and lawful owner(s) of the Premises described
   above.

2. That I/We have no objection to [Company Name] Private Limited, a company
   incorporated / proposed to be incorporated under the Companies Act, 2013,
   using the Premises as its Registered Office for the purpose of carrying
   on its business and for compliance with statutory and regulatory
   requirements under the Companies Act, 2013 and the rules made thereunder.

3. That I/We shall permit the said company to use the Premises as its
   Registered Office for so long as [it remains in lawful occupation of the
   Premises / until this NOC is revoked in writing by me/us with reasonable
   prior notice].

4. That this NOC is issued voluntarily and I/We shall have no objection to
   the Registrar of Companies, GST authorities, or any other statutory
   authority corresponding with the company at this address.

I/We confirm that the Premises is lawfully held by me/us and there is no
legal impediment to its use as the Registered Office of the said company.

Signed: _________________________
Name of Owner: ___________________
Relationship to Director/Promoter (if any): ___________________
Address: _________________________
Date: __________________  Place: __________________

Attachments required: copy of ownership proof (sale deed / latest property
tax receipt) and a self-attested photo ID of the owner.
```

**Template B — Commercial / Managed / Virtual Office**

```
NO OBJECTION CERTIFICATE (NOC) FOR USE OF MANAGED/VIRTUAL OFFICE PREMISES
AS REGISTERED OFFICE

To,
The Registrar of Companies,
[Jurisdictional RoC / State]

Subject: No Objection Certificate for use of premises as Registered Office of
         [Company Name] Private Limited

We, [Managed/Virtual Office Provider — Legal Entity Name], having our
registered/principal office at [Provider's Address], being the lawful
licensor/operator of the premises situated at [Complete Serviced-Office
Address, including PIN code] ("the Premises") under [reference: Service
Agreement / License Agreement No. ___ dated ___] with [Company Name]
Private Limited ("the Licensee"), do hereby confirm as follows:

1. That we are duly authorized to grant use of the Premises for registered-
   office purposes to our licensees under the terms of our standard service
   agreement.

2. That we have no objection to the Licensee using the Premises as its
   Registered Office for compliance with the Companies Act, 2013, for so
   long as the underlying service/license agreement between us remains in
   force.

3. That statutory correspondence addressed to the Licensee at this address
   will be accepted and made available to the Licensee in the ordinary
   course of our mail-handling service.

Signed: _________________________
Name & Designation: ___________________
For and on behalf of: [Provider Legal Entity Name]
Date: __________________  Place: __________________

Attachments required: copy of the executed service/license agreement, the
provider's own property-ownership or lease proof, and a utility bill for
the Premises not older than two months.
```

> **CA/CS note:** Most managed/virtual office providers issue their own pre-approved NOC on their letterhead — Template B above states the minimum clauses that document must contain; use the provider's own template where one exists rather than substituting this draft, to avoid a mismatch with their internal compliance process.

### 4.2 First Board Resolution — Authorized Signatory Designation (Draft)

```
CERTIFIED TRUE COPY OF A RESOLUTION PASSED AT THE FIRST MEETING OF THE
BOARD OF DIRECTORS OF [COMPANY NAME] PRIVATE LIMITED (CIN: [CIN]) HELD ON
[DATE] AT [TIME] AT THE REGISTERED OFFICE OF THE COMPANY AT [REGISTERED
OFFICE ADDRESS]

Present:
1. [Director 1 Name] — Director, DIN: [DIN]
2. [Director 2 Name] — Director, DIN: [DIN]

AUTHORIZED SIGNATORY FOR BANKING AND STATUTORY FILINGS

"RESOLVED THAT pursuant to the applicable provisions of the Companies Act,
2013, [Director Name], Director of the Company, be and is hereby
authorized, singly / jointly with [Name], to act as the Authorized
Signatory of the Company for the following purposes:

  (a) opening, operating, and closing bank account(s) in the name of the
      Company with [Bank Name], and signing cheques, deposit instruments,
      and related banking documents;

  (b) making applications to, and corresponding with, the Registrar of
      Companies, the Goods and Services Tax authorities, the Reserve Bank
      of India / the Company's Authorized Dealer bank, the Directorate
      General of Foreign Trade, and the Income Tax Department, for and on
      behalf of the Company;

  (c) execution and filing of the Declaration for Commencement of Business
      under Section 10A of the Companies Act, 2013 (Form INC-20A);

  (d) signing and filing of the GST registration application, the Letter
      of Undertaking (LUT) under Rule 96A of the CGST Rules, 2017, and the
      Import Export Code (IEC) application with the DGFT;

  (e) doing all such further acts, deeds, and things as may be necessary
      or incidental to give effect to this resolution."

"RESOLVED FURTHER THAT a certified true copy of this resolution be issued
to any bank, authority, or third party requiring evidence of the above
authorization."

For [Company Name] Private Limited


_________________________
[Director Name]
Director
DIN: [DIN]
```

### 4.3 Document Checklist — Promoter & Director KYC

| Category | Document | Notes |
|---|---|---|
| Identity | PAN Card | Mandatory for every Director/promoter (foreign nationals: passport in lieu of PAN, per current MCA rules) |
| Identity | Aadhaar Card | For Indian-resident Directors |
| Identity | Passport | Mandatory for any foreign-national Director |
| Identity | Passport-size photograph | Recent, for each Director |
| Address proof | Bank statement, electricity bill, mobile/telephone bill | Not older than 2 months, for each Director |
| Registered office proof | Ownership deed / rent or leave-and-license agreement | Per Section 4.1 |
| Registered office proof | Utility bill (electricity/water/gas) for the Premises | Not older than 2 months |
| Registered office proof | Executed NOC (Section 4.1, Template A or B) | Original signed copy |
| Signatures | DSC enrollment — live mobile/email for OTP, video KYC | Per Step 2, Section 2 |
| Banking | Specimen signature, board resolution (Section 4.2) | For account opening |
| Capital | Proof of subscribed-capital remittance into the company's bank account | Required before Form INC-20A (Section 2, Step 5.1) can be filed |

---

## 5. Foreign Exchange & Cross-Border Tax Protocol — CA Directives

The following directives extend the cross-border tax and RBI export-compliance pipeline already documented at the proprietorship level in `COMPLIANCE_AND_RISK_MITIGATION.md` §2 to the newly incorporated entity. The CA/CS engaged for incorporation should hand these off to (or coordinate directly with) whoever manages ongoing FEMA/RBI and DTAA compliance post-incorporation.

### 5.1 FIRC / e-FIRC Maintenance

- Every inward remittance received from a non-resident customer against a software license or support invoice must have a corresponding **Foreign Inward Remittance Certificate (FIRC)**, or its electronic equivalent (**e-FIRC**), issued by the company's Authorized Dealer (AD) bank.
- The e-FIRC must be reconciled against the underlying SOFTEX filing and invoice (per the existing pipeline in `COMPLIANCE_AND_RISK_MITIGATION.md` §2.3, steps 2–4) before that invoice is treated as realized in the RBI's Export Data Processing and Monitoring System (EDPMS).
- Retain e-FIRCs for no less than the record-retention period already established for the issuance ledger in `COMPLIANCE_AND_RISK_MITIGATION.md` §3.3 (term of the applicable agreement plus three years), as the corporate compliance baseline going forward.

### 5.2 RBI Purpose Code Classification

- Cross-border software-export remittances are to be classified under **Purpose Code P0802** (software consultancy/implementation-type receipts) or **Purpose Code P0807** (computer software export receipts), as applicable to the specific transaction's characterization (standardized license fee vs. an engineering/consultancy-scoped receipt — see the License Fee vs. services distinction already drawn in `COMPLIANCE_AND_RISK_MITIGATION.md` §2.1).
- **CA/CS action item:** confirm the exact applicable purpose code against the AD bank's current RBI Master Direction on reporting under FEMA at the time each remittance is reported — purpose codes are amended periodically, and the AD bank's own reporting system is the authoritative source at the point of filing, not this document.

### 5.3 Cross-Border Double Tax Treaty (DTAA) Documentation

- **Entity-form correction:** incorporation resolves the open item flagged in `docs/LEGAL_VERIFICATION_AUDIT.md` §1 — US customer withholding-tax documentation should now be collected/issued on **Form W-8BEN-E** (entity), not Form W-8BEN (individual), since payments will flow to the incorporated company rather than to Alakshendra Roy in an individual/sole-proprietor capacity. Any US customer relationship carried over from the proprietorship needs a fresh W-8BEN-E on file before the first post-incorporation invoice.
- **Tax Residency Certificate (TRC):** obtain the company's own TRC from the Indian tax authority annually, in the corporate entity's name, once the corporate PAN is issued (Section 2, Step 4) — the Promoter's individual TRC, if any existed, does not carry over to the company.
- **Form 10F:** file the self-declaration supplementing the TRC, per the same annual cadence described in `COMPLIANCE_AND_RISK_MITIGATION.md` §2.2, under the corporate PAN.
- **EU counterparty equivalents:** per-country certificate/self-declaration requirements under the applicable bilateral India–[Member State] DTAA, to be re-issued under the corporate entity per the same table in `COMPLIANCE_AND_RISK_MITIGATION.md` §2.2.

### 5.4 Downstream Consistency Follow-Up (Not in This Filing's Scope, Flagged for the Record)

Once the Certificate of Incorporation, CIN, and registered office are final, the following companion documents carry blank or proprietorship-scoped fields that require a conforming update — tracked here so it isn't lost between the CA/CS engagement and the engineering/legal-docs team:

- `LEGAL_EULA.md` — "Vendor" party block, governing-law/venue fields (currently blank per `docs/LEGAL_VERIFICATION_AUDIT.md` §2), and the Signature Block's notice address.
- `COMMERCIAL.md` §4.4 — "Licensing entity" contact-table row, currently "Alakshendra Roy, Founder — India."
- Any pilot/contract templates in `docs/` still carrying `[entity type / jurisdiction of organization — to be confirmed]` placeholders, including reconciling `docs/PILOT_CONTRACT.md`'s existing Delaware governing-law clause against the India-incorporation decision made here — that inconsistency was flagged, not resolved, by `docs/LEGAL_VERIFICATION_AUDIT.md` §2, and this brief's India-Private-Limited structure is the fact that finally resolves which way it should go.

---

## 6. Engagement Scope Summary for CA/CS Quotation

| Item | In scope for this engagement |
|---|---|
| Name reservation (Section 2, Step 1) | Yes |
| DSC procurement, both Directors (Step 2) | Yes |
| MOA/AOA drafting (Step 3) | Yes |
| SPICe+ Part B, DIN/PAN/TAN/AGILE-PRO-S (Step 4) | Yes |
| Form INC-20A (Step 5.1) | Yes |
| GST registration + LUT filing (Step 5.2–5.3) | Yes |
| DGFT IEC application (Step 5.4) | Yes |
| Registered-office NOC review (Section 4.1) | Yes — review/tailor the templates above to the actual premises |
| First Board Resolution drafting (Section 4.2) | Yes — finalize post-incorporation with actual CIN/DIN/bank name |
| Ongoing FEMA/RBI/DTAA compliance pipeline (Section 5) | Handoff only — scope as a separate retainer per `COMPLIANCE_AND_RISK_MITIGATION.md` §2's standing CA/FEMA retainer model |
| Companion legal-document conforming updates (Section 5.4) | Out of scope — internal follow-up task |

---

*Prepared for direct handoff to the engaged CA/CS. Populate every `[placeholder]` before filing, and route any professional-judgment deviation from this brief back to the Promoter before the affected step is filed.*
