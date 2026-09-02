# Animus Core — Outbound Outreach Templates

**Audience:** Heads of Trading, Quant Infrastructure Leads, Latency Engineering at prop firms and market makers.

> Figures cited below are pulled directly from [`../BENCHMARK_DATASHEET.md`](../BENCHMARK_DATASHEET.md) (RDTSC-resolution, depth-1 phase, cross-core SPSC) — don't round them up or drop the methodology qualifier if you extend these; a technical recipient will check. The Proof-of-Performance structure referenced here is [`PILOT_PROGRAM.md`](PILOT_PROGRAM.md). A PDF of the datasheet for attachments lives at [`Animus_Core_Benchmark_Datasheet.pdf`](Animus_Core_Benchmark_Datasheet.pdf).

Each channel below has three lead-hook variants — same claims, different opening signal — for A/B testing which lands hardest with a given recipient.

---

## 1. Cold Email

### Variant A — Tail-latency led

**Subject:** `64.9ns p99, RDTSC-measured — worth 20 minutes?`

> Hi [First Name],
>
> Cross-core SPSC dispatch on our engine measures p50 53.3ns / p99 64.9ns / p99.9 111.6ns, RDTSC-resolution, lfence-serialized, calibrated against steady_clock — not QPC-quantized guesswork. Sustained throughput 47.3M msgs/sec. Full methodology and reproduction commands are public if you want to check our math before you read another word of this email.
>
> The part that usually matters more to a desk than the raw number: the Python SDK drains that ring at ~4.5ns/event (amortized), zero-copy, direct buffer pointers via `ctypes` — no serialization, no socket, no message broker between the ring buffer and your strategy code. If your current stack is paying an IPC/socket tax to get market data or signals into Python, that tax goes away without rewriting the strategy layer. Same Python, same logic, different transport underneath it.
>
> If that's a real budget line for your desk — order-risk check, market-data-triggered decision loop, telemetry/surveillance path — we run a paid 30-day Proof-of-Performance: Week 1 is historical replay against your own tick/order data (correctness + throughput baseline on your event shapes, not our synthetic payloads), Week 2 is shadow execution against live data with zero production risk, Week 3 is tail-latency characterization on your own hardware, Week 4 is a signed-off decision either way. No production commitment implied by running it.
>
> Worth a 20-minute call to see if it's a fit? Happy to send the full datasheet and reproduction commands first if that's a faster path for your team.
>
> Alakshendra Roy
> Founder & Engineer — Animus Core
> royrichie006@gmail.com

---

### Variant B — Throughput/scale led

**Subject:** `47.3M msgs/sec, one machine — how are you moving telemetry today?`

> Hi [First Name],
>
> Our lock-free SPSC ring sustains 47.3M msgs/sec cross-core, with p99 dispatch latency at 64.9ns (RDTSC-measured, not clock-quantized). At 8-producer MPMC contention it still holds 7.2M pushes/sec. These aren't synthetic single-core numbers — full methodology and reproduction commands are public.
>
> The reason this matters beyond the raw throughput: the same ring drains into Python at ~4.5ns/event, zero-copy, direct buffer pointers — no serialization step eating into that headroom before your strategy code sees it. If your desk is capped by a broker or socket layer well below what the transport itself could sustain, that's a specific, fixable bottleneck.
>
> We validate this against your own hardware and event shapes in a paid 30-day Proof-of-Performance — historical replay first, then live shadow execution, zero production risk.
>
> Worth 20 minutes to see where your current numbers sit against this?
>
> Alakshendra Roy
> Founder & Engineer — Animus Core
> royrichie006@gmail.com

---

### Variant C — Python zero-copy interop led

**Subject:** `Same strategy Python, ~4.5ns/event to drain — no rewrite`

> Hi [First Name],
>
> If your team's strategy logic is in Python and your data path into it runs through IPC or a socket layer, here's the number that usually changes that calculus: our SDK drains the native ring at ~4.5ns/event (amortized), zero-copy, via direct buffer pointers through `ctypes` — not a serialized message per event. Full decode when you need it runs ~560ns/event; the point is you only pay for what you actually touch.
>
> Underneath that, the transport itself measures p99 64.9ns cross-core (RDTSC-resolution) with 47.3M msgs/sec sustained throughput. The SDK is the part that matters for your integration timeline, though — it's a drop-in replacement for whatever's moving data into your strategy code today, not a rewrite of it.
>
> We validate both layers against your own data and hardware in a paid 30-day Proof-of-Performance: historical replay, then live shadow execution, no production risk.
>
> Worth a quick call to see if it's a fit?
>
> Alakshendra Roy
> Founder & Engineer — Animus Core
> royrichie006@gmail.com

---

## 2. LinkedIn InMail

### Variant A — Tail-latency led

> [First Name] — cross-core dispatch on the engine I built runs p99 64.9ns (RDTSC, not QPC-rounded), and the Python SDK drains it at ~4.5ns/event zero-copy — no IPC serialization between the ring and your strategy code.
>
> If your desk is still paying a socket/serialization tax to get signals into Python, that's the specific thing this removes, without a rewrite. We run a paid 30-day PoP — historical replay on your own data, then shadow execution live, no production risk — if that's worth validating against your own hardware. Open to a quick call?

### Variant B — Throughput led

> [First Name] — our lock-free ring sustains 47.3M msgs/sec cross-core at p99 64.9ns (RDTSC-measured). Python SDK drains it at ~4.5ns/event, zero-copy — no serialization eating into that before your strategy code sees it.
>
> If your data path is capped well below what the transport could sustain, that's usually a socket/broker bottleneck, not a hardware one. Paid 30-day PoP against your own data and hardware if you want to check. Worth a call?

### Variant C — Python interop led

> [First Name] — if your strategy's in Python and it's fed through IPC or a socket, our SDK drains a native ring at ~4.5ns/event, zero-copy, direct buffer pointers — no rewrite, just a different transport underneath the same code.
>
> Transport itself runs p99 64.9ns cross-core, RDTSC-measured. We validate against your own hardware in a paid 30-day PoP — historical replay, then live shadow, no production risk. Open to a quick call?

---

## 3. Follow-Up Bump (Day 5, no response)

### Email

**Subject:** `Re: [original subject]` (reply in-thread, don't restart)

> [First Name] — following up in case this landed in a busy week.
>
> One more data point in case it's useful: the 64.9ns p99 is cross-core SPSC transport only — the number that usually decides these conversations is what your desk sees end-to-end on your own event shapes, which is exactly what Week 1 of the PoP (historical replay, your data, no live access needed) is scoped to answer before anything touches a live feed.
>
> No pressure if the timing's wrong — happy to send the datasheet with reproduction commands now and pick this back up whenever it's relevant on your end.
>
> Alakshendra Roy

### LinkedIn

> [First Name] — following up in case this got buried. Happy to just send the datasheet + reproduction commands if that's an easier first step than a call. No worries either way.

---

## 4. Follow-Up Bump (Day 12, no response)

### LinkedIn

> [First Name] — last nudge from me on this one. If tail latency / IPC overhead isn't an active priority for the desk right now, no worries at all — I'll leave it here. If it becomes relevant later, the datasheet's public and the offer stands whenever the timing's better.
