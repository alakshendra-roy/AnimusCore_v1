# Animus — Outbound Go-To-Market & Prospecting Package

**Owner:** Chief Commercial Officer, Animus
**Product:** Animus Core — C++20/Python deterministic IPC & telemetry engine (sub-40ns p50 latency, 16M+ events/sec sustained throughput)
**Licensing:** Dual-License Gateway — Community Evaluation (free, non-production) / Enterprise Tier 1 $35,000/yr / Enterprise Tier 2 $90,000/yr / Enterprise Tier 3 OEM $150,000+/yr / Enterprise Tier 4 Global Strategic Master Agreement $350,000–$750,000+/yr

> Note on Section 2: company names are realistic, publicly known firms in each vertical, used as **illustrative targets**. Stack/bottleneck notes are **inferred from public engineering blogs, job postings, and conference talks**, not confirmed insider information — verify before referencing specifics in outreach.

---

## 1. Target Ideal Customer Profiles (ICPs)

### Profile A — Mid-Tier Proprietary Trading & Market Making Desks

**Who they are:** Prop shops and market makers running latency-sensitive strategies (latency arbitrage, crypto cross-exchange market making, automated execution) who have outgrown a single-language stack but are not large enough to run a 40-person platform team.

**Target job titles:**
- Head of Trading Technology / Head of Low-Latency Engineering
- Director of Execution Systems
- Principal / Staff Software Engineer, Trading Infrastructure
- Quant Developer Lead (Python → C++ bridge owner)
- CTO (at firms <150 engineers)

**Core technical bottlenecks:**
- Serialization/copy overhead marshaling data between a Python research/alpha layer and a C++ execution gateway (pickling, protobuf, or REST/gRPC round trips adding microseconds where nanoseconds matter).
- Hand-rolled shared-memory ring buffers that are brittle, undocumented, and owned by one departing engineer ("bus factor of one").
- Kernel-bypass and lock contention issues under bursty market-data fan-out (order book updates, tick-to-trade).
- Non-deterministic tail latency (p99/p999) from GC pauses, malloc jitter, or context switches — invisible in backtests, fatal in production.
- Difficulty benchmarking/proving latency claims to internal risk committees before promoting a new strategy to prod.

**Wedge hook:** "You're already paying the Python↔C++ tax every tick — we eliminate it with a zero-copy shared-memory bridge that benchmarks in under a minute on your own hardware, no vendor lock-in, no IPC redesign."

---

### Profile B — Autonomous Robotics & Edge Appliance Developers

**Who they are:** Robotics, AV, drone, and edge-AI companies building real-time sensor fusion pipelines (LiDAR, camera, radar, IMU) on embedded or edge-server compute, typically on ROS2/CycloneDDS or a custom middleware stack.

**Target job titles:**
- Director of Robotics Software / Perception Systems
- Staff/Principal Software Engineer, Middleware or Real-Time Systems
- Head of Autonomy Platform
- Embedded Systems Architect
- VP Engineering (at Series B–D robotics companies)

**Core technical bottlenecks:**
- ROS2/CycloneDDS dynamic memory allocation and serialization overhead on high-bandwidth sensor topics (multi-camera + LiDAR fan-out), causing missed frame deadlines.
- DDS discovery and QoS tuning fragility at scale (multi-node fleets, degraded performance under network contention).
- Jitter in the sensor-fusion hot path that erodes the timing budget for perception → planning → control loops.
- Cross-process IPC between a real-time control loop (C++) and higher-level perception/ML stacks (often Python/PyTorch) without violating determinism guarantees.
- Certification/safety-case pressure (ISO 26262 / DO-178C-adjacent) that makes "swap in a new open-source DDS implementation" a multi-quarter risk, not a quick fix.

**Wedge hook:** "Keep your ROS2 API surface — replace only the transport under CycloneDDS/FastDDS with a deterministic zero-copy engine, and get your frame-deadline jitter back under control without re-architecting your perception stack."

### Strategic Positioning Note — Using Tier 4 as a Procurement Anchor

Tier 4 (Global Strategic / Infrastructure Master Agreement, $350,000–$750,000+/yr) is not the opening offer for either ICP above — it is an **anchoring instrument**. In any institutional procurement conversation involving a global account (a multi-region market maker, a Tier-1 exchange, or a fleet-scale robotics platform operator), introduce Tier 4 early and explicitly as the ceiling of what Animus can contractually structure — an uncapped, enterprise-wide master agreement with escrow/audit access and a dedicated engineering desk. This has two effects:

1. **Anchoring.** Once a buyer has processed a $350k–$750k+/yr master agreement as a real, structured offering, Tier 1 ($35,000/yr) and Tier 2 ($90,000/yr) read as low-friction, low-commitment entry points rather than "vendor pricing to be negotiated down" — the perceived gap between "trying it" and "buying it" collapses.
2. **Land-and-expand path.** Most global accounts should still be closed at Tier 1 or Tier 2 first (per Section 4's qualification criteria) and expanded into Tier 4 once the technical champion has internal traction and the account's own procurement organization is ready for a master-agreement negotiation — do not lead a first conversation with a Tier 4 ask unless the prospect's compliance/procurement posture (e.g., a stated aversion to per-node licensing across a distributed estate) makes Tier 4 the *only* structure they can transact under (see the objection response below).

Never present Tier 4 as the default ask for a single-desk or single-platform prospect — it is reserved for the global/enterprise-wide procurement conversation, not a general upsell target.

---

## 2. The 30-Account Target Matrix

| # | Category | Company Archetype / Name | Typical HQ | Target Job Title | Probable Stack / Bottleneck | Custom Pitch Angle |
|---|----------|---------------------------|------------|-------------------|------------------------------|---------------------|
| 1 | Trading | Jump Trading | Chicago, IL | Head of Low-Latency Engineering | Custom C++ core, kernel-bypass NICs; likely in-house ring buffers with high maintenance overhead | Offer independent benchmark vs. their in-house buffer under identical NIC/kernel config |
| 2 | Trading | Jane Street | New York, NY | Director of Trading Systems | OCaml + C++ hybrid; cross-language IPC between OCaml research and C++ execution is a known pain point | Position as neutral zero-copy bridge layer, language-agnostic C-ABI |
| 3 | Trading | Hudson River Trading | New York, NY | Principal Engineer, Trading Infra | Heavy custom FPGA + C++ stack; software-side IPC between strategy containers likely still socket/shm hybrid | Pitch sub-40ns software layer as complement to FPGA fast path, not competitor |
| 4 | Trading | Citadel Securities | Chicago, IL | Head of Execution Engineering | Large internal platform team; Python research (alpha) to C++ execution gateway bridge | Target the Python/C++ alpha-to-execution tax specifically |
| 5 | Trading | DRW | Chicago, IL | VP, Trading Technology | Mixed C++/Java market-making stack; cross-service messaging likely ZeroMQ-based | Middleware bottleneck angle — ZeroMQ jitter replacement |
| 6 | Trading | Optiver | Amsterdam, NL / Chicago, IL | Head of Software Engineering, Trading | Proprietary low-latency C++ stack; discovery-based IPC scaling issues at multi-region scale | Determinism + p999 tail-latency proof point for risk committee sign-off |
| 7 | Trading | IMC Trading | Chicago, IL / Amsterdam, NL | Director of Core Technology | Custom market-data distribution bus; likely shared-memory with manual synchronization | Offer RSA key for internal benchmark bake-off |
| 8 | Trading | Tower Research Capital | New York, NY | Head of Latency Engineering | Highly custom per-desk stacks; fragmented IPC solutions across strategy teams | Standardization pitch — one deterministic IPC layer across desks |
| 9 | Trading | XTX Markets | London, UK | Principal Systems Engineer | ML-heavy research pipeline (Python) feeding C++ execution; GPU-to-CPU handoff latency | C++/Python bridge angle — model inference to execution gateway |
| 10 | Trading | Susquehanna International Group (SIG) | Bala Cynwyd, PA | Director, Trading Technology | Mixed options market-making stack; legacy messaging middleware | Benchmark challenge variant, single bash command verification |
| 11 | Trading | Two Sigma | New York, NY | Head of Trading Infrastructure | Large polyglot research stack (Python/Java/C++); internal IPC framework likely aging | Position as drop-in replacement transport, not full platform rewrite |
| 12 | Trading | Belvedere Trading | Chicago, IL | VP Engineering | Mid-size options MM desk; likely ZeroMQ or custom shm for options book distribution | Direct benchmark challenge to engineering lead |
| 13 | Trading | Akuna Capital | Chicago, IL | Director of Technology | Options MM; C++ core, Python quant tooling bridge | C++/Python bridge angle |
| 14 | Trading | GTS (Global Trading Systems) | New York, NY | Head of Market Making Technology | Equities/ETF MM; high fan-out market data distribution | Middleware bottleneck solver — market data fan-out jitter |
| 15 | Trading | Wolverine Trading | Chicago, IL | Principal Engineer, Infrastructure | Mid-tier options MM; likely custom in-house messaging | In-house ring buffer replacement pitch |
| 16 | Trading | Old Mission Capital | Chicago, IL | Head of Technology | Options/ETF MM; smaller eng team, likely open-source middleware (ZeroMQ/Aeron) | Aeron/ZeroMQ displacement — total cost of ownership angle |
| 17 | Trading (Crypto) | Wintermute | London, UK | Head of Engineering | Crypto MM/prop; cross-exchange latency arb, Python strategy layer to C++ execution | Crypto-specific benchmark: cross-exchange tick-to-trade |
| 18 | Trading (Crypto) | GSR Markets | London, UK / Singapore | Director of Trading Systems | Crypto market making; likely REST/WebSocket-heavy with internal normalization layer | Zero-copy internal bus behind exchange connectivity layer |
| 19 | Trading (Crypto) | Cumberland (DRW) | Chicago, IL | Head of Technology | Crypto OTC/prop; shared infra with DRW core trading stack | Cross-sell via DRW relationship (#5) |
| 20 | Trading (Crypto) | Amber Group | Singapore | VP Engineering | Crypto MM/prop; distributed multi-region execution nodes | Determinism across multi-region deployment angle |
| 21 | Robotics | Boston Dynamics | Waltham, MA | Director, Robotics Software | ROS2-adjacent custom middleware; multi-sensor fusion on legged/mobile platforms | ROS2 transport replacement, keep API surface |
| 22 | Robotics | Waymo | Mountain View, CA | Staff Engineer, Perception Middleware | Large custom AV middleware stack; high-bandwidth LiDAR/camera fan-out | Sensor fusion jitter reduction pitch |
| 23 | Robotics | Zoox (Amazon) | Foster City, CA | Principal Engineer, Autonomy Platform | Custom + ROS2-influenced stack; safety-case constraints on middleware swaps | Transport-layer-only swap, no re-architecture, safety-case friendly |
| 24 | Robotics | Anduril Industries | Costa Mesa, CA | Head of Autonomy Software | Defense-grade autonomy stack; likely DDS-based (CycloneDDS/FastDDS) sensor pipelines | DDS transport replacement, deterministic timing for defense certification |
| 25 | Robotics | Skydio | San Mateo, CA | Director of Software Engineering | Drone autonomy; onboard compute-constrained sensor fusion, likely ROS2 | Edge-appliance memory contention angle, embedded footprint |
| 26 | Robotics | Nuro | Mountain View, CA | Staff Engineer, Middleware/Real-Time Systems | AV delivery robots; ROS2/CycloneDDS-based perception stack | ROS2/CycloneDDS bottleneck solver variant |
| 27 | Robotics | Figure AI | Sunnyvale, CA | Head of Robotics Software | Humanoid robotics; real-time control loop + Python/PyTorch perception bridge | C++/Python bridge for control-loop to ML-inference handoff |
| 28 | Robotics | Agility Robotics | Corvallis, OR | Director of Software | Humanoid/bipedal robotics; embedded real-time control, sensor fusion IPC | Determinism pitch for control-loop timing budget |
| 29 | Robotics | Applied Intuition | Mountain View, CA | Principal Engineer, Simulation & Middleware | AV simulation/middleware tooling vendor; DDS-based test infrastructure | Position as complementary infra for their customers' stacks (partner angle) |
| 30 | Robotics | Shield AI | San Diego, CA | Head of Autonomy Engineering | Defense autonomy/drones; DDS-based sensor fusion, certification pressure | DDS transport replacement, deterministic timing for certification |

---

## 3. High-Conversion Technical Cold Outreach Sequences

All variants target senior ICs and engineering directors. No marketing fluff, no buzzwords, lead with a verifiable technical claim.

### Variant 1 — Direct Benchmark Challenge

**Email (3 sentences):**
> Subject: sub-40ns IPC — verify it yourself in one line
>
> [Name] — we built a deterministic IPC engine that holds sub-40ns p50 / <150ns p99 latency at 16M+ events/sec on commodity hardware, and I'd rather you verify that than take my word for it. Run this against your own box and compare it to whatever ring buffer or message bus you're running today:
> ```bash
> curl -sL https://animus.dev/bench.sh | bash -s -- --events 16000000 --report json
> ```
> If our numbers don't beat your current stack on your own hardware, I won't follow up again — if they do, worth 15 minutes to talk about where it'd slot into your execution path?

**LinkedIn connection note (<300 chars):**
> Hi [Name] — built a sub-40ns / 16M+ events-per-sec deterministic IPC engine, benchmarkable in one bash command on your own hardware. Not pitching, just want a systems engineer to try to break the numbers. Worth connecting?

---

### Variant 2 — The Middleware Bottleneck Solver (ROS2/FastDDS/ZeroMQ)

**Email (3 sentences):**
> Subject: replacing DDS/ZeroMQ jitter under your transport layer
>
> [Name] — if your sensor fusion or market-data fan-out is still fighting CycloneDDS/FastDDS discovery overhead or ZeroMQ copy/jitter under load, we built a drop-in zero-copy transport that sits under your existing API (ROS2 topics stay ROS2 topics) and holds deterministic sub-40ns latency at 16M+ events/sec. No re-architecture, no protocol migration — just swap the transport layer and rerun your existing jitter benchmarks. Happy to send a 30-day evaluation key if you want to run it against your current p99/p999 numbers before we talk pricing.

**LinkedIn connection note (<300 chars):**
> Hi [Name] — we replace DDS/ZeroMQ under the hood (ROS2 API stays intact) with a zero-copy transport holding sub-40ns latency at 16M+ events/sec. If jitter under load is a live problem for you, worth a quick look?

---

### Variant 3 — The C++/Python Bridge Angle

**Email (3 sentences):**
> Subject: killing the Python↔C++ tax between your alpha and your execution gateway
>
> [Name] — every microsecond your strategy loses marshaling data between Python research code and your C++ execution gateway is a microsecond a competitor with a tighter bridge doesn't lose. Animus gives you a zero-copy shared-memory bridge with a C-ABI and ctypes-only Python wrapper — no pickling, no protobuf, no gRPC round trip — holding sub-40ns latency at 16M+ events/sec. I can get you a 30-day evaluation key today if you want to benchmark it against your current bridge before your next strategy promotion cycle.

**LinkedIn connection note (<300 chars):**
> Hi [Name] — we eliminate the Python↔C++ serialization tax between research/alpha code and execution gateways with a zero-copy C-ABI bridge (sub-40ns, 16M+ events/sec). If that tax is costing you latency, worth comparing notes?

---

## 4. Qualification & Objection Playbook

### Qualification Criteria for Issuing a 30-Day RSA Evaluation Key

Issue an evaluation key only when the prospect meets **at least 3 of the following 5**:

1. **Named technical champion** — a systems/infra engineer (not a manager forwarding a form) has engaged directly and can run the benchmark themselves.
2. **Concrete bottleneck identified** — they've named a specific pain point (tail latency, DDS jitter, Python/C++ marshaling cost) rather than generic interest.
3. **Production intent** — there's a real deployment target (a strategy going live, a robot platform shipping) within 2 quarters, not pure R&D curiosity.
4. **Budget authority reachable** — the champion can get a signature on a $35k–$150k/yr line item, or has a clear path to whoever can.
5. **Benchmarking capability** — they have hardware and the internal access to run a fair, isolated benchmark (not a shared dev box with noisy neighbors that will produce misleading numbers).

If fewer than 3 are met, keep them on Community Evaluation (non-production) until qualification improves — don't burn a 30-day key on a tire-kicker.

---

### Objection Responses

**1. "We already built an in-house shared-memory ring buffer."**

> That's exactly the profile of prospect we want benchmarking against us, not the one we want to talk out of it. In-house ring buffers are usually solid on the happy path but degrade under two conditions: multi-producer/multi-consumer contention at scale, and bus-factor risk when the one engineer who built it leaves. Ask two questions: what's your measured p999 (not p50) under realistic multi-producer load, and who's the second person who can debug it at 2am? If both answers are solid, you may genuinely not need us — but most in-house buffers haven't been pressure-tested against those two questions, and that's a 20-minute benchmark to find out, not a re-architecture.

**2. "Why not just use open-source Aeron, Iceoryx, or Boost.Interprocess?"**

> Those are real, credible options — this isn't "open source is bad." The gap is deterministic latency floor and support surface: Aeron is JVM-centric with GC-adjacent tuning overhead for a pure-C++ hot path; Iceoryx is solid for pub/sub robotics IPC but isn't built for the Python↔C++ bridge case (no zero-copy ctypes-native path); Boost.Interprocess gives you the primitives but you're still building and maintaining the ring-buffer logic, backpressure handling, and telemetry yourself. Animus gives you sub-40ns determinism, the C-ABI/Python bridge, and telemetry out of the box, with a support contract instead of a GitHub issue queue when it breaks in production. Benchmark it against whichever of the three you're evaluating — same bash command, your hardware.

**3. "We don't buy third-party infrastructure for live execution nodes."**

> That's a real and reasonable bar for infrastructure that trades your book — we'd have the same policy. Two things worth knowing before that closes the conversation: first, this isn't a hosted service or SaaS dependency — Community and Enterprise builds are a static library/DLL you compile and vet like any other vendor dependency, with no runtime network calls or external IPC. Second, most institutional desks we talk to don't run vendor code on the live execution node itself — they run it as the transport layer beneath their own execution logic, same trust boundary as a kernel-bypass NIC driver or a commercial market-data feed handler. If your policy specifically excludes any third-party binary regardless of deployment model, that's a hard no we respect — but if the objection is really about auditability and control, we can walk through the binary, the C-ABI surface, and the support/SLA terms so your risk team can evaluate it on its actual merits.

**4. "What if our compliance forbids per-node or per-core license tracking across distributed clusters?"**

> Then Tier 1–3's per-node model is genuinely the wrong fit, and we'd say so rather than push it — but that's exactly what the Tier 4 Global Strategic / Infrastructure Master Agreement exists to solve. Tier 4 is priced and licensed at the master-agreement level, not the node level: entitlement covers your entire global estate — every legal entity, data center, co-location rack, and edge platform under common control — under one uncapped grant, with no requirement to reconcile license counts against individual nodes or cores across a distributed cluster. Each node still runs its own signed, air-gapped `.lic` file for cryptographic integrity, but your compliance function tracks one master agreement, not a fleet-wide license inventory. That's a $350,000–$750,000+/yr structure, so it's worth confirming your estate's scale justifies it before we go there — but if per-node tracking is the actual blocker, Tier 4 removes it entirely.

---

*Prepared for Animus Business Development. Update the 30-account matrix quarterly as firms are contacted, qualified, or disqualified — track status in a CRM, not in this file.*
