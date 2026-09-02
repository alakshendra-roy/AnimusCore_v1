# Animus Core — Outbound Outreach Templates

**Audience:** Heads of Trading, Quant Infrastructure Leads, Latency Engineering at prop firms and market makers.

> Figures cited below are pulled directly from [`../BENCHMARK_DATASHEET.md`](../BENCHMARK_DATASHEET.md) (RDTSC-resolution, depth-1 phase, cross-core SPSC) — don't round them up or drop the methodology qualifier if you extend these; a technical recipient will check. The Proof-of-Performance structure referenced here is [`PILOT_PROGRAM.md`](PILOT_PROGRAM.md).

---

## 1. Cold Email

**Subject options** (pick one, A/B if volume allows):
- `64.9ns p99, RDTSC-measured — worth 20 minutes?`
- `Removing your IPC hop without touching strategy Python`
- `Tail latency number for [Firm]'s desk`

---

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

## 2. LinkedIn InMail

> [First Name] — cross-core dispatch on the engine I built runs p99 64.9ns (RDTSC, not QPC-rounded), and the Python SDK drains it at ~4.5ns/event zero-copy — no IPC serialization between the ring and your strategy code.
>
> If your desk is still paying a socket/serialization tax to get signals into Python, that's the specific thing this removes, without a rewrite. We run a paid 30-day PoP — historical replay on your own data, then shadow execution live, no production risk — if that's worth validating against your own hardware. Open to a quick call?

---

## 3. Telegram DM

> Hey [First Name] — built a zero-copy telemetry engine, p99 64.9ns cross-core (RDTSC-measured), Python SDK drains it at ~4.5ns/event, no IPC/socket hop into your strategy code. If IPC overhead is costing your desk latency budget, we run a paid 30-day PoP — historical replay + live shadow execution on your own hardware, no rewrite required. Worth a look?

---

## 4. Follow-Up Bump (Day 5, no response)

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

### Telegram

> Hey [First Name] — bumping this in case it got lost. Can send the benchmark writeup if that's easier than a call for now.
