# Animus Core — Outbound Tracker

Working tracker for the 25-firm target list in [`ICP_TARGET_LIST.md`](ICP_TARGET_LIST.md), staged across 5 weekly batches of 5 firms each so response signal from earlier batches can inform later ones. Templates referenced by variant letter are in [`OUTREACH_TEMPLATES.md`](OUTREACH_TEMPLATES.md).

**Channel note:** Telegram is listed as a column option for completeness but is not in active use (no Telegram account currently) — every row below defaults to Email or LinkedIn InMail per firm. Update the "Contact Name" and "Outreach Channel" columns once a real contact is sourced; a firm with no findable corporate email should default to InMail regardless of what's assigned below.

**Cadence:** First Touch on the assigned date; Day-5 Bump is First Touch + 7 calendar days (~5 business days), matching the bump templates. Update Status as each firm moves — **Not Started → Sent → Replied → Scheduled PoP** — and log a real bump date once the actual first-touch date is confirmed (dates below are the planned schedule, not sent confirmations).

---

## Week 1 — Batch 1 (Tier A, first touch 2026-09-08)

| Firm Name | Tier | Recommended Target Title | Contact Name | Outreach Channel | First Touch Date | Day-5 Bump Date | Status | Technical Angle / Hook Note |
|---|---|---|---|---|---|---|---|---|
| Hudson River Trading | A | Head of Trading Technology / Platform Engineering Lead | Partial leads only — see note 1 | InMail | 2026-09-08 | 2026-09-15 | Not Started | They built their own OSS interop layer (pymetabind) — lead with respect for that work, then Variant C: "same problem, different transport underneath it" rather than a hard sell. |
| Two Sigma | A | Fast Engineering Lead | [To Be Sourced] — researched, no match found (note 2) | Email | 2026-09-08 | 2026-09-15 | Not Started | Variant B (throughput/scale) — their own "Fast Engineering" framing already splits deployment by latency sensitivity; mirror that language. |
| Point72 / Cubist | A | Head of Trading Infrastructure (Cubist) | Geoffrey Lauprete — see note 3 (business lead, not confirmed tech-title match) | Email | 2026-09-08 | 2026-09-15 | Not Started | Variant A (tail-latency) — open Low-Latency Market Data Engineer req; anchor the hook on market-data ingestion specifically. |
| Millennium (SPEED team) | A | Head of Systematic Platform Execution & Exchange Data | Alexandre Fournier — see note 4 (title unconfirmed by firm) | Email | 2026-09-08 | 2026-09-15 | Not Started | Variant C (Python interop) — their own req names Python + Apache Arrow + real-time feeds in one role; reference the Arrow/columnar handoff directly. |
| Man Group / Man AHL | A | Head of AHL Technology | Gary Collier — see note 5 (Group CTO, broader than AHL-specific) | Email | 2026-09-08 | 2026-09-15 | Not Started | Variant C — public "why Python" identity; lead with respecting their Python-first culture, "no rewrite" lands hardest here. |

### Contact sourcing notes — Batch 1

One web-research pass via public sources (conference speaker rosters, company bio pages, OSS project READMEs, press coverage). Every name below has a caveat — none is a clean "verified full name + confirmed exact title + verified email/LinkedIn" match. No email address or LinkedIn URL was fabricated or constructed; where none was found, that's stated plainly rather than guessed.

1. **Hudson River Trading** — no individual confirmed at a "Head of..." title. Two partial, lower-confidence leads instead: **"Hashem"** (surname not found), titled "Lead Software Engineer" per his [PyCon US 2026 speaker profile](https://us.pycon.org/2026/speaker/profile/159/) and the [PSF blog post on HRT's sponsorship](https://pyfound.blogspot.com/2026/05/psf-welcomes-hudson-river-trading-hrt.html) — he's delivering HRT's "Scaling Python to Saturate the Hypercube" talk, a strong content hook, but no surname or contact channel found. **Joshua Oreman** (GitHub `oremanj`) is credited in the [pymetabind README](https://github.com/hudson-trading/pymetabind) as its author "as part of his work with Hudson River Trading" — confirmed HRT-affiliated engineer, no leadership title found, no LinkedIn found. Recommend: use the PyCon talk as the opening line regardless of who ends up as the actual recipient once a name is confirmed via LinkedIn search.
2. **Two Sigma** — no named leader found for "Fast Engineering" specifically. Adjacent titles found but explicitly ruled out as non-matches: Carter Page (Head of Data Engineering — different team) and Matt Greenwood (Chief Innovation Officer & Head of Investment Management Engineering — broader remit). Still needs a manual LinkedIn search.
3. **Point72 / Cubist** — **Geoffrey Lauprete** confirmed via [Bloomberg (2025-09-10)](https://www.bloomberg.com/news/articles/2025-09-10/point72-replaces-cubist-chief-with-ex-worldquant-cio-lauprete) as the new head of Cubist Systematic Strategies (ex-WorldQuant CIO), succeeding the departed Denis Dancanet. This is business/quant leadership, not confirmed as the "Head of Trading Infrastructure" technology role specifically — a matching req, [Head of Systematic Trading Infrastructure](https://builtin.com/job/head-systematic-trading-infrastructure/7151943), appears to still be open, suggesting that specific technology role may be vacant or unannounced. No LinkedIn URL found for Lauprete. **Do not use Denis Dancanet** — confirmed departed.
4. **Millennium (SPEED team)** — **Alexandre Fournier**, per [Hedgeweek](https://www.hedgeweek.com/millennium-recruits-hft-cto-to-build-nanosecond-latency-trading-infrastructure/) and [eFinancialCareers](https://www.efinancialcareers-norway.com/news/millennium-quietly-hired-the-cto-of-a-major-hft-firm-to-work-on-nanosecond-latency-architecture), joined Millennium after serving as CTO / global head of architecture at Tower Research Capital, reported as linked to nanosecond-latency infrastructure work consistent with the SPEED team — but the source itself states "Millennium did not respond for comment," so his exact title there is unconfirmed. A possibly-matching LinkedIn (`linkedin.com/in/alexandre-fournier-026479`) surfaced by name+location match only — not independently confirmed as the same person; verify before using.
5. **Man Group / Man AHL** — **Gary Collier**, Chief Technology Officer, Man Group, confirmed via his [official bio page](https://www.man.com/gary-collier), which also states he was previously CTO of Man Alpha Technology and, before that, CTO of Man AHL specifically — strong lineage, but his *current* title is firm-wide, broader than "Head of AHL Technology." No LinkedIn URL or email found. A "Head of Data Engineering at Man AHL" name surfaced only in a 2016 case study — too dated to use without independent re-verification, not included above.

## Week 2 — Batch 2 (Tier B, first touch 2026-09-14)

| Firm Name | Tier | Recommended Target Title | Contact Name | Outreach Channel | First Touch Date | Day-5 Bump Date | Status | Technical Angle / Hook Note |
|---|---|---|---|---|---|---|---|---|
| IMC Trading | B | Head of Low-Latency Engineering | [To Be Sourced] — no match found (note 1) | InMail | 2026-09-14 | 2026-09-21 | Not Started | Variant C — dedicated Python dev track sits organizationally apart from the low-latency C++/FPGA team; hook on bridging that gap. |
| XTX Markets | B | Head of Engineering | "Craig" (surname unconfirmed) — see note 2, low confidence | InMail | 2026-09-14 | 2026-09-21 | Not Started | Variant B — Python (NumPy/PyTorch/JAX) research to C++ production split; frame around research-to-prod handoff latency. |
| Optiver | B | Head of Low-Latency Engineering | David Gross or Scott McKenzie — see note 3 (neither an exact title match) | Email | 2026-09-14 | 2026-09-21 | Not Started | Variant A — Low Latency Network Engineer req wants Python scripting bridging into FPGA/C++; FPGA-adjacent tail-latency framing. |
| Squarepoint Capital | B | Head of Platform Engineering | Benjamin Garnier-Petit — see note 4 (CTO, broader than target role) | Email | 2026-09-14 | 2026-09-21 | Not Started | Variant B — Ultra Low Latency Platform Engineer role plus a separate Python platform track; throughput framing. |
| D.E. Shaw | B | Head of Quant Systems Engineering | [To Be Sourced] — no match found (note 5) | Email | 2026-09-14 | 2026-09-21 | Not Started | Variant B — Quant Systems Developer role names Python/Golang/Rust for kernel/network latency tuning explicitly. |

### Contact sourcing notes — Batch 2

Same web-research approach and verification bar as Batch 1. Weaker overall hit rate this round — two firms returned no leadership match at all, and every match found is either a title conflict, unconfirmed surname, or an adjacent-but-broader role rather than an exact hit. No email address or LinkedIn URL was fabricated or constructed.

1. **IMC Trading** — no individual found meeting the bar. Searched job postings, conference-speaker angles, and "Teams at IMC Trading" pages; only open reqs and an unrelated Optiver speaker surfaced. Still needs a direct LinkedIn search.
2. **XTX Markets** — [The Org's XTX Markets engineering team page](https://theorg.com/org/xtx-markets/teams/engineering-and-technology) shows a "Head Of Shared Engineering" first-named **Craig**, surname truncated by the source. A LinkedIn search surfaced a **Craig Waddell** at XTX Markets, but no source directly confirms he holds this title — the name match is unverified. **Low confidence; verify directly on LinkedIn before using.**
3. **Optiver** — title conflict, not a clean lead. **David Gross**, "Options Tech Lead" per [Optiver's own technology blog](https://optiver.com/insights/technology-blog/designing-low-latency-cpp-systems/) and a CppCon speaker on low-latency C++ — real, company-sourced title, but not "Head of Low-Latency Engineering." Separately, **Scott McKenzie** is described as "Global Head of Engineering" on a (low-confidence, possibly-truncated) [LinkedIn match](https://www.linkedin.com/in/scott-m-52b47050/) but as "Chief Technology Officer, Optiver Asia Pacific" on RocketReach/AroundDeal — conflicting titles across sources, resolve before outreach.
4. **Squarepoint Capital** — no exact "Head of Platform Engineering" match. Closest is **Benjamin Garnier-Petit, Chief Technology Officer**, confirmed via [The Org](https://theorg.com/org/squarepoint-capital/teams/leadership-team-1), [LinkedIn](https://www.linkedin.com/in/benjamin-garnier-petit-15758111b/), and ZoomInfo — firm-wide CTO, broader remit than the target title, promoted internally from Quant Researcher → Deputy CTO → CTO.
5. **D.E. Shaw** — no individual found meeting the bar. [D.E. Shaw's official leadership page](https://www.deshaw.com/who-we-are/leadership) lists a full Executive Committee (Anne Dinning, Max Stone, Eddie Fishman, Alexis Halaby, Edwin Jager, Anoop Prasad, Adam Deaton), but none holds a "Quant Systems" title specifically — closest adjacent are Eddie Fishman (COO, oversees IT platforms) and Anoop Prasad (Global Head of Systematic Equities, jointly oversees trading/IT platforms), both broader executive roles. Quant Systems appears to be a large distributed team without one named public leader.

## Week 3 — Batch 3 (Tier B, first touch 2026-09-21)

| Firm Name | Tier | Recommended Target Title | Contact Name | Outreach Channel | First Touch Date | Day-5 Bump Date | Status | Technical Angle / Hook Note |
|---|---|---|---|---|---|---|---|---|
| Wolverine Trading | B | Head of Trading Technology | [To Be Sourced] | InMail | 2026-09-21 | 2026-09-28 | Not Started | Variant A — confirmed C++/C#/Python/SQL stack with a dedicated latency-critical-systems track. |
| DRW | B | Head of Core Infrastructure | [To Be Sourced] | Email | 2026-09-21 | 2026-09-28 | Not Started | Variant C — their own Core Infra C++ req combines "C++ and Python" under one tight-latency requirement; quote it back to them. |
| Virtu Financial | B | Head of Core Development | [To Be Sourced] | Email | 2026-09-21 | 2026-09-28 | Not Started | Variant A — Core Dev team explicitly builds "internal messaging infrastructure"; IPC-messaging-specific hook. |
| Citadel Securities | B | Head of Trading Systems Engineering | [To Be Sourced] | Email | 2026-09-21 | 2026-09-28 | Not Started | Variant C — SDET role builds "Python-first frameworks with C++-adjacent hooks" protecting latency-sensitive releases. |
| Akuna Capital | B | Head of Trading Technology | [To Be Sourced] | InMail | 2026-09-21 | 2026-09-28 | Not Started | Variant B — Python role names Kafka explicitly; direct Kafka-vs-zero-copy contrast is the sharpest hook available. |

## Week 4 — Batch 4 (Tier B, first touch 2026-09-28)

| Firm Name | Tier | Recommended Target Title | Contact Name | Outreach Channel | First Touch Date | Day-5 Bump Date | Status | Technical Angle / Hook Note |
|---|---|---|---|---|---|---|---|---|
| Old Mission Capital | B | Head of Engineering | [To Be Sourced] | InMail | 2026-09-28 | 2026-10-05 | Not Started | Variant C — junior devs write production Python touching exchange connectivity and real-time data directly. |
| Cumberland (DRW crypto arm) | B | Head of Trading Technology, Cumberland | [To Be Sourced] | InMail | 2026-09-28 | 2026-10-05 | Not Started | Variant C — single req requires 5+ yrs C++ and 3+ yrs Python on the same crypto market-data stack. |
| Wintermute | B | Head of Trading Technology | [To Be Sourced] | InMail | 2026-09-28 | 2026-10-05 | Not Started | Variant A — C++/Python trading platform, Linux-only infra; crypto-MM tail-latency framing. |
| Chicago Trading Company (CTC) | B | Head of Low-Latency Engineering | [To Be Sourced] | InMail | 2026-09-28 | 2026-10-05 | Not Started | Variant B — Python/C++ testbench work sits next to a dedicated low-latency market-data team. |
| Belvedere Trading | B | Head of Systematic Volatility Trading Technology | [To Be Sourced] | InMail | 2026-09-28 | 2026-10-05 | Not Started | Variant C — Python/C++ named together for production implementation on the vol-trading desk. |

## Week 5 — Batch 5 (Tier B/C, first touch 2026-10-05)

| Firm Name | Tier | Recommended Target Title | Contact Name | Outreach Channel | First Touch Date | Day-5 Bump Date | Status | Technical Angle / Hook Note |
|---|---|---|---|---|---|---|---|---|
| GSR Markets | B | Head of Trading Systems Engineering | [To Be Sourced] | InMail | 2026-10-05 | 2026-10-12 | Not Started | Variant B — C++ team moving to Rust, Python (pandas/numpy) named alongside; use their Rust migration as a timing trigger. |
| PDT Partners | B | Head of Trading Infrastructure | [To Be Sourced] | Email | 2026-10-05 | 2026-10-12 | Not Started | Variant C — C++/Python named in the same OMS/exchange-connectivity/market-data stack. |
| ExodusPoint | B | Head of Core Infrastructure | [To Be Sourced] | Email | 2026-10-05 | 2026-10-12 | Not Started | Variant B — Kafka/Hadoop/Spark in the data stack plus reusable Python/C++ components; Kafka contrast hook. |
| Tower Research Capital | C | (title not yet identified) | [To Be Sourced] | Email | 2026-10-05 | 2026-10-12 | Not Started | Variant A — hub-and-spoke C++/Python split is generic; treat as a general tail-latency intro, evidence is thinner here. |
| Vatic Labs | C | (title not yet identified) | [To Be Sourced] | InMail | 2026-10-05 | 2026-10-12 | Not Started | Variant A — concurrently open Python and Low Latency C++ reqs; generic intro, gauge actual fit on the call. |

---

## Notes

- Dates above are the planned schedule, not confirmed sends — update First Touch Date to the actual send date once a message goes out, and recompute Day-5 Bump Date from that real date if it slips.
- 2026-09-07 (the natural Monday for Batch 1) is Labor Day in the US — Batch 1 first touch is set to Tuesday 2026-09-08 instead.
- Tier A goes out first by design: strongest evidence, highest expected response rate, and any pattern in what lands (which variant, which channel) should inform how Batches 2–5 are actually sent, not just this static schedule.
- "[To Be Sourced]" placeholders should be replaced firm-by-firm via LinkedIn before First Touch — no individual names were sourced during the original ICP research pass.
- Batch 1 and Batch 2 have each had one named-contact research pass (see "Contact sourcing notes" under each batch above) — every name found carries a real caveat (unconfirmed title, no surname, conflicting sources, or no verified contact channel), so treat each as a lead to verify on LinkedIn before sending, not a ready-to-use contact. No email address or LinkedIn URL was fabricated for any firm above.
