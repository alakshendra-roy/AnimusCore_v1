# Outreach Queue — 2026-09-28

Copy-paste ready. Two archetypes, three channel variants each. Read the pre-flight table first — three of the six named prop-shop accounts have a live conflict or channel block against the existing four GTM motions tracked in `docs/outreach/outbound_console.html`.

---

## ⚠️ Pre-flight status check — read before sending

Cross-checked against `docs/outreach/outbound_console.html` (the live tracker across all four existing motions) on 2026-09-27.

| Firm | Persona on file | Status | Channel to use tomorrow | Why |
|---|---|---|---|---|
| **Tower Research Capital (Global)** | CEO Albert An / Head of Trading Technology | **Already contacted 2026-09-23** (both the Python-bridge and raw-engine motions) | **Skip.** Do not re-open cold. | Sending a fresh cold open now reads as scattershot against an active 5-day-old thread. If you want a second touch, follow up on the existing thread instead of using this queue's Option A/B/C as a new cold open. `Tower Research Capital India` (the India engineering site) is a distinct, uncontacted entity if you want a legitimate fresh angle there. |
| **AlphaGrep** | Head of Trading Technology | Ready, not yet sent | Option C (Direct Email) | Listed on **two** trackers (raw-engine pipeline + Indian domestic pipeline, row IN-A3) with the same real firm. Send once, log in both places, don't fire from this queue *and* those. |
| **Quadeye** | Head of Execution Infrastructure | Ready, not yet sent | Option C (Direct Email / Gmail) | Same duplicate risk as AlphaGrep (raw-engine pipeline + Indian pipeline row IN-A4). Also switched off LinkedIn InMail on 2026-09-25 (quota — see below), so Option B is not currently usable for this one. |
| **Wintermute** | Head of Trading Technology | Ready, not yet sent | Option C (Direct Email / Gmail) | Switched from LinkedIn InMail to direct Gmail email 2026-09-25 (quota). No named contact on file — verify the recipient before send. |
| **Flow Traders** | Lead Low-Latency C++ Engineer | Ready, not yet sent | Option C (Direct Email / Gmail) | Same as Wintermute — InMail-blocked, switched to Gmail 2026-09-25. No named contact on file — verify before send. |
| **NK Securities** | Lead Low-Latency C++ Engineer | Ready, not yet sent, no cross-motion duplicate | Option A or B (LinkedIn) or C (email) — cleanest of the six | The one genuinely clean send in this batch of six. |

**LinkedIn InMail quota:** hit its weekly limit 2026-09-25; no confirmation as of this writing (2026-09-27) that it has reset. Treat Option B below as **connection-request-note-only (Option A) or hold** for Wintermute, Flow Traders, and Quadeye specifically until the quota is confirmed reset — send those three via Option C instead. NK Securities and any other InMail-channel firm not on this list are unaffected.

**After any actual send:** log it in the pinned Outbound Console artifact (`SPa2n9NPXxTdYU5xMJ2McP`) and update the matching row in `docs/outreach/outbound_console.html` — this queue is a drafting surface, not a tracker of record.

---

## Archetype 1 — Quant Dev / Low-Latency Infra Lead (Prop Shops & Market Makers)

Reference accounts: Tower Research *(skip — see above)*, AlphaGrep, Wintermute, Flow Traders, NK Securities, Quadeye.

### Option A — LinkedIn Connection Request Note (zero-pitch)

```
Hi [First Name] — I build a zero-alloc, lock-free C++ telemetry/execution engine (sub-50-cycle ring push, invariant-TSC verified). Always good to compare notes with people solving the same tail-latency problems at [Company]. No pitch — connecting.
```
*(247 characters — under the 300-char limit.)*

### Option B — LinkedIn InMail / Direct Message

```
Hi [First Name],

I'm the sole engineer behind AnimusCore — a zero-copy, lock-free C++17 telemetry/execution transport. Raw ring push (ShmRing<T>::try_push()) measures P50 43 cycles / ~18ns on invariant-TSC, lfence-serialized RDTSC — zero heap allocations across a million-iteration hot loop, instrumented with global operator new/delete counters, not asserted by inspection. 64-byte cache-line isolation is enforced at compile time via static_assert on the ring header and padding types, not just documented.

No pitch, no call — the standalone eval harness (source + exact build command) is public if you want to verify these numbers on your own hardware in under ten minutes. Worth a look if allocator noise or tail-latency jitter on the hot path is a live problem at [Company].
```
*(119 words.)*

### Option C — Direct Technical Email / Gmail

```
Subject: P50 43 cycles / ~18ns raw ring push — verify it yourself, no call needed

Hi [First Name],

I'm Alakshendra Roy — solo founder and engineer behind AnimusCore, a zero-copy, lock-free C++17 telemetry/execution transport.

Locked, reproducible baseline on the raw ring push path: P50 43 cycles / ~18ns on invariant-TSC, zero heap allocations (instrumented, not assumed), 64-byte cache-line isolation enforced by static_assert at compile time — full methodology in the Technical Verification Dossier.

Standalone eval harness: `dist/AnimusCore_Eval_Sandbox/` in the public repo. `README_BENCHMARK.txt` has the exact build command (`g++ -O3 -march=native -DNDEBUG`, no dependencies beyond a C++17 toolchain) and the locked numbers. Clone it, build it, run it on your own bare metal — five minutes, no license, no call.

If it holds on your hardware and this is a live problem for your team, I'm easy to reach directly.

Alakshendra Roy
Founder & Chief Architect — AnimusCore
alakshendra@animusinfra.com | animusinfra.com
```
*(124 words in the body, excluding subject and signature block.)*

---

## Archetype 2 — Real-Time Aerospace / Avionics Edge Infrastructure (satellite/UAV telemetry)

Note: the Indian aerospace/defense/spacetech accounts (HAL, BEL, DRDO/ADE, Skyroot, Agnikul Cosmos, TASL, L&T Precision Engineering, BDL, Data Patterns India, Dhruva Space, Pixxel Space) already have their own tracked motion — see `docs/outreach/indian_enterprise_pipeline.md`. Use this archetype for global/other avionics and satellite/UAV telemetry leads not already on that list, not as a second angle into those ten.

### Option A — LinkedIn Connection Request Note (zero-pitch)

```
Hi [First Name] — I build a zero-alloc, lock-free C++ telemetry transport (sub-50-cycle ring push, invariant-TSC verified), originally for HFT but maps directly onto satellite/UAV telemetry pipelines. Glad to compare notes on real-time edge infra. No pitch — connecting.
```
*(270 characters — under the 300-char limit.)*

### Option B — LinkedIn InMail / Direct Message

```
Hi [First Name],

I'm the sole engineer behind AnimusCore — a zero-copy, lock-free C++17 telemetry transport built under the same constraints avionics and satellite/UAV telemetry pipelines run under: no heap allocation, no OS-scheduler dependency, deterministic cache-line behavior. Raw ring push measures P50 43 cycles / ~18ns on invariant-TSC, zero heap allocations across a million-iteration hot loop (instrumented with operator new/delete counters, not asserted), with 64-byte cache-line isolation enforced at compile time via static_assert.

No pitch, no call — the standalone eval harness (source + build instructions) is public if you want to verify these numbers directly on your own target hardware. Worth a look if a telemetry/logging layer bolted onto your control loop is introducing nondeterministic jitter at [Company].
```
*(119 words.)*

### Option C — Direct Technical Email / Gmail

```
Subject: Zero-alloc C++ telemetry transport — P50 43 cycles / ~18ns, verify on your own hardware

Hi [First Name],

I'm Alakshendra Roy — solo founder and engineer behind AnimusCore, a zero-copy, lock-free C++17 telemetry transport. Built originally for HFT tail-latency, the same constraints — no heap allocation, no OS-scheduler dependency, deterministic cache-line behavior — apply directly to satellite/UAV telemetry and avionics edge pipelines.

Locked baseline: P50 43 cycles / ~18ns raw ring push on invariant-TSC, zero heap allocations (instrumented, not assumed), 64-byte cache-line isolation enforced by static_assert at compile time — full methodology in the Technical Verification Dossier.

Standalone eval harness: `dist/AnimusCore_Eval_Sandbox/` in the public repo. `README_BENCHMARK.txt` has the exact build command (`g++ -O3 -march=native -DNDEBUG`) and the locked numbers. Build and run it on your own target hardware directly — no license, no call required.

If it holds under your own workload, happy to go deeper directly.

Alakshendra Roy
Founder & Chief Architect — AnimusCore
alakshendra@animusinfra.com | animusinfra.com
```
*(132 words in the body, excluding subject and signature block.)*

---

## Guardrails applied

- **Voice:** first-person singular throughout ("I," never "we"/"our team") — consistent with the sole-operator positioning; no implied team, no SLA language a solo founder can't back.
- **Zero corporate filler, zero call-begging:** no "let me know if you want to hop on a call" anywhere. Every message points to something the recipient can do *themselves*, async, with no dependency on the founder's calendar.
- **Numbers cited are the locked, verified baseline** from `dist/AnimusCore_Eval_Sandbox/README_BENCHMARK.txt` and cross-checked against `tests/eval_harness.cpp` on 2026-09-27 (P50 43 cycles / ~18ns, min 39 / P90 47, zero heap allocations, invariant-TSC, static_assert-enforced 64-byte cache-line alignment). This is the **raw ring push** number specifically — deliberately not blended with the separate integration-level throughput figures (16.5M+ burst pushes/sec, ~100ns tick-to-telemetry) that `benchmark_harness.cpp`/`telemetry_benchmark.cpp` measure under different concurrency; the Verification Dossier's own §3 keeps these apart for the same reason.
- **Supporting reference for anyone who wants deeper methodology:** `docs/marketing_whitepapers/AnimusCore_Technical_Verification_Dossier.md` (or `.pdf`) — not linked inline in every short message to keep word counts tight, but named directly in both Option C emails.
