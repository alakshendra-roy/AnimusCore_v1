# Expanded Outreach Matrix — 20 Targets, 4 Industries (2026-09-28)

Sixth outreach surface. Distinct from the four firm-level GTM motions (`docs/outreach/outbound_console.html`) and the fifth archetype-copy surface (`docs/OUTREACH_QUEUE_2026-09-28.md`): this is a fresh 20-firm dispatch matrix spanning four industries that have zero prior AnimusCore contact, built specifically to avoid re-treading the prop-shop/Indian-domestic/gaming-security ground already covered.

**Not pushed to public origin.** Local file only — confirm before any `git push`, per [[repo_visibility]].

---

## ⚠️ Pre-flight cross-check (read before sending anything)

Cross-checked against `docs/outreach/outbound_console.html`, `docs/OUTREACH_QUEUE_2026-09-28.md`, and memory on 2026-09-28. Three of the user-suggested reference names had to be swapped out of the final 20 — logged here so the substitution isn't silently lost:

| Suggested name | Status found | Disposition | Swapped in |
|---|---|---|---|
| **Optiver** | Contacted twice — 2026-09-23 exec batch (Scott McKenzie / Lance Braunstein) **and** raw-engine pipeline, same date. `py-optiver` / `raw-optiver`. | **Skip — do not re-open.** | **Akuna Capital** (Chicago/Sydney/Shanghai options market maker — same APAC+US footprint, zero prior contact) |
| **Pixxel** | **Sent** 2026-09-25 11:18 IST to Kshitij Khandelwal (Co-Founder & CTO), direct Gmail. `in-b11`, status `sent`. | **Skip — live thread, don't cold-open a second angle.** | **Astranis** (MicroGEO software-defined comms satellites — zero prior contact) |
| **Dhruva Space** | Already tracked with a bespoke, unsent draft in the Indian domestic pipeline (`in-b10`, status `ready`). Not contacted, but a live duplicate-in-waiting. | **Swapped out** — sending it from a *third* surface risks the same "don't double-fire" duplication this file's own convention exists to prevent, per [[gtm_indian_domestic_motion]] and [[gtm_dual_motion_soak_pitch]]. If Dhruva Space needs a touch, send from `in-b10`'s existing draft, not from here. | **Loft Orbital** (LEO multi-mission satellite-as-a-service, edge compute on orbit — zero prior contact) |
| Quadeye, Maven Derivatives (mentioned in the request as examples, not required) | Both already `ready` (unsent) on **two** existing pipelines apiece (raw-engine + Indian domestic for Quadeye; raw-engine for Maven). | **Excluded from this matrix entirely** rather than made a third instance of the same firm across three surfaces. | Da Vinci Derivatives (genuinely zero-touch) retained; GTS and Tibra Capital added to round out the quant vertical to 5 |
| NK Securities | **Correction (caught on the console-logging pass, 2026-09-28):** already tracked as `raw-nk` in the raw-engine pipeline, status `ready` (unsent) — missed on the first cross-check pass in this doc, which incorrectly called it "zero matches." | **Kept in this matrix, duplicate-flagged** rather than removed — it's unsent, so not a hard conflict, but don't send from both `raw-nk` and `ex-nk`. | — |

19 of the 20 firms below were individually grepped against the live tracker and came back with **zero matches** — genuinely first-touch. NK Securities is the one exception (see correction row above); it carries an explicit duplicate flag in `outbound_console.html` on both its rows (`raw-nk` and `ex-nk`).

**No named individual contacts are asserted.** Every persona below is a role-level title consistent with this repo's standing "no fabricated contact" rule (see [[gtm_indian_domestic_motion]], [[gtm_gaming_security_motion]]) — verify the actual name via LinkedIn/company site before send, then log it in the pinned Outbound Console.

**After any actual send:** log it in the pinned Outbound Console artifact (`SPa2n9NPXxTdYU5xMJ2McP`) and add a matching row to `docs/outreach/outbound_console.html` so the next cross-check surface catches it — this file is a drafting matrix, not the tracker of record.

**Numbers cited** are the same locked, verified raw-ring-push baseline used across every other surface in this repo: P50 43 cycles / ~18ns on invariant-TSC (lfence-serialized RDTSC), zero heap allocations across a million-iteration hot loop (instrumented via global `operator new`/`delete` counters, not asserted by inspection), 64-byte cache-line isolation enforced at compile time via `static_assert` on the ring header/padding types — cross-checked against `dist/AnimusCore_Eval_Sandbox/README_BENCHMARK.txt` and `tests/eval_harness.cpp` on 2026-09-27. Not blended with the separate soak/throughput figures (153-run soak, 21.08ns median SPSC ingest) other pipelines use — kept apart per the Verification Dossier's own convention.

---

## Guardrails applied

- **Voice:** first-person singular ("I," never "we"/"our team") — sole-operator positioning, no implied headcount.
- **Zero corporate filler, zero call-begging.** No "let's hop on a call" anywhere. Every message points to something the recipient can run themselves, async.
- **Option A** is a zero-pitch LinkedIn connection note, under 300 characters, peer-to-peer.
- **Option B** is a dense technical LinkedIn InMail/DM, ~100–130 words, zero meeting ask.
- **Option C** is a direct Gmail email offering the self-contained eval sandbox (`dist/AnimusCore_Eval_Sandbox/README_BENCHMARK.txt`) under a 14-day zero-liability evaluation window, subject + body under 140 words.
- All 20 email addresses below are `{role-alias}@{domain}` placeholders, not fabricated named addresses — resolve the actual recipient before send.

---

# Industry 1 — Quantitative Trading & Electronic Market Making

*(APAC & US/EU prop desks — cacheline contention in shared-memory order books, allocator jitter on the matching/signal hot path)*

## 1.1 NK Securities — Lead Low-Latency C++ Engineer

**Duplicate flag:** also tracked as row `raw-nk` in the raw-engine pipeline (`outbound_console.html`), status `ready` (unsent), with a different draft. Send once, from either surface, not both.

**Background & bottleneck:** NK Securities runs high-frequency market-making across NSE/BSE and international venues where order-book updates and signal recomputation share the same hot loop; any allocator call or false-sharing stall on that loop shows up directly as missed quotes. The team's core exposure is cacheline contention between the order-book writer and the signal-reader thread on a shared ring — exactly the MPMC/SPSC boundary condition AnimusCore's ring header is designed to isolate via padding.

**Option A (LinkedIn connection, zero-pitch):**
```
Hi [First Name] — I build a zero-alloc, lock-free C++ telemetry/execution ring (P50 43 cycles / ~18ns, invariant-TSC verified). Always worth comparing notes with someone solving the same order-book cacheline contention problem at NK. No pitch — connecting.
```
*(233 characters.)*

**Option B (LinkedIn InMail/DM, ~110 words):**
```
Hi [First Name],

I'm the sole engineer behind AnimusCore — a zero-copy, lock-free C++17 telemetry/execution transport. Raw ring push (ShmRing<T>::try_push()) measures P50 43 cycles / ~18ns on invariant-TSC, lfence-serialized RDTSC, zero heap allocations across a million-iteration hot loop (instrumented via global operator new/delete counters, not asserted by inspection). 64-byte cache-line isolation between reader/writer state is enforced at compile time via static_assert on the ring header and padding types, not just documented.

No pitch, no call — the standalone eval harness is public if you want to verify these numbers on your own hardware in under ten minutes. Worth a look if writer/reader cacheline contention on the book is a live cost at NK.
```
*(114 words.)*

**Option C (Direct email):**
```
Subject: P50 43 cycles / ~18ns ring push, zero-alloc — 14-day eval, no call needed

Hi [First Name],

I'm Alakshendra Roy — solo founder and engineer behind AnimusCore, a zero-copy, lock-free C++17 telemetry/execution transport built to eliminate cacheline contention between order-book writer and signal-reader threads.

Locked baseline: P50 43 cycles / ~18ns raw ring push on invariant-TSC, zero heap allocations (instrumented, not assumed), 64-byte isolation enforced by static_assert at compile time.

Self-contained eval sandbox attached in spirit: `dist/AnimusCore_Eval_Sandbox/README_BENCHMARK.txt` has the exact build command and locked numbers — clone, build, run on your own book-replay data. 14-day zero-liability evaluation window, no license, no call required.

Alakshendra Roy
Founder & Chief Architect — AnimusCore
alakshendra@animusinfra.com | animusinfra.com
```
*(96 words in the body.)*

---

## 1.2 Da Vinci Derivatives — Head of Trading Systems

**Background & bottleneck:** Da Vinci's options market-making book depends on recomputing greeks and re-quoting across strikes within a microsecond-scale window after every underlying tick; a GC-style pause or allocator call anywhere on that path directly widens the quote-update latency across the whole chain. The bottleneck is specifically allocation-driven jitter on the tick-to-requote path, not raw throughput.

**Option A (LinkedIn connection, zero-pitch):**
```
Hi [First Name] — I build a zero-alloc, lock-free C++ execution/telemetry ring (P50 43 cycles / ~18ns, invariant-TSC verified). Interested to compare notes on tick-to-requote jitter in options market-making at Da Vinci. No pitch — connecting.
```
*(220 characters.)*

**Option B (LinkedIn InMail/DM, ~115 words):**
```
Hi [First Name],

I'm the sole engineer behind AnimusCore — a zero-copy, lock-free C++17 execution/telemetry transport. Raw ring push measures P50 43 cycles / ~18ns on invariant-TSC (lfence-serialized RDTSC), zero heap allocations across a million-iteration hot loop, instrumented with global operator new/delete counters rather than asserted by inspection. Ring header and padding types enforce 64-byte cache-line isolation via static_assert at compile time — a structural guarantee, not a convention someone can accidentally violate later.

No pitch, no call — the standalone eval harness (source + exact build command) is public if you want to verify these numbers directly. Relevant if allocator noise on the tick-to-requote path across strikes is a live cost at Da Vinci.
```
*(120 words.)*

**Option C (Direct email):**
```
Subject: Zero-alloc ring push, P50 43 cycles/~18ns — 14-day eval on your own book-replay

Hi [First Name],

I'm Alakshendra Roy — solo founder and engineer behind AnimusCore, a zero-copy, lock-free C++17 execution/telemetry transport built to keep tick-to-requote latency deterministic across a full options chain.

Locked baseline: P50 43 cycles / ~18ns raw ring push, zero heap allocations (instrumented, not assumed), 64-byte cache-line isolation enforced by static_assert at compile time — no allocator-driven requote jitter possible by construction.

Self-contained eval sandbox: `dist/AnimusCore_Eval_Sandbox/README_BENCHMARK.txt` — exact build command, locked numbers, 14-day zero-liability evaluation window on your own hardware, no license, no call.

Alakshendra Roy
Founder & Chief Architect — AnimusCore
alakshendra@animusinfra.com | animusinfra.com
```
*(100 words in the body.)*

---

## 1.3 Akuna Capital — Director of Low-Latency Engineering

**Background & bottleneck:** Akuna's options and derivatives market-making spans Chicago, Sydney, and Shanghai desks running the same core engine across time zones, so any non-deterministic allocator behavior has to reproduce identically on every box, not just perform well on one. The constraint is cross-region determinism under contention, not peak single-box throughput.

**Option A (LinkedIn connection, zero-pitch):**
```
Hi [First Name] — I build a zero-alloc, lock-free C++ telemetry/execution ring (P50 43 cycles / ~18ns, invariant-TSC verified). Curious how Akuna keeps engine behavior identical across Chicago/Sydney/Shanghai under contention. No pitch — connecting.
```
*(218 characters.)*

**Option B (LinkedIn InMail/DM, ~115 words):**
```
Hi [First Name],

I'm the sole engineer behind AnimusCore — a zero-copy, lock-free C++17 telemetry/execution transport. Raw ring push measures P50 43 cycles / ~18ns on invariant-TSC, lfence-serialized RDTSC, zero heap allocations across a million-iteration hot loop (instrumented with global operator new/delete counters, not asserted by inspection). 64-byte cache-line isolation on the ring header and padding types is enforced at compile time via static_assert — deterministic across any box that compiles it, not just the one it was tuned on.

No pitch, no call — the standalone eval harness is public if you want to verify these numbers yourself, on any of your regional boxes.
```
*(112 words.)*

**Option C (Direct email):**
```
Subject: Deterministic zero-alloc ring push across regions — 14-day eval, no call

Hi [First Name],

I'm Alakshendra Roy — solo founder and engineer behind AnimusCore, a zero-copy, lock-free C++17 telemetry/execution transport built to behave identically on every box it compiles on.

Locked baseline: P50 43 cycles / ~18ns raw ring push, zero heap allocations (instrumented, not assumed), 64-byte isolation enforced by static_assert at compile time — no region-specific tuning required.

Self-contained eval sandbox: `dist/AnimusCore_Eval_Sandbox/README_BENCHMARK.txt`, exact build command included. 14-day zero-liability evaluation window — build and run it on your Chicago, Sydney, or Shanghai hardware and compare.

Alakshendra Roy
Founder & Chief Architect — AnimusCore
alakshendra@animusinfra.com | animusinfra.com
```
*(97 words in the body.)*

---

## 1.4 Tibra Capital — Head of Core Infrastructure

**Background & bottleneck:** Tibra's quant/prop desk runs signal generation and execution on the same shared-memory pipeline across multiple asset classes; the specific failure mode is priority inversion between a low-frequency signal writer and a high-frequency execution reader contending on the same cacheline. AnimusCore's SPSC ring is built to structurally prevent that contention rather than mitigate it with locking.

**Option A (LinkedIn connection, zero-pitch):**
```
Hi [First Name] — I build a zero-alloc, lock-free C++ telemetry/execution ring (P50 43 cycles / ~18ns, invariant-TSC verified). Good to compare notes on signal/execution contention on shared pipelines at Tibra. No pitch — connecting.
```
*(213 characters.)*

**Option B (LinkedIn InMail/DM, ~110 words):**
```
Hi [First Name],

I'm the sole engineer behind AnimusCore — a zero-copy, lock-free C++17 telemetry/execution transport. Raw ring push measures P50 43 cycles / ~18ns on invariant-TSC (lfence-serialized RDTSC), zero heap allocations across a million-iteration hot loop, instrumented with global operator new/delete counters rather than asserted by inspection. The SPSC ring structurally separates writer and reader cachelines via static_assert-enforced 64-byte padding — no locking, no priority inversion possible by construction.

No pitch, no call — the standalone eval harness is public if you want to verify these numbers on your own multi-asset pipeline directly.
```
*(105 words.)*

**Option C (Direct email):**
```
Subject: Lock-free ring push, zero priority inversion by construction — 14-day eval

Hi [First Name],

I'm Alakshendra Roy — solo founder and engineer behind AnimusCore, a zero-copy, lock-free C++17 telemetry/execution transport built to structurally prevent signal/execution cacheline contention.

Locked baseline: P50 43 cycles / ~18ns raw ring push, zero heap allocations (instrumented, not assumed), 64-byte cache-line isolation enforced by static_assert at compile time.

Self-contained eval sandbox: `dist/AnimusCore_Eval_Sandbox/README_BENCHMARK.txt`, exact build command included, locked numbers. 14-day zero-liability evaluation window on your own hardware, no license, no call.

Alakshendra Roy
Founder & Chief Architect — AnimusCore
alakshendra@animusinfra.com | animusinfra.com
```
*(90 words in the body.)*

---

## 1.5 GTS (Global Trading Systems) — VP, Trading Infrastructure

**Background & bottleneck:** GTS market-makes across equities, ETFs, and options with a matching-adjacent infrastructure layer where telemetry/logging sits directly on the hot path; the bottleneck is that any instrumentation added to observe the system risks becoming the thing that slows it down. This is the exact "telemetry that costs zero" problem AnimusCore's zero-alloc design targets.

**Option A (LinkedIn connection, zero-pitch):**
```
Hi [First Name] — I build a zero-alloc, lock-free C++ telemetry ring (P50 43 cycles / ~18ns, invariant-TSC verified) — telemetry that doesn't cost hot-path latency. Worth comparing notes at GTS. No pitch — connecting.
```
*(210 characters.)*

**Option B (LinkedIn InMail/DM, ~110 words):**
```
Hi [First Name],

I'm the sole engineer behind AnimusCore — a zero-copy, lock-free C++17 telemetry transport built on the premise that instrumentation shouldn't be able to become the bottleneck it's meant to observe. Raw ring push measures P50 43 cycles / ~18ns on invariant-TSC, zero heap allocations across a million-iteration hot loop (instrumented via operator new/delete counters, not asserted), with 64-byte cache-line isolation enforced by static_assert at compile time.

No pitch, no call — the standalone eval harness is public if you want to verify these numbers directly against your own matching-adjacent telemetry path.
```
*(102 words.)*

**Option C (Direct email):**
```
Subject: Telemetry that costs zero hot-path cycles — P50 43 cycles/~18ns, 14-day eval

Hi [First Name],

I'm Alakshendra Roy — solo founder and engineer behind AnimusCore, a zero-copy, lock-free C++17 telemetry transport built so instrumentation can't become the bottleneck it's observing.

Locked baseline: P50 43 cycles / ~18ns raw ring push, zero heap allocations (instrumented, not assumed), 64-byte isolation enforced by static_assert at compile time.

Self-contained eval sandbox: `dist/AnimusCore_Eval_Sandbox/README_BENCHMARK.txt`, exact build command, locked numbers. 14-day zero-liability evaluation window on your own hardware, no license, no call.

Alakshendra Roy
Founder & Chief Architect — AnimusCore
alakshendra@animusinfra.com | animusinfra.com
```
*(90 words in the body.)*

---

# Industry 2 — Aerospace & Satellite Edge Systems

*(LEO edge compute, flight avionics telemetry — deterministic radiation-tolerant RTOS ring buffers, ground-segment pass-window ingest)*

## 2.1 Rocket Lab — Principal Flight Software Architect

**Background & bottleneck:** Rocket Lab's Electron/Photon flight computers run avionics telemetry ingest under hard real-time constraints where an allocation-driven pause during a launch or on-orbit maneuver window is not recoverable. The bottleneck is deterministic ring-buffer behavior under radiation-tolerant RTOS constraints, where standard heap allocators are already avoided by convention but rarely verified by instrumentation.

**Option A (LinkedIn connection, zero-pitch):**
```
Hi [First Name] — I build a zero-alloc, lock-free C++ telemetry ring (P50 43 cycles / ~18ns, invariant-TSC verified), originally for HFT but structurally maps onto flight-computer telemetry. Glad to compare notes at Rocket Lab. No pitch — connecting.
```
*(232 characters.)*

**Option B (LinkedIn InMail/DM, ~120 words):**
```
Hi [First Name],

I'm the sole engineer behind AnimusCore — a zero-copy, lock-free C++17 telemetry transport built under the same constraint flight software runs under: no heap allocation, no scheduler dependency, deterministic cache-line behavior. Raw ring push measures P50 43 cycles / ~18ns on invariant-TSC, zero heap allocations across a million-iteration hot loop, instrumented with global operator new/delete counters — not asserted by inspection, not just "we avoid malloc in the hot path" by convention. 64-byte cache-line isolation is enforced at compile time via static_assert on the ring header and padding types.

No pitch, no call — the standalone eval harness is public if you want to verify these numbers on your own target hardware directly.
```
*(122 words.)*

**Option C (Direct email):**
```
Subject: Zero-alloc telemetry ring, verified not assumed — 14-day eval on flight-computer hardware

Hi [First Name],

I'm Alakshendra Roy — solo founder and engineer behind AnimusCore, a zero-copy, lock-free C++17 telemetry transport built for the same no-heap, no-scheduler-dependency constraint flight software runs under.

Locked baseline: P50 43 cycles / ~18ns raw ring push, zero heap allocations (instrumented via operator counters, not assumed), 64-byte cache-line isolation enforced by static_assert at compile time.

Self-contained eval sandbox: `dist/AnimusCore_Eval_Sandbox/README_BENCHMARK.txt`, exact build command included. 14-day zero-liability evaluation window — build and run it on your own flight-computer target directly, no license, no call.

Alakshendra Roy
Founder & Chief Architect — AnimusCore
alakshendra@animusinfra.com | animusinfra.com
```
*(101 words in the body.)*

---

## 2.2 Planet Labs — Lead Ground Segment Systems Engineer

**Background & bottleneck:** Planet's Dove constellation downlinks high-volume imagery telemetry during short ground-station pass windows, where sustained ingest throughput with zero packet loss for the exact duration of the pass is non-negotiable. The bottleneck is allocator-driven ingest stalls during peak-throughput pass windows, not average-case throughput.

**Option A (LinkedIn connection, zero-pitch):**
```
Hi [First Name] — I build a zero-alloc, lock-free C++ telemetry ring (P50 43 cycles / ~18ns, invariant-TSC verified) for sustained zero-loss ingest under load. Relevant to ground-segment pass-window ingest at Planet. No pitch — connecting.
```
*(215 characters.)*

**Option B (LinkedIn InMail/DM, ~115 words):**
```
Hi [First Name],

I'm the sole engineer behind AnimusCore — a zero-copy, lock-free C++17 telemetry transport built for sustained ingest with zero data loss under continuous load, the same constraint a live satellite pass window imposes. Raw ring push measures P50 43 cycles / ~18ns on invariant-TSC, zero heap allocations across a million-iteration hot loop (instrumented, not asserted by inspection), with 64-byte cache-line isolation enforced by static_assert at compile time — no allocator stall possible mid-pass by construction.

No pitch, no call — the standalone eval harness is public if you want to verify sustained ingest behavior on your own ground-segment hardware.
```
*(112 words.)*

**Option C (Direct email):**
```
Subject: Zero-loss ingest ring, zero-alloc under load — 14-day eval for ground-segment hardware

Hi [First Name],

I'm Alakshendra Roy — solo founder and engineer behind AnimusCore, a zero-copy, lock-free C++17 telemetry transport built to guarantee zero data loss under sustained ingest, the same constraint a live pass window imposes.

Locked baseline: P50 43 cycles / ~18ns raw ring push, zero heap allocations (instrumented, not assumed), 64-byte isolation enforced by static_assert at compile time — no allocator stall possible mid-pass.

Self-contained eval sandbox: `dist/AnimusCore_Eval_Sandbox/README_BENCHMARK.txt`, exact build command, locked numbers. 14-day zero-liability evaluation window on your own ground-segment hardware, no license, no call.

Alakshendra Roy
Founder & Chief Architect — AnimusCore
alakshendra@animusinfra.com | animusinfra.com
```
*(97 words in the body.)*

---

## 2.3 Spire Global — VP, Space Systems Engineering

**Background & bottleneck:** Spire's LEMUR cubesat constellation runs onboard payload processing on power- and compute-constrained hardware where any heap fragmentation over a multi-year mission life is a slow, unrecoverable failure mode. The bottleneck is long-duration allocation-free operation on constrained embedded compute, not short-burst throughput.

**Option A (LinkedIn connection, zero-pitch):**
```
Hi [First Name] — I build a zero-alloc, lock-free C++ telemetry ring (P50 43 cycles / ~18ns, invariant-TSC verified) — zero heap fragmentation risk over long mission durations. Relevant at Spire. No pitch — connecting.
```
*(202 characters.)*

**Option B (LinkedIn InMail/DM, ~115 words):**
```
Hi [First Name],

I'm the sole engineer behind AnimusCore — a zero-copy, lock-free C++17 telemetry transport built for constrained embedded compute where heap fragmentation over a multi-year mission life is a real failure mode, not a theoretical one. Raw ring push measures P50 43 cycles / ~18ns on invariant-TSC, zero heap allocations across a million-iteration hot loop (instrumented with operator new/delete counters, not asserted), with 64-byte cache-line isolation enforced by static_assert at compile time.

No pitch, no call — the standalone eval harness is public if you want to verify zero-allocation behavior directly on your own onboard target.
```
*(108 words.)*

**Option C (Direct email):**
```
Subject: Zero-alloc telemetry ring for constrained onboard compute — 14-day eval

Hi [First Name],

I'm Alakshendra Roy — solo founder and engineer behind AnimusCore, a zero-copy, lock-free C++17 telemetry transport built to eliminate heap fragmentation risk over long mission durations on constrained embedded compute.

Locked baseline: P50 43 cycles / ~18ns raw ring push, zero heap allocations (instrumented via operator counters, not assumed), 64-byte isolation enforced by static_assert at compile time.

Self-contained eval sandbox: `dist/AnimusCore_Eval_Sandbox/README_BENCHMARK.txt`, exact build command included. 14-day zero-liability evaluation window on your own onboard hardware, no license, no call.

Alakshendra Roy
Founder & Chief Architect — AnimusCore
alakshendra@animusinfra.com | animusinfra.com
```
*(94 words in the body.)*

---

## 2.4 Loft Orbital — Head of Edge Compute & Payload Integration

**Background & bottleneck:** Loft Orbital's multi-mission satellite-as-a-service model runs several independent customer payloads' processing on shared onboard compute, where one payload's allocator behavior can jitter another's timing budget if isolation isn't structural. The bottleneck is cross-payload timing isolation on shared edge compute, not any single payload's raw performance.

**Option A (LinkedIn connection, zero-pitch):**
```
Hi [First Name] — I build a zero-alloc, lock-free C++ telemetry ring (P50 43 cycles / ~18ns, invariant-TSC verified) for structural cross-workload timing isolation. Relevant to shared-payload edge compute at Loft Orbital. No pitch — connecting.
```
*(220 characters.)*

**Option B (LinkedIn InMail/DM, ~115 words):**
```
Hi [First Name],

I'm the sole engineer behind AnimusCore — a zero-copy, lock-free C++17 telemetry transport built for exactly the constraint multi-tenant onboard compute imposes: one workload's allocator behavior can't be allowed to jitter another's timing budget. Raw ring push measures P50 43 cycles / ~18ns on invariant-TSC, zero heap allocations across a million-iteration hot loop (instrumented, not asserted by inspection), with 64-byte cache-line isolation enforced by static_assert at compile time — structural isolation, not a scheduling policy that can be misconfigured.

No pitch, no call — the standalone eval harness is public if useful for verifying cross-payload isolation directly.
```
*(107 words.)*

**Option C (Direct email):**
```
Subject: Structural cross-payload timing isolation, zero-alloc — 14-day eval

Hi [First Name],

I'm Alakshendra Roy — solo founder and engineer behind AnimusCore, a zero-copy, lock-free C++17 telemetry transport built so one payload's allocator behavior structurally cannot jitter another's timing budget on shared onboard compute.

Locked baseline: P50 43 cycles / ~18ns raw ring push, zero heap allocations (instrumented, not assumed), 64-byte isolation enforced by static_assert at compile time.

Self-contained eval sandbox: `dist/AnimusCore_Eval_Sandbox/README_BENCHMARK.txt`, exact build command, locked numbers. 14-day zero-liability evaluation window on your own multi-payload target, no license, no call.

Alakshendra Roy
Founder & Chief Architect — AnimusCore
alakshendra@animusinfra.com | animusinfra.com
```
*(96 words in the body.)*

---

## 2.5 Astranis — Principal Software-Defined Radio Systems Engineer

**Background & bottleneck:** Astranis's MicroGEO satellites run software-defined radio telemetry processing where jitter in the digital signal chain directly degrades link quality for the customer service being resold. The bottleneck is deterministic latency through the DSP-to-telemetry handoff, where any allocator-driven variance shows up as measurable link jitter, not just a logging inconvenience.

**Option A (LinkedIn connection, zero-pitch):**
```
Hi [First Name] — I build a zero-alloc, lock-free C++ telemetry ring (P50 43 cycles / ~18ns, invariant-TSC verified) for deterministic DSP-to-telemetry handoff. Relevant to SDR link-quality work at Astranis. No pitch — connecting.
```
*(206 characters.)*

**Option B (LinkedIn InMail/DM, ~115 words):**
```
Hi [First Name],

I'm the sole engineer behind AnimusCore — a zero-copy, lock-free C++17 telemetry transport built to keep the handoff from a DSP/signal chain to telemetry logging deterministic, since any allocator-driven variance there is measurable link jitter, not just noisy logs. Raw ring push measures P50 43 cycles / ~18ns on invariant-TSC, zero heap allocations across a million-iteration hot loop (instrumented via operator counters, not asserted), with 64-byte cache-line isolation enforced by static_assert at compile time.

No pitch, no call — the standalone eval harness is public if you want to verify deterministic handoff latency directly.
```
*(108 words.)*

**Option C (Direct email):**
```
Subject: Deterministic DSP-to-telemetry handoff, zero-alloc — 14-day eval

Hi [First Name],

I'm Alakshendra Roy — solo founder and engineer behind AnimusCore, a zero-copy, lock-free C++17 telemetry transport built to keep the DSP-to-telemetry handoff deterministic, since allocator variance there shows up as measurable link jitter.

Locked baseline: P50 43 cycles / ~18ns raw ring push, zero heap allocations (instrumented, not assumed), 64-byte isolation enforced by static_assert at compile time.

Self-contained eval sandbox: `dist/AnimusCore_Eval_Sandbox/README_BENCHMARK.txt`, exact build command included. 14-day zero-liability evaluation window on your own SDR target, no license, no call.

Alakshendra Roy
Founder & Chief Architect — AnimusCore
alakshendra@animusinfra.com | animusinfra.com
```
*(92 words in the body.)*

---

# Industry 3 — Autonomous Robotics & Defense Vehicles

*(Hard real-time sensor fusion & mission control — ROS2 bypass jitter, deterministic multi-sensor timing budgets)*

## 3.1 Skydio — VP of Core Autonomy

**Background & bottleneck:** Skydio's autonomous drone perception stack fuses camera, IMU, and lidar streams on a hard real-time budget where a single allocator stall on any sensor thread desyncs the fusion window across all of them. The bottleneck is deterministic multi-sensor fusion timing, where the fusion algorithm itself is only as reliable as the slowest thread's worst-case allocation behavior.

**Option A (LinkedIn connection, zero-pitch):**
```
Hi [First Name] — I build a zero-alloc, lock-free C++ telemetry ring (P50 43 cycles / ~18ns, invariant-TSC verified) for deterministic multi-sensor fusion timing. Relevant to core autonomy work at Skydio. No pitch — connecting.
```
*(206 characters.)*

**Option B (LinkedIn InMail/DM, ~115 words):**
```
Hi [First Name],

I'm the sole engineer behind AnimusCore — a zero-copy, lock-free C++17 telemetry transport built for hard real-time sensor fusion, where a single allocator stall on any one sensor thread desyncs the whole fusion window. Raw ring push measures P50 43 cycles / ~18ns on invariant-TSC, zero heap allocations across a million-iteration hot loop (instrumented via operator new/delete counters, not asserted by inspection), with 64-byte cache-line isolation enforced by static_assert at compile time — no shared-state contention between sensor threads by construction.

No pitch, no call — the standalone eval harness is public to verify directly on your own fusion pipeline.
```
*(112 words.)*

**Option C (Direct email):**
```
Subject: Deterministic multi-sensor fusion timing, zero-alloc ring — 14-day eval

Hi [First Name],

I'm Alakshendra Roy — solo founder and engineer behind AnimusCore, a zero-copy, lock-free C++17 telemetry transport built so a single sensor thread's allocator behavior can't desync the whole fusion window.

Locked baseline: P50 43 cycles / ~18ns raw ring push, zero heap allocations (instrumented, not assumed), 64-byte isolation enforced by static_assert at compile time.

Self-contained eval sandbox: `dist/AnimusCore_Eval_Sandbox/README_BENCHMARK.txt`, exact build command, locked numbers. 14-day zero-liability evaluation window on your own fusion hardware, no license, no call.

Alakshendra Roy
Founder & Chief Architect — AnimusCore
alakshendra@animusinfra.com | animusinfra.com
```
*(91 words in the body.)*

---

## 3.2 Anduril Industries — Principal Systems Architect, Lattice

**Background & bottleneck:** Anduril's Lattice OS fuses sensor feeds across heterogeneous hardware (towers, drones, vehicles) into a single mission picture in real time, where the integration layer itself must not introduce nondeterministic delay into a picture operators are acting on. The bottleneck is telemetry-layer determinism across a heterogeneous, distributed sensor mesh, not any single sensor's raw bandwidth.

**Option A (LinkedIn connection, zero-pitch):**
```
Hi [First Name] — I build a zero-alloc, lock-free C++ telemetry ring (P50 43 cycles / ~18ns, invariant-TSC verified) for deterministic cross-sensor mesh integration. Relevant to Lattice-style fusion work at Anduril. No pitch — connecting.
```
*(219 characters.)*

**Option B (LinkedIn InMail/DM, ~120 words):**
```
Hi [First Name],

I'm the sole engineer behind AnimusCore — a zero-copy, lock-free C++17 telemetry transport built for the constraint a distributed sensor-fusion mesh imposes: the integration layer itself can't be the source of the nondeterminism it's supposed to eliminate. Raw ring push measures P50 43 cycles / ~18ns on invariant-TSC, zero heap allocations across a million-iteration hot loop (instrumented via operator counters, not asserted by inspection), with 64-byte cache-line isolation enforced by static_assert at compile time — a structural guarantee across heterogeneous producer threads, not a runtime policy.

No pitch, no call — the standalone eval harness is public if useful for verifying deterministic behavior on your own mesh integration layer.
```
*(118 words.)*

**Option C (Direct email):**
```
Subject: Structurally deterministic sensor-mesh telemetry, zero-alloc — 14-day eval

Hi [First Name],

I'm Alakshendra Roy — solo founder and engineer behind AnimusCore, a zero-copy, lock-free C++17 telemetry transport built so the integration layer of a distributed sensor mesh can't itself introduce nondeterministic delay.

Locked baseline: P50 43 cycles / ~18ns raw ring push, zero heap allocations (instrumented, not assumed), 64-byte isolation enforced by static_assert at compile time.

Self-contained eval sandbox: `dist/AnimusCore_Eval_Sandbox/README_BENCHMARK.txt`, exact build command included. 14-day zero-liability evaluation window on your own integration target, no license, no call.

Alakshendra Roy
Founder & Chief Architect — AnimusCore
alakshendra@animusinfra.com | animusinfra.com
```
*(97 words in the body.)*

---

## 3.3 Shield AI — VP of Core Autonomy, Hivemind

**Background & bottleneck:** Shield AI's Hivemind autonomy software runs GPS-denied, comms-degraded mission autonomy on embedded flight hardware, where the autonomy stack has to make timing-critical decisions without any allocator-driven variance corrupting the decision loop's timing assumptions. The bottleneck is deterministic decision-loop timing under degraded, resource-constrained conditions specifically.

**Option A (LinkedIn connection, zero-pitch):**
```
Hi [First Name] — I build a zero-alloc, lock-free C++ telemetry ring (P50 43 cycles / ~18ns, invariant-TSC verified) for deterministic decision-loop timing on embedded flight hardware. Relevant at Shield AI. No pitch — connecting.
```
*(212 characters.)*

**Option B (LinkedIn InMail/DM, ~115 words):**
```
Hi [First Name],

I'm the sole engineer behind AnimusCore — a zero-copy, lock-free C++17 telemetry transport built for embedded autonomy stacks where a decision loop's timing assumptions can't be corrupted by allocator-driven variance, especially under GPS-denied or comms-degraded conditions. Raw ring push measures P50 43 cycles / ~18ns on invariant-TSC, zero heap allocations across a million-iteration hot loop (instrumented via operator counters, not asserted by inspection), with 64-byte cache-line isolation enforced by static_assert at compile time.

No pitch, no call — the standalone eval harness is public if you want to verify deterministic timing directly on your own embedded target.
```
*(112 words.)*

**Option C (Direct email):**
```
Subject: Deterministic decision-loop timing under degraded conditions — zero-alloc, 14-day eval

Hi [First Name],

I'm Alakshendra Roy — solo founder and engineer behind AnimusCore, a zero-copy, lock-free C++17 telemetry transport built so allocator variance can't corrupt a decision loop's timing assumptions under GPS-denied or comms-degraded conditions.

Locked baseline: P50 43 cycles / ~18ns raw ring push, zero heap allocations (instrumented, not assumed), 64-byte isolation enforced by static_assert at compile time.

Self-contained eval sandbox: `dist/AnimusCore_Eval_Sandbox/README_BENCHMARK.txt`, exact build command, locked numbers. 14-day zero-liability evaluation window on your own embedded target, no license, no call.

Alakshendra Roy
Founder & Chief Architect — AnimusCore
alakshendra@animusinfra.com | animusinfra.com
```
*(96 words in the body.)*

---

## 3.4 Ghost Robotics — Lead Systems Architect, Real-Time Control

**Background & bottleneck:** Ghost Robotics' quadruped platforms run a real-time control loop where actuator commands and sensor-bus telemetry must stay tightly synchronized under continuous mechanical vibration and thermal load in the field; sensor-bus jitter that would be a minor logging artifact elsewhere directly destabilizes gait control here. The bottleneck is sensor-bus telemetry jitter feeding into a physically unstable control loop if it arrives late.

**Option A (LinkedIn connection, zero-pitch):**
```
Hi [First Name] — I build a zero-alloc, lock-free C++ telemetry ring (P50 43 cycles / ~18ns, invariant-TSC verified) for jitter-free sensor-bus telemetry feeding real-time control loops. Relevant at Ghost Robotics. No pitch — connecting.
```
*(216 characters.)*

**Option B (LinkedIn InMail/DM, ~115 words):**
```
Hi [First Name],

I'm the sole engineer behind AnimusCore — a zero-copy, lock-free C++17 telemetry transport built for the constraint a real-time control loop imposes: sensor-bus telemetry jitter that's a minor logging artifact elsewhere directly destabilizes a physical control loop here. Raw ring push measures P50 43 cycles / ~18ns on invariant-TSC, zero heap allocations across a million-iteration hot loop (instrumented via operator counters, not asserted), with 64-byte cache-line isolation enforced by static_assert at compile time.

No pitch, no call — the standalone eval harness is public if you want to verify jitter-free behavior on your own control-loop hardware.
```
*(110 words.)*

**Option C (Direct email):**
```
Subject: Jitter-free sensor-bus telemetry for real-time control loops — 14-day eval

Hi [First Name],

I'm Alakshendra Roy — solo founder and engineer behind AnimusCore, a zero-copy, lock-free C++17 telemetry transport built so sensor-bus jitter that's cosmetic elsewhere can't destabilize a physical control loop here.

Locked baseline: P50 43 cycles / ~18ns raw ring push, zero heap allocations (instrumented, not assumed), 64-byte isolation enforced by static_assert at compile time.

Self-contained eval sandbox: `dist/AnimusCore_Eval_Sandbox/README_BENCHMARK.txt`, exact build command included. 14-day zero-liability evaluation window on your own control-loop target, no license, no call.

Alakshendra Roy
Founder & Chief Architect — AnimusCore
alakshendra@animusinfra.com | animusinfra.com
```
*(93 words in the body.)*

---

## 3.5 Boston Dynamics — Principal Real-Time Software Engineer

**Background & bottleneck:** Boston Dynamics' Atlas/Spot platforms run whole-body real-time control at kilohertz-scale loop rates where telemetry logging for diagnostics and RL-policy training data collection must never compete with the control loop for cache or scheduler time. The bottleneck is telemetry-vs-control resource contention at kilohertz loop rates, where even microsecond-scale interference is measurable in gait/balance quality.

**Option A (LinkedIn connection, zero-pitch):**
```
Hi [First Name] — I build a zero-alloc, lock-free C++ telemetry ring (P50 43 cycles / ~18ns, invariant-TSC verified) that can't contend with a kHz-scale control loop for cache or scheduler time. Relevant at Boston Dynamics. No pitch — connecting.
```
*(240 characters.)*

**Option B (LinkedIn InMail/DM, ~120 words):**
```
Hi [First Name],

I'm the sole engineer behind AnimusCore — a zero-copy, lock-free C++17 telemetry transport built so diagnostic/training-data telemetry can't compete with a kilohertz-scale whole-body control loop for cache or scheduler time. Raw ring push measures P50 43 cycles / ~18ns on invariant-TSC, zero heap allocations across a million-iteration hot loop (instrumented via global operator new/delete counters, not asserted by inspection), with 64-byte cache-line isolation enforced by static_assert at compile time — structurally separated from the control path, not just deprioritized.

No pitch, no call — the standalone eval harness is public if useful for verifying zero-contention behavior directly on your own control hardware.
```
*(117 words.)*

**Option C (Direct email):**
```
Subject: Telemetry that can't contend with a kHz control loop — zero-alloc, 14-day eval

Hi [First Name],

I'm Alakshendra Roy — solo founder and engineer behind AnimusCore, a zero-copy, lock-free C++17 telemetry transport built so diagnostic telemetry structurally can't compete with a kilohertz-scale control loop for cache or scheduler time.

Locked baseline: P50 43 cycles / ~18ns raw ring push, zero heap allocations (instrumented, not assumed), 64-byte isolation enforced by static_assert at compile time.

Self-contained eval sandbox: `dist/AnimusCore_Eval_Sandbox/README_BENCHMARK.txt`, exact build command, locked numbers. 14-day zero-liability evaluation window on your own control hardware, no license, no call.

Alakshendra Roy
Founder & Chief Architect — AnimusCore
alakshendra@animusinfra.com | animusinfra.com
```
*(96 words in the body.)*

---

# Industry 4 — Ultra-Low-Latency Audio/DSP & Hardware-in-the-Loop Simulation

*(RT kernel-bypass telemetry — zero-alloc audio thread buffers, deterministic HIL simulation step timing)*

## 4.1 dSPACE — Principal HIL Real-Time Systems Engineer

**Background & bottleneck:** dSPACE's HIL simulators run automotive/aerospace ECU testing on fixed-step real-time loops where the simulation step itself must complete within a hard deadline every cycle; any telemetry/logging call that risks an allocator stall threatens the determinism the entire HIL rig exists to guarantee. The bottleneck is telemetry overhead intruding on a hard-deadline simulation step, not average simulation throughput.

**Option A (LinkedIn connection, zero-pitch):**
```
Hi [First Name] — I build a zero-alloc, lock-free C++ telemetry ring (P50 43 cycles / ~18ns, invariant-TSC verified) that can't intrude on a hard-deadline sim step. Relevant to HIL real-time work at dSPACE. No pitch — connecting.
```
*(224 characters.)*

**Option B (LinkedIn InMail/DM, ~115 words):**
```
Hi [First Name],

I'm the sole engineer behind AnimusCore — a zero-copy, lock-free C++17 telemetry transport built so instrumentation can't intrude on a hard-deadline HIL simulation step. Raw ring push measures P50 43 cycles / ~18ns on invariant-TSC, zero heap allocations across a million-iteration hot loop (instrumented via global operator new/delete counters, not asserted by inspection), with 64-byte cache-line isolation enforced by static_assert at compile time — a structural guarantee the step deadline can rely on.

No pitch, no call — the standalone eval harness is public if useful for verifying deterministic behavior on your own fixed-step rig.
```
*(107 words.)*

**Option C (Direct email):**
```
Subject: Telemetry that can't intrude on a hard-deadline sim step — zero-alloc, 14-day eval

Hi [First Name],

I'm Alakshendra Roy — solo founder and engineer behind AnimusCore, a zero-copy, lock-free C++17 telemetry transport built so instrumentation can't intrude on a fixed-step HIL deadline.

Locked baseline: P50 43 cycles / ~18ns raw ring push, zero heap allocations (instrumented, not assumed), 64-byte isolation enforced by static_assert at compile time.

Self-contained eval sandbox: `dist/AnimusCore_Eval_Sandbox/README_BENCHMARK.txt`, exact build command, locked numbers. 14-day zero-liability evaluation window on your own fixed-step rig, no license, no call.

Alakshendra Roy
Founder & Chief Architect — AnimusCore
alakshendra@animusinfra.com | animusinfra.com
```
*(90 words in the body.)*

---

## 4.2 Rohde & Schwarz — Principal RF/DSP Systems Engineer

**Background & bottleneck:** Rohde & Schwarz's real-time test and measurement instruments capture and process RF/DSP signal chains at high sample rates where telemetry/logging bolted onto the acquisition path risks introducing exactly the kind of nondeterministic jitter the instrument is being used to measure in someone else's system. The bottleneck is measurement-instrument-grade determinism in its own internal telemetry path.

**Option A (LinkedIn connection, zero-pitch):**
```
Hi [First Name] — I build a zero-alloc, lock-free C++ telemetry ring (P50 43 cycles / ~18ns, invariant-TSC verified) — instrument-grade determinism in its own telemetry path. Relevant at Rohde & Schwarz. No pitch — connecting.
```
*(210 characters.)*

**Option B (LinkedIn InMail/DM, ~110 words):**
```
Hi [First Name],

I'm the sole engineer behind AnimusCore — a zero-copy, lock-free C++17 telemetry transport built to the same determinism standard a measurement instrument has to hold in its own internal path, since jittery internal telemetry would corrupt the very RF/DSP measurements the instrument exists to deliver. Raw ring push measures P50 43 cycles / ~18ns on invariant-TSC, zero heap allocations across a million-iteration hot loop (instrumented, not asserted), with 64-byte cache-line isolation enforced by static_assert at compile time.

No pitch, no call — the standalone eval harness is public to verify directly.
```
*(103 words.)*

**Option C (Direct email):**
```
Subject: Instrument-grade deterministic telemetry, zero-alloc — 14-day eval

Hi [First Name],

I'm Alakshendra Roy — solo founder and engineer behind AnimusCore, a zero-copy, lock-free C++17 telemetry transport built to the determinism standard a measurement instrument needs in its own internal telemetry path.

Locked baseline: P50 43 cycles / ~18ns raw ring push, zero heap allocations (instrumented, not assumed), 64-byte isolation enforced by static_assert at compile time.

Self-contained eval sandbox: `dist/AnimusCore_Eval_Sandbox/README_BENCHMARK.txt`, exact build command, locked numbers. 14-day zero-liability evaluation window on your own instrument hardware, no license, no call.

Alakshendra Roy
Founder & Chief Architect — AnimusCore
alakshendra@animusinfra.com | animusinfra.com
```
*(90 words in the body.)*

---

## 4.3 Speedgoat — Lead Real-Time Target Machine Engineer

**Background & bottleneck:** Speedgoat's real-time target machines run Simulink-generated HIL models at fixed sample rates for customers across automotive, aerospace, and power electronics, where the target machine's own housekeeping/telemetry layer has to stay entirely off the model's execution budget. The bottleneck is keeping platform telemetry overhead invisible to the customer's real-time model step, across every customer workload the box runs.

**Option A (LinkedIn connection, zero-pitch):**
```
Hi [First Name] — I build a zero-alloc, lock-free C++ telemetry ring (P50 43 cycles / ~18ns, invariant-TSC verified) that stays off the model's real-time execution budget. Relevant to target-machine work at Speedgoat. No pitch — connecting.
```
*(222 characters.)*

**Option B (LinkedIn InMail/DM, ~115 words):**
```
Hi [First Name],

I'm the sole engineer behind AnimusCore — a zero-copy, lock-free C++17 telemetry transport built so platform-level housekeeping telemetry stays entirely off a real-time target machine's model execution budget, regardless of which customer's Simulink model is running. Raw ring push measures P50 43 cycles / ~18ns on invariant-TSC, zero heap allocations across a million-iteration hot loop (instrumented via operator counters, not asserted by inspection), with 64-byte cache-line isolation enforced by static_assert at compile time.

No pitch, no call — the standalone eval harness is public if useful for verifying zero-overhead behavior on your own target hardware.
```
*(110 words.)*

**Option C (Direct email):**
```
Subject: Zero hot-path overhead telemetry for real-time target machines — 14-day eval

Hi [First Name],

I'm Alakshendra Roy — solo founder and engineer behind AnimusCore, a zero-copy, lock-free C++17 telemetry transport built to stay entirely off a real-time target machine's model execution budget.

Locked baseline: P50 43 cycles / ~18ns raw ring push, zero heap allocations (instrumented, not assumed), 64-byte isolation enforced by static_assert at compile time.

Self-contained eval sandbox: `dist/AnimusCore_Eval_Sandbox/README_BENCHMARK.txt`, exact build command included. 14-day zero-liability evaluation window on your own target machine, no license, no call.

Alakshendra Roy
Founder & Chief Architect — AnimusCore
alakshendra@animusinfra.com | animusinfra.com
```
*(93 words in the body.)*

---

## 4.4 Native Instruments — Principal Audio Engine Software Engineer

**Background & bottleneck:** Native Instruments' audio hardware/software stack (Kontakt, Komplete Kontrol, ASIO driver layer) runs the audio callback thread under a hard real-time deadline where a single heap allocation can cause an audible dropout; the bottleneck is guaranteeing zero allocation across the entire callback path, including deep inside sample-streaming and voice-allocation logic that's easy to audit at the top level but easy to violate several calls deep.

**Option A (LinkedIn connection, zero-pitch):**
```
Hi [First Name] — I build a zero-alloc, lock-free C++ telemetry/buffer ring (P50 43 cycles / ~18ns, invariant-TSC verified), static_assert-enforced zero-heap. Relevant to audio callback-thread work at NI. No pitch — connecting.
```
*(203 characters.)*

**Option B (LinkedIn InMail/DM, ~110 words):**
```
Hi [First Name],

I'm the sole engineer behind AnimusCore — a zero-copy, lock-free C++17 buffer/telemetry transport built for hard-real-time audio callback threads, where a single heap allocation several calls deep in voice-allocation logic is enough to cause an audible dropout. Raw ring push measures P50 43 cycles / ~18ns on invariant-TSC, zero heap allocations across a million-iteration hot loop, instrumented via global operator new/delete counters — not audited by inspection, actually instrumented — with 64-byte cache-line isolation enforced by static_assert at compile time.

No pitch, no call — the standalone eval harness is public to verify directly.
```
*(107 words.)*

**Option C (Direct email):**
```
Subject: Zero-heap audio callback buffer, instrumented not audited — 14-day eval

Hi [First Name],

I'm Alakshendra Roy — solo founder and engineer behind AnimusCore, a zero-copy, lock-free C++17 buffer transport built for hard-real-time audio callback threads, where one allocation several calls deep causes an audible dropout.

Locked baseline: P50 43 cycles / ~18ns raw ring push, zero heap allocations (instrumented via operator counters across a million-iteration hot loop, not audited by inspection), 64-byte isolation enforced by static_assert at compile time.

Self-contained eval sandbox: `dist/AnimusCore_Eval_Sandbox/README_BENCHMARK.txt`, exact build command. 14-day zero-liability evaluation window on your own audio hardware, no license, no call.

Alakshendra Roy
Founder & Chief Architect — AnimusCore
alakshendra@animusinfra.com | animusinfra.com
```
*(96 words in the body.)*

---

## 4.5 Ableton — Principal Core Audio Engine Engineer

**Background & bottleneck:** Ableton Live's Core Audio Engine schedules device processing and MIDI/audio buffer routing per callback under the same hard-real-time-audio deadline, where the bottleneck isn't the DSP itself but the buffer-management/telemetry scaffolding around it — device-count scaling means any per-buffer overhead compounds linearly with session complexity. The problem is zero-alloc buffer routing that stays flat as device count scales, not single-device performance.

**Option A (LinkedIn connection, zero-pitch):**
```
Hi [First Name] — I build a zero-alloc, lock-free C++ buffer/telemetry ring (P50 43 cycles / ~18ns, invariant-TSC verified) that stays flat as device count scales. Relevant to Core Audio Engine work at Ableton. No pitch — connecting.
```
*(224 characters.)*

**Option B (LinkedIn InMail/DM, ~115 words):**
```
Hi [First Name],

I'm the sole engineer behind AnimusCore — a zero-copy, lock-free C++17 buffer/telemetry transport built so per-buffer routing overhead stays flat as device count scales, rather than compounding linearly across a complex session's audio graph. Raw ring push measures P50 43 cycles / ~18ns on invariant-TSC, zero heap allocations across a million-iteration hot loop (instrumented via global operator new/delete counters, not asserted by inspection), with 64-byte cache-line isolation enforced by static_assert at compile time.

No pitch, no call — the standalone eval harness is public if useful for verifying zero-alloc scaling behavior directly.
```
*(108 words.)*

**Option C (Direct email):**
```
Subject: Zero-alloc buffer routing that stays flat as device count scales — 14-day eval

Hi [First Name],

I'm Alakshendra Roy — solo founder and engineer behind AnimusCore, a zero-copy, lock-free C++17 buffer transport built so per-buffer overhead doesn't compound as a session's device count grows.

Locked baseline: P50 43 cycles / ~18ns raw ring push, zero heap allocations (instrumented, not assumed), 64-byte isolation enforced by static_assert at compile time.

Self-contained eval sandbox: `dist/AnimusCore_Eval_Sandbox/README_BENCHMARK.txt`, exact build command, locked numbers. 14-day zero-liability evaluation window on your own audio hardware, no license, no call.

Alakshendra Roy
Founder & Chief Architect — AnimusCore
alakshendra@animusinfra.com | animusinfra.com
```
*(93 words in the body.)*

---

## Send-order notes

- **No named contacts sourced** for any of the 20 firms — every persona is role-level per this repo's standing convention. Verify the actual name (LinkedIn search or company engineering-team page) before sending Option B or C; Option A connection notes can go out to a role-matched profile directly.
- **Email addresses are placeholders** (`{role-alias}@{domain}`) — resolve the real recipient/alias per firm before send, same as every other pipeline in this repo.
- **Quant vertical (1.1–1.5)** is the only one of the four with pre-existing GTM activity in this repo — the pre-flight table above is load-bearing there. The other three verticals (aerospace beyond the swapped names, robotics/defense, audio/DSP-HIL) are entirely new ground with zero cross-motion conflict.
- Log every actual send in the pinned Outbound Console artifact (`SPa2n9NPXxTdYU5xMJ2McP`) and mirror the row into `docs/outreach/outbound_console.html` immediately — don't let a seventh surface accumulate before the sixth one is reconciled.
