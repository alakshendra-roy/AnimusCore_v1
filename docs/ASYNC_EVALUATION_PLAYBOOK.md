# Animus Core — Async Evaluation Playbook (No-Meeting Sales Track)

> ## ⚠️ INDICATIVE PLAYBOOK — PRICING NOT COUNSEL-REVIEWED
> This document sets internal process and email/slide *copy*. Any dollar figure quoted here (including the **Desk License** tier introduced in §3) is indicative until a licensed attorney has reviewed pricing/contract exposure per `PILOT_READINESS_TODO.md`'s Blocking item (GitHub issue #7) — same standing caveat as `ANIMUS_ENTERPRISE_TERMSHEET.md` and `PILOT_CONTRACT.md`. Do not treat a number in this file as binding until it also exists in a signed order form or the definitive MSLA.

**Purpose:** a fully written, asynchronous path from "technical sponsor wants to evaluate" to "signed Desk License," with **zero required live meetings**. Built for a solo-operator sales motion where the founder is the sole technical and commercial point of contact and prefers written channels over live video.

**Audience:** you (Alakshendra), running this process — and, where noted, prospective technical sponsors reading the emails/FAQ this playbook produces.

**Companion documents:** [`../Pilot_Kit/PILOT_README.md`](../Pilot_Kit/PILOT_README.md) (Windows self-serve kit — the trial vehicle for §1) · [`../eval_kit/README.md`](../eval_kit/README.md) (Linux turnkey tarball — alternate/parallel trial vehicle) · [`PILOT_EVAL_CHECKLIST.md`](PILOT_EVAL_CHECKLIST.md) (the technical gate the roadmap below walks through) · [`../BENCHMARK_DATASHEET.md`](../BENCHMARK_DATASHEET.md) (reference figures cited throughout) · [`PILOT_PROGRAM.md`](PILOT_PROGRAM.md) (the paid, deeper 4-week engagement this playbook can *optionally* upsell into — not required) · [`ANIMUS_ENTERPRISE_TERMSHEET.md`](ANIMUS_ENTERPRISE_TERMSHEET.md) (the existing $240k/yr Institutional tier — see §3 for how the new Desk License tier relates to it).

---

## 0. Why this track exists, and where it sits relative to what's already in this repo

This repo already has a paid, four-week, high-touch Proof-of-Performance track (`PILOT_PROGRAM.md`) that assumes "direct founder-level engineering engagement" and a Week 4 "joint review." That track is real and should stay as-is for desks that ask for it or that a deal naturally grows into.

**This playbook is a different, lower-friction front door**, built on the *free* self-serve kits that already exist (`Pilot_Kit/`, `eval_kit/`) plus `PILOT_EVAL_CHECKLIST.md`'s existing verification gates. It assumes:

- The prospect is a hands-on systems engineer who would rather read a datasheet and run a benchmark than sit on a call.
- Every checkpoint is a written artifact (an email reply, a checklist sign-off, a JSON results file, a GitHub Discussion post) — never a status update that only exists in someone's memory of a call.
- A live meeting is a fallback for a stuck deal, not a required step. §4 is what to do if one gets requested anyway.

---

## 1. The 30-Day Evaluation Roadmap

**Channels used, in order of preference:** email (primary) → a private Slack Connect channel if the prospect's org uses Slack and asks for one → a GitHub Discussion thread in a scoped eval repo, if you're comfortable granting read access to one. Never default to scheduling a call to answer a question that fits in three paragraphs of writing.

**Trial vehicle:** issue whichever kit matches their platform — `Pilot_Kit/` (Windows, `AnimusNative.dll`, hardware-locked 30-day `.lic` via the existing `get_fingerprint.ps1` → `generate_license.py` handoff) or `eval_kit/` (Linux x86_64 turnkey tarball, no license needed for the shared-memory transport layer). Both are already production-ready deliverables — this roadmap just sequences how a sponsor works through them without you on a call.

### Week 1 — Install, License, First Green Run

**Goal:** sponsor has a compiled binary running on their own hardware and one verified ingestion pass, unassisted.

| Day | Client does | You do | Written exit artifact |
|---|---|---|---|
| 1 | Requests trial | Send the **Day-1 Delivery Email** (§2.1) with binary/tarball, `PILOT_README.md` or `eval_kit/README.md`, and this repo's FAQ (§2 below) attached | Delivery email sent |
| 1–2 | Runs `get_fingerprint.ps1` (Windows) or unpacks the tarball (Linux); sends fingerprint if Windows | Issue `.lic` via `generate_license.py`, reply within 1 business day | Signed `.lic` file + reply email |
| 3–5 | Runs `animus_integration_example.py` or `./run_demo.sh`; gets one clean pass | — (no action needed unless they hit an error) | Client's own console output / first PASS verdict |
| 5–7 | Replies confirming the pass, or posts a blocker | Answer any written question same-day if possible, next business day at latest | A written "Week 1 done" confirmation from the client — this is the gate before Week 2, don't let it slide unconfirmed |

**Exit criterion:** one client-run, client-witnessed green pass, with the client having *said so in writing* — not you inferring it from silence.

### Week 2 — Host Tuning & Threshold Verification

**Goal:** sponsor independently works through `PILOT_EVAL_CHECKLIST.md` §1–3 (hardware prerequisites, CPU isolation / `taskset` pinning, the air-gapped verification runbook) and checks their numbers against the published acceptance thresholds.

- Client works the checklist solo — it's written to be self-contained, that's the point.
- Client posts results (or a specific blocker, with the actual error/output, not "it's not working") to the written channel.
- You respond in writing, same cadence as Week 1. If a blocker is genuinely hardware- or kernel-specific, the **Troubleshooting & Differential Diagnosis Matrix** in `PILOT_EVAL_CHECKLIST.md` §4 covers the overwhelming majority of real cases — link the exact row rather than re-explaining it.

**Exit criterion:** `PILOT_EVAL_CHECKLIST.md` §5 sign-off table, filled in and sent back (email attachment or pasted into the thread), with a PASS/FAIL verdict per section.

### Day 14 — Async Check-In (not a call)

Send the **Day-14 Check-In Email** (§2.2) regardless of where they are in Week 2 — proactively, don't wait to be asked. Its only job is to surface a stuck technical bottleneck in writing before it quietly stalls the trial into week 3 and 4 with no signal. If they reply with a blocker, treat it exactly like any Week 2 blocker — written diagnosis, written fix, no call offered as the first move.

### Week 3 — Own-Data / Own-Workload Integration

**Goal:** the self-serve version of what the paid `PILOT_PROGRAM.md` does in its Week 1 with joint engineering time — here, the client does it themselves, against their own historical or synthetic replay data (still non-production; no live feed, no order path access, same non-production boundary `14_DAY_POC_AGREEMENT.md` §1's Recitals already draws for the paid track).

- Client adapts `Pilot_Kit/animus_integration_example.py` (or their own harness against the public C-ABI) to their own event shapes and sizes.
- Client runs their own correctness + throughput pass and records numbers specific to their event shapes — not the synthetic 64-byte payloads in `benchmarks/telemetry_benchmark.cpp`.
- You offer an async code-review pass if they paste or attach their integration snippet — this is real technical value delivered in writing, and it's the single highest-leverage thing you can do in this track without a call.

**Exit criterion:** a short written integration report from the client — what they measured, on what event shape, on what hardware. It doesn't need to be formal; a paragraph in an email is enough.

### Week 4 — Decision Week

| Day | Action |
|---|---|
| 22–24 | Client finalizes their internal write-up. No action from you unless asked. |
| 25 | Send the **Day-25 Commercial Transition Email** (§2.3) — moves the conversation from technical validation to commercial terms, in writing. |
| 26–28 | Client responds with questions, a redline, or a decision — all in writing. Answer commercial questions the same way you answered technical ones: same-day where possible, no call as the default next step. |
| 29 | If no response, one short written nudge — reuse the tone of `OUTREACH_TEMPLATES.md`'s Day-5/Day-12 follow-ups, not a new pressure tactic. |
| 30 | Trial `.lic` expires. Renewal/extension only happens as part of a paid engagement — don't quietly re-issue a free 30-day license as a substitute for closing. |

**Exit criterion:** either a signed order form for the Desk License (§3), an escalation into `PILOT_PROGRAM.md`'s paid 4-week track for a desk that wants deeper joint validation before committing, or a clean, specific written "no" you can log and move on from.

---

## 2. No-Meeting Email Templates

Signature block for all three (matches `OUTREACH_TEMPLATES.md`'s existing convention — keep it identical, don't invent a second signature style):

```
Alakshendra Roy
Founder & Chief Architect — Animus Core
alakshendra@animusinfra.com | +91 9891161189
animusinfra.com
```

### 2.1 Day-1 — Trial Delivery Email

**Subject:** `Your Animus Core 30-day evaluation — binary, docs, and where to start`

> Hi [First Name],
>
> Here's everything for your 30-day evaluation — no call needed to get started, everything below is self-contained.
>
> **Attached / linked:**
> - [Windows] The Pilot Kit (`Pilot_Kit/`) — `PILOT_README.md` is the only doc you need to get a first real ingestion call running end to end.
> - [Linux] `animus-eval-kit-linux-x86_64.tar.gz` — turnkey, prebuilt, `./run_demo.sh` gets you a pass/fail verdict in under three minutes, no compiler needed.
> - `BENCHMARK_DATASHEET.md` — every figure I'll reference is measured and reproducible from source; the commands to reproduce each one are in the doc itself, not just asserted.
> - The attached Technical FAQ — the plain-English version of ring buffers, cache alignment, and core pinning, written so you don't have to take my word for any of it.
>
> [Windows only] To get your license: run `get_fingerprint.ps1` (read-only, no network call, safe to run) and reply with the fingerprint it prints. I'll issue a 30-day hardware-locked `.lic` and send it back same day.
>
> **How this works over the next 30 days:** everything's async. I'll check in at Day 14 regardless of where you are, and again around Day 25 once you've had a chance to see real numbers. If something's broken or a number looks off, reply with the actual console output and I'll get back to you same-day — no need to schedule anything to get an answer.
>
> `PILOT_EVAL_CHECKLIST.md` in the repo is the full verification runbook if you want to go straight to host tuning and threshold checks — it's written to be run solo.
>
> [Your Name]

### 2.2 Day-14 — Async Check-In Email

**Subject:** `Day 14 — how's the eval going, and what's blocking you (if anything)`

> Hi [First Name],
>
> Two weeks in — wanted to check in before this quietly stalls on something small. A few honest options, just reply with whichever's true:
>
> 1. **On track** — you've got a clean run and are working through `PILOT_EVAL_CHECKLIST.md`'s host tuning section. No action needed from me, just good to know.
> 2. **Stuck on something specific** — paste the actual error or output and I'll get back to you same-day. `PILOT_EVAL_CHECKLIST.md` §4 has a troubleshooting matrix that covers most environment issues (core isolation, `/dev/shm` permissions, wheel/Python ABI mismatches) if you want to check there first.
> 3. **Haven't had time yet** — totally fine, no pressure. Let me know roughly when you expect to pick it back up so I'm not guessing.
> 4. **Numbers don't look right** — send me your `producer_report.json` / `verify_stream.py` output (or the Pilot Kit console output) and I'll take a look. This is exactly the kind of thing worth flagging even mid-eval, not just at the end.
>
> Whatever the answer, no need to hop on a call for it — happy to go a few rounds in writing if something's genuinely gnarly.
>
> [Your Name]

### 2.3 Day-25 — Commercial Transition Email

**Subject:** `Day 25 — where this can go next`

> Hi [First Name],
>
> You're coming up on the end of the 30-day window, so here's where things stand commercially, entirely in writing — no pressure to jump on a call to talk pricing.
>
> **If your numbers cleared what you needed:** the next step is a **Desk License** — $30,000–$60,000/year depending on core-count and integration scope (single strategy / single production node; final number scoped from what you tell me about your deployment footprint), compiled binary, quarterly maintenance updates, and direct email support from me. I can send a short written scope questionnaire — three or four questions about your deployment — and turn around an exact quote within a day or two of your answers. No live pricing negotiation needed to get a real number.
>
> **If you need deeper validation before committing** — your own historical data replayed at scale, live shadow-mode comparison against your current system, or a hardware-specific tail-latency report with my direct involvement — that's `PILOT_PROGRAM.md`'s four-week paid Proof-of-Performance track. It's a bigger commitment (scoped fee, credited toward year one if you convert), but it ends in a signed-off report either way, not just a sales pitch.
>
> **If it's a no for now** — genuinely fine, and I'd rather hear the specific reason than a vague "not right now." If it's a numbers gap, a missing feature, or bad timing, tell me which — it's useful to me even when the answer's no.
>
> Either way, reply whenever works — nothing here needs a call to move forward.
>
> [Your Name]

---

## 3. The Desk License — how it relates to the existing Enterprise tier

This playbook introduces a **Desk License** ($30k–$60k/year, indicative) as a narrower, lower-commitment paid tier than the existing $240k/year Institutional Enterprise tier in `ANIMUS_ENTERPRISE_TERMSHEET.md`. It is **not** a discount on that tier — it's a smaller scope, so it needs its own boundary, not just a smaller number stapled to the same terms:

| | Desk License (new, this playbook) | Institutional Enterprise (`ANIMUS_ENTERPRISE_TERMSHEET.md`) |
|---|---|---|
| Price | $30k–$60k/year (indicative, scoped by footprint) | $240k/year ($20k/month) |
| Scope | Single strategy, single production node, capped core count (to be fixed per quote) | Unlimited cores/threads/packet volume within one designated trading desk |
| Support | Email, best-effort, same terms as `PILOT_PROGRAM.md` §6.1's "commercially reasonable efforts" language | Dedicated Slack Connect channel + priority email, per §6.3 |
| Source escrow | Not offered at this tier | Negotiable per §5.4 |
| Upgrade path | Rolls into Institutional Enterprise if footprint outgrows single-node/single-strategy scope | — |

The term sheet for this tier is [`DESK_LICENSE_TERMSHEET.md`](DESK_LICENSE_TERMSHEET.md) — same CIN-pending and counsel-review disclaimers as `ANIMUS_ENTERPRISE_TERMSHEET.md`, plus its own §2.4 scope table and §8 upgrade path into the Enterprise tier. Send that document for pricing conversations at this tier, not the $240k Enterprise template.

---

## 4. Technical FAQ & Architecture Cheat Sheet

Plain-English, no C++ background assumed. This is written so you can answer a prospect's written question yourself without faking familiarity — and it doubles as the attachment you send prospects in the Day-1 email, so a written question they'd otherwise ask on a call gets answered before they even send it.

### "What's a ring buffer, and why does this engine use one?"

Picture a fixed number of numbered parking spaces arranged in a circle. A **producer** (whatever's generating events — market data, telemetry) writes into the next space and moves a pointer forward. A **consumer** reads from the oldest unread space and moves its own pointer forward. Neither ever allocates a new space — the circle is fixed-size and pre-allocated once, at startup. When the producer laps the consumer, in *overwrite mode* it just writes over the oldest unread slot (the consumer missed it — expected and counted, not a crash); in other modes it blocks or applies backpressure instead.

Why this matters for latency: no memory allocation happens on the hot path (see next entry), and producer/consumer never have to coordinate through a lock — they just watch each other's position markers.

### "What does 'zero dynamic allocation' actually mean, and why is `malloc`/`new` bad here?"

`malloc`/`new` (and Python's own object allocation) ask the operating system's memory allocator for a chunk of memory *at the moment you need it*. That allocator has to search for free space, maybe ask the OS for more memory, maybe trigger garbage collection — and how long that takes is **not predictable call to call**. For a trading decision loop, an occasional 50-microsecond allocator hiccup can matter more than the average case being fast.

"Zero dynamic allocation on the hot path" means: every buffer the engine touches while processing an event was allocated once, at startup, at a fixed size. Processing an event never asks the OS for memory — so there's nothing for the allocator to be slow (or unpredictable) *about*, in the part of the code that actually runs on every single event.

### "What's cache-line alignment / `alignas(64)`, and what's 'false sharing'?"

A CPU doesn't read memory one byte at a time — it pulls a whole 64-byte chunk ("cache line") into its local cache at once. If two *different* pieces of data that are updated by *different* CPU cores happen to land in the same 64-byte chunk, every time either core updates its piece, the *whole chunk* gets invalidated in the other core's cache — even though that core doesn't care about the byte that changed. Both cores end up fighting over the same cache line for no real reason. That's **false sharing**, and it silently tanks performance in exactly the multi-core producer/consumer pattern this engine relies on.

`alignas(64)` is a compiler instruction that pads a piece of data (e.g. the ring's `head` and `tail` position counters) so it starts on its own fresh 64-byte boundary — guaranteeing the producer's counter and the consumer's counter never share a cache line, so they stop fighting over one.

### "What's `taskset` / core pinning, and why does it matter?"

By default, the operating system's scheduler can move any thread to any CPU core at any moment, and can interrupt a thread mid-execution to let something else run. Both of those are great for a general-purpose machine and terrible for predictable low-latency code — a thread that gets moved to a "cold" core (its data isn't in that core's cache anymore) or gets preempted mid-critical-section can see a latency spike that has nothing to do with the code itself.

`taskset -c N <command>` (Linux) pins a thread to run *only* on core `N`, so the OS scheduler can't move it. That alone helps the median case. The **tail** (p99/p99.9) case needs more: `isolcpus`/`nohz_full`/`rcu_nocbs` kernel boot parameters actually remove a core from the general scheduling pool entirely, so nothing else gets scheduled there at all. `PILOT_EVAL_CHECKLIST.md` §2 walks through both, and is honest that pinning alone reliably helps p50/p90 but doesn't reliably move p99.99 without full isolation — that finding is disclosed, not hidden, in `BENCHMARKS.md` Phase 14.

### "What's the difference between the p50, p99, and p99.9 numbers in the datasheet?"

If you ran the same operation a million times and sorted all million latency measurements from fastest to slowest: **p50** (median) is the value at the halfway point — "typical" case. **p99** is the value 99% of runs were faster than — the slow 1%. **p99.9** is the value 99.9% of runs were faster than — the slow 0.1%, the real outliers. For a trading system, p50 tells you the common case; p99/p99.9 tell you what your *worst* realistic moment looks like, which is usually the number that actually determines whether a latency budget is safe.

### "What does 'zero-copy' mean for the Python SDK, and how is that different from a normal Python interop layer?"

A typical way to move data from a native (C++) library into Python is to **serialize** it — turn it into bytes, hand those bytes to Python, and have Python **deserialize** them into a new Python object. That's at least one full copy of the data, sometimes two, plus the CPU cost of the serialization format itself (JSON, protobuf, pickle, whatever).

"Zero-copy" here means the Python side gets a NumPy array view that points **directly at the same memory the ring buffer already wrote to** — no copy, no serialization step. That's why the datasheet's "drain() only" number (~26ns/event) is so much smaller than "full decode" (~539ns/event): draining is just reading the pointer; decoding is the CPU work of turning those raw bytes into a usable Python object, and you only pay that cost for the events you actually decode.

### "Is this lock-free? Is that the same as 'wait-free'?"

**Lock-free**, specifically — not wait-free, and the repo doesn't claim wait-free. "Lock-free" means no thread ever holds a mutex/lock that could block another thread indefinitely (no thread can go to sleep holding something another thread needs); progress as a system is guaranteed. It does **not** mean every individual operation completes in a bounded number of steps regardless of what other threads are doing — that stronger guarantee is "wait-free," and this engine doesn't claim it. For the SPSC (single-producer/single-consumer) ring specifically, this distinction rarely matters in practice since there's no contention between more than two parties — but it matters if someone asks precisely, and you should answer precisely rather than reach for the stronger word because it sounds better.

### "What happens if the consumer can't keep up with the producer?"

Depends on the configured mode: **overwrite mode** — the producer keeps going and the oldest unread slot gets overwritten; the consumer's own gap counter tracks exactly how many events it missed, so loss is *counted*, not silent. **Backpressure mode** — the producer slows down or blocks instead of overwriting, trading throughput for zero loss. Which one's right depends on whether your use case cares more about "never fall behind" (telemetry/surveillance, some loss tolerable) or "never lose an event" (something in an order-risk path, throughput must yield instead).

---

## 5. Call Contingency Plan — if a prospect insists on a 15-minute sync

Some technical sponsors will ask for a call anyway, especially late in a deal. Don't fight that — but don't let it become a live technical Q&A that depends on fluent, unscripted C++ explanation either. The structure below is built so the **slides do the talking** and your live verbal role stays under two minutes total, in short, low-pressure lines you can say even on a bad day. Screen-share the deck; read only the "SAY" lines verbatim; let silence sit on the slide otherwise — you don't need to fill it.

**Format:** 5 slides, ~12 minutes of the prospect reading/asking in the chat or unmuted, ~2 minutes of you talking, ends with a written follow-up, not a verbal close.

---

**Slide 1 — Title**

*On screen:* "Animus Core — 30-Day Evaluation Results, [Client Name]" + your name/title, contact email.

**SAY (10 seconds):** *"Thanks for the time — I'll keep this short and let the numbers do most of the talking. Stop me anytime with questions in chat or out loud."*

---

**Slide 2 — What you tested and where it landed**

*On screen:* a table, filled in with the client's own Week 1–3 numbers (not generic marketing figures) — their measured p50/p99, throughput, and the acceptance thresholds from `PILOT_EVAL_CHECKLIST.md` §3 side by side, PASS/FAIL per row.

**SAY (15 seconds):** *"This is pulled straight from your own runs, not our reference numbers. [Point at any FAIL row, if present:] That one's the one I want to make sure we cover today."*

*(Then stop talking. Let them read the table and react. This slide is the whole meeting's substance — everything after it is process, not new information.)*

---

**Slide 3 — Open items**

*On screen:* a bullet list, written *before* the call from whatever's already in the written thread — every open question or blocker the client has raised in email/Slack, verbatim where possible, each with a status (Answered / In progress / Needs your input).

**SAY (10 seconds):** *"These are the open items from our email thread — wanted to make sure nothing's fallen through before we talk commercial terms."*

*(This slide exists so the call adds zero new open items you have to answer live and unprepared — everything on it, you already have a written answer for, because it's already in the thread.)*

---

**Slide 4 — Commercial options**

*On screen:* the same two-path structure as the Day-25 email (§2.3) — Desk License ($30k–$60k/yr, scope bullets) vs. `PILOT_PROGRAM.md`'s paid 4-week track — as a simple side-by-side, no negotiation happening live on this slide.

**SAY (20 seconds):** *"Two paths from here — a direct Desk License if your numbers already cleared what you needed, or the four-week paid PoP if you want deeper validation first. I'll send both options with real numbers in writing right after this, so there's no pressure to decide on this call."*

---

**Slide 5 — Next step**

*On screen:* "Next: written follow-up within 24 hours — [your email]." Nothing else.

**SAY (15 seconds, closing):** *"I'll follow up in writing with everything we covered plus exact pricing — take whatever time you need on your end, no rush from me. Thanks again for the time today."*

*(Total live speaking: ~70 seconds across five lines, none of which require explaining an architectural concept from memory — every technical claim is already sitting on the slide behind you. If a live technical question comes up that you can't answer cleanly on the spot, it's fine to say so directly: "Good question — let me get you a precise answer in writing rather than guess" and move on. That's not a weak answer, it's the correct one, and it's consistent with the honest-benchmark-disclosure tone the rest of this repo already uses.)*

---

*This document is process and sales copy, not a commercial agreement. It does not modify, supersede, or replace `PILOT_PROGRAM.md`, `14_DAY_POC_AGREEMENT.md`, `PILOT_CONTRACT.md`, or `ANIMUS_ENTERPRISE_TERMSHEET.md` — see the banner at the top of this file for how the new Desk License tier relates to the existing Enterprise term sheet.*
