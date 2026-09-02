# Animus Core — Outbound Tracker

Working tracker for the 25-firm target list in [`ICP_TARGET_LIST.md`](ICP_TARGET_LIST.md), staged across 5 weekly batches of 5 firms each so response signal from earlier batches can inform later ones. Templates referenced by variant letter are in [`OUTREACH_TEMPLATES.md`](OUTREACH_TEMPLATES.md).

**Channel note:** Telegram is listed as a column option for completeness but is not in active use (no Telegram account currently) — every row below defaults to Email or LinkedIn InMail per firm. Update the "Contact Name" and "Outreach Channel" columns once a real contact is sourced; a firm with no findable corporate email should default to InMail regardless of what's assigned below.

**Cadence:** First Touch on the assigned date; Day-5 Bump is First Touch + 7 calendar days (~5 business days), matching the bump templates. Update Status as each firm moves — **Not Started → Sent → Replied → Scheduled PoP** — and log a real bump date once the actual first-touch date is confirmed (dates below are the planned schedule, not sent confirmations).

---

## Week 1 — Batch 1 (Tier A, first touch 2026-09-08)

| Firm Name | Tier | Recommended Target Title | Contact Name | Outreach Channel | First Touch Date | Day-5 Bump Date | Status | Technical Angle / Hook Note |
|---|---|---|---|---|---|---|---|---|
| Hudson River Trading | A | Head of Trading Technology / Platform Engineering Lead | [To Be Sourced] | InMail | 2026-09-08 | 2026-09-15 | Not Started | They built their own OSS interop layer (pymetabind) — lead with respect for that work, then Variant C: "same problem, different transport underneath it" rather than a hard sell. |
| Two Sigma | A | Fast Engineering Lead | [To Be Sourced] | Email | 2026-09-08 | 2026-09-15 | Not Started | Variant B (throughput/scale) — their own "Fast Engineering" framing already splits deployment by latency sensitivity; mirror that language. |
| Point72 / Cubist | A | Head of Trading Infrastructure (Cubist) | [To Be Sourced] | Email | 2026-09-08 | 2026-09-15 | Not Started | Variant A (tail-latency) — open Low-Latency Market Data Engineer req; anchor the hook on market-data ingestion specifically. |
| Millennium (SPEED team) | A | Head of Systematic Platform Execution & Exchange Data | [To Be Sourced] | Email | 2026-09-08 | 2026-09-15 | Not Started | Variant C (Python interop) — their own req names Python + Apache Arrow + real-time feeds in one role; reference the Arrow/columnar handoff directly. |
| Man Group / Man AHL | A | Head of AHL Technology | [To Be Sourced] | Email | 2026-09-08 | 2026-09-15 | Not Started | Variant C — public "why Python" identity; lead with respecting their Python-first culture, "no rewrite" lands hardest here. |

## Week 2 — Batch 2 (Tier B, first touch 2026-09-14)

| Firm Name | Tier | Recommended Target Title | Contact Name | Outreach Channel | First Touch Date | Day-5 Bump Date | Status | Technical Angle / Hook Note |
|---|---|---|---|---|---|---|---|---|
| IMC Trading | B | Head of Low-Latency Engineering | [To Be Sourced] | InMail | 2026-09-14 | 2026-09-21 | Not Started | Variant C — dedicated Python dev track sits organizationally apart from the low-latency C++/FPGA team; hook on bridging that gap. |
| XTX Markets | B | Head of Engineering | [To Be Sourced] | InMail | 2026-09-14 | 2026-09-21 | Not Started | Variant B — Python (NumPy/PyTorch/JAX) research to C++ production split; frame around research-to-prod handoff latency. |
| Optiver | B | Head of Low-Latency Engineering | [To Be Sourced] | Email | 2026-09-14 | 2026-09-21 | Not Started | Variant A — Low Latency Network Engineer req wants Python scripting bridging into FPGA/C++; FPGA-adjacent tail-latency framing. |
| Squarepoint Capital | B | Head of Platform Engineering | [To Be Sourced] | Email | 2026-09-14 | 2026-09-21 | Not Started | Variant B — Ultra Low Latency Platform Engineer role plus a separate Python platform track; throughput framing. |
| D.E. Shaw | B | Head of Quant Systems Engineering | [To Be Sourced] | Email | 2026-09-14 | 2026-09-21 | Not Started | Variant B — Quant Systems Developer role names Python/Golang/Rust for kernel/network latency tuning explicitly. |

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
- "[To Be Sourced]" placeholders should be replaced firm-by-firm via LinkedIn before First Touch — no individual names were sourced during the ICP research pass.
