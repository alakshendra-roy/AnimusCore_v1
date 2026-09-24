# Animus Core — Institutional Outreach Pipeline & Account Dossier

**Pitch angle:** this pipeline is built around the raw-engine performance story — the 153-run, 62-minute zero-allocation soak verification (`docs/marketing_whitepapers/AnimusCore_Thermal_Soak_Whitepaper.pdf`, kit copy in `dist/animus_client_eval_kit/`). It targets teams that own low-latency C++ infrastructure directly and would evaluate/license the engine itself (Desk License / Enterprise / OEM-Redistribution tiers — see [`desk_license_tier`] pricing), **not** the Python-interop bridge story that [`ICP_TARGET_LIST.md`](../operations_legal/ICP_TARGET_LIST.md) and [`OUTBOUND_TRACKER.md`](../operations_legal/OUTBOUND_TRACKER.md) are built around. These are two distinct sales motions running in parallel — see the overlap notes below before sending anything on this list.

> **Read before sending — known overlap with the existing motion:**
> - **Optiver** and **IMC Trading** already show `Sent` status as of 2026-09-23 in `OUTBOUND_TRACKER.md`, under the Python-bridge pitch, to named contacts (Scott McKenzie/Lance Braunstein at Optiver; Sunny Khiani at IMC). Sending this soak-verification pitch to the same people less than 48 hours later, cold, risks reading as scattershot. Both are marked **Staged** below, not **Ready** — recommend holding until there's response signal (or lack of it) from the existing thread, then following up with *this* pitch as a second angle from the same sender rather than a fresh cold open.
> - **Jump Trading** and **Jane Street** were explicitly *excluded* from the Python-bridge ICP list (`ICP_TARGET_LIST.md` §3) — Jump Trading because "Python is research/prototyping only; production trading runs in C++," Jane Street because its core language is OCaml, not Python. Neither exclusion applies here: this pitch has no Python-interop claim, and a heavy-native-C++ shop like Jump Trading is arguably a *better* fit for a raw zero-allocation engine story than for the Python bridge. Included below on that basis, not by oversight.
> - No individual contact names are asserted below beyond what already exists in the tracker. Unlike `ICP_TARGET_LIST.md`, this document has not been through a per-firm web-research/verification pass — **"Contact" is a role-level persona, not a sourced name**, until someone runs that pass the way Batches 1–3 did.

**Outreach Status legend:** `Ready` = cleared to send on next available slot. `Staged` = drafted/queued but intentionally held (sequencing conflict, unsourced contact, or awaiting a decision) — see the Notes column for why.

---

## Tier 1 — US (NYC / Chicago)

| Target Firm | Persona | Core Pain Point | Outreach Channel | Outreach Status | Notes |
|---|---|---|---|---|---|
| Citadel Securities | Head of Trading Systems Engineering | Tail-latency jitter under contention | Direct Email | Ready | No sourced contact yet — needs a Batch-style research pass before send. |
| Jump Trading | Lead Low-Latency C++ Engineer | Memory allocation regressions on the hot path | LinkedIn InMail | Ready | Excluded from the Python-bridge motion for being C++-native — that's a fit signal here, not a disqualifier. |
| DRW | Head of Core Infrastructure | Serialization bottlenecks between market data and strategy | Direct Email | Ready | DRW appears in `ICP_TARGET_LIST.md` Tier B (Python-bridge angle, C++/Python split req). Different pitch, same firm — fine to run both in parallel since it wasn't excluded there. |
| Hudson River Trading | Head of Execution Infrastructure | Tail-latency jitter under contention | LinkedIn InMail | Staged | Also on the Python-bridge list (`Sent` 2026-09-22, per `OUTBOUND_TRACKER.md` Batch 1) via a partial/low-confidence contact lead. Hold for the same sequencing reason as Optiver/IMC. |
| Jane Street | Lead Low-Latency C++ Engineer | Memory allocation regressions on the hot path | LinkedIn InMail | Ready | Core language is OCaml (per `ICP_TARGET_LIST.md` §3) — the C++ engine/licensing pitch may not map cleanly onto their stack. Worth a light-touch send, not a heavy investment, until that's confirmed one way or the other. |
| Tower Research Capital | Head of Trading Technology | Tail-latency jitter under contention | Direct Email | Ready | Listed Tier C in `ICP_TARGET_LIST.md` ("real but generic" C++/Python split) — no conflict, no existing send. |
| Belvedere Trading | Head of Execution Infrastructure | Serialization bottlenecks between market data and strategy | Direct Email | Ready | Also Tier B on the Python-bridge list, currently `Not Started` there — no conflict yet, but re-check status before send in case that changes first. |

## Tier 1 — Europe (London / Amsterdam)

| Target Firm | Persona | Core Pain Point | Outreach Channel | Outreach Status | Notes |
|---|---|---|---|---|---|
| Optiver | Head of Low-Latency Engineering | Tail-latency jitter under contention | Direct Email | **Staged** | `Sent` 2026-09-23 under the Python-bridge pitch to Scott McKenzie / Lance Braunstein. Hold for sequencing — see overlap note above. |
| Flow Traders | Lead Low-Latency C++ Engineer | Memory allocation regressions on the hot path | LinkedIn InMail | Ready | Not present on the existing Python-bridge list — clean slate, no sourced contact yet. |
| IMC Trading | Head of Low-Latency Engineering | Serialization bottlenecks between market data and strategy | Direct Email | **Staged** | `Sent` 2026-09-23 under the Python-bridge pitch to Sunny Khiani (MD). Hold for sequencing — see overlap note above. |
| Wintermute | Head of Trading Technology | Tail-latency jitter under contention | LinkedIn InMail | Ready | Also on the Python-bridge Tier B list (C++/Python role cited), currently `Not Started` there — no conflict yet. |
| Maven Derivatives | Head of Execution Infrastructure | Memory allocation regressions on the hot path | LinkedIn InMail | Ready | Not present on the existing Python-bridge list — clean slate, no sourced contact yet. |

## Tier 1 — APAC / India

| Target Firm | Persona | Core Pain Point | Outreach Channel | Outreach Status | Notes |
|---|---|---|---|---|---|
| Graviton Research Capital | Lead Low-Latency C++ Engineer | Tail-latency jitter under contention | LinkedIn InMail | Ready | Not on the existing Python-bridge list. |
| AlphaGrep | Head of Trading Technology | Memory allocation regressions on the hot path | Direct Email | Ready | Not on the existing Python-bridge list. |
| Quadeye | Head of Execution Infrastructure | Serialization bottlenecks between market data and strategy | LinkedIn InMail | Ready | Not on the existing Python-bridge list. |
| NK Securities | Lead Low-Latency C++ Engineer | Tail-latency jitter under contention | Direct Email | Ready | Not on the existing Python-bridge list. |

---

## Outreach templates

Both reference the 153-run/62-minute soak specifically — do not blend these with the Python-bridge datasheet numbers (p50 53.3ns / 47.3M msgs/sec) from `OUTREACH_TEMPLATES.md`; they're a different benchmark configuration and mixing them in one message undermines the "don't round up, don't overclaim" standard both pitches are built on.

### Template 1 — Cold email

**Subject:** `0 heap allocations, 153 runs, 62 minutes straight — soak report attached`

> Hi [First Name],
>
> We just finished a 62-minute unattended soak of our core telemetry engine: 153 back-to-back runs, SPSC ingest latency holding at a 21.08ns median (P90 24.39ns, P99 27.70ns), MPMC throughput steady at a 7.47M pushes/sec median under 8-producer contention. Every run verified **zero heap allocations** on the hot path via a runtime watchdog, and **153/153 event sequences intact** — no loss, no reordering, across 21M+ events per run.
>
> We're also not hiding the one thing that didn't look perfect: run #123 hit a single 30ms scheduler-preemption spike — real, reported as-is, not smoothed over. Full methodology, the raw per-run data, and that one outlier are all in the attached whitepaper.
>
> If your team is fighting tail-latency jitter, allocation regressions under load, or a serialization tax between your market-data path and strategy code, this is a fifteen-minute conversation, not a sales pitch — bring your own hardware and event shapes and we'll run the same harness on your box.
>
> Worth a look?
>
> Alakshendra Roy
> Founder & Chief Architect — Animus Core
> alakshendra@animusinfra.com | +91 9891161189
> animusinfra.com

### Template 2 — LinkedIn InMail (short form)

> Hi [First Name] — ran a 62-minute, 153-run soak on our zero-allocation C++ telemetry engine: 21.08ns median SPSC ingest, 0 heap allocations end to end, sequence-intact on every single run. Full report + one honest outlier (a single 30ms scheduler spike) here: [link]. If tail-latency jitter or allocation regressions are live pain for your team, open to running the same harness on your own hardware — 15 minutes to see if it's worth a longer look?

---

## Next steps before first send

1. Run a `ICP_TARGET_LIST.md`-style research pass (job postings, conference talks, LinkedIn) to source real named contacts for every row currently without one — none are fabricated here.
2. Get an explicit decision on the four `Staged` rows (Optiver, IMC Trading, Hudson River Trading, and re-check Belvedere/Wintermute before send) rather than defaulting them to `Ready` on a future pass.
3. Log every actual send in the pinned Outbound Console artifact (`SPa2n9NPXxTdYU5xMJ2McP`), the same as the Python-bridge motion, so both pipelines are visible in one place.
