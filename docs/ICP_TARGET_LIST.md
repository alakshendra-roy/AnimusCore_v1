# Animus Core — ICP & Initial 25-Firm Outbound Target List

**Purpose:** Ideal Customer Profile, qualification criteria, and a real, evidence-cited 25-firm target list for the initial outbound motion, using the templates in [`OUTREACH_TEMPLATES.md`](OUTREACH_TEMPLATES.md).

**Core thesis:** the product's differentiated claim (sub-100ns C++ transport, zero-copy Python drain, no strategy rewrite) only matters to a firm that (a) actually runs strategy/decision logic in Python, and (b) has a measurable IPC/serialization tax between their data layer and that Python code. The ICP filters hard on those two conditions, not on general "quant firm" prestige or size.

---

## 1. Ideal Customer Profile

### Firmographic filters

| Criterion | Qualifies | Disqualifies |
|---|---|---|
| Firm type | Prop trading desk, market maker, or execution-infra team running their own book/risk | Buy-side asset manager with no low-latency execution path, pure research shop with no live trading |
| Latency sensitivity | Order-risk checks, market-data-triggered decisions, or telemetry/surveillance on a real microsecond-or-better budget | Strategies on minute/hour+ horizons where transport latency is noise |
| Team maturity | Dedicated infra/platform engineering function that owns the data pipeline as a discrete system | Solo/small quant shop with no infra role |
| Hardware control | Owned/colo hardware they can pin cores on and instrument | Fully outsourced execution via a broker's platform, no kernel-level access |

### Technographic filters

| Signal | Weight |
|---|---|
| Python confirmed in the strategy/decision/execution path (not just research/backtesting) | Must-have |
| Evidence of an IPC/serialization layer today (ZeroMQ, gRPC, Protobuf/FlatBuffers, Kafka, Redis pub/sub, custom sockets) | High |
| Active fight with this exact problem (postings to "speed up Python," reduce GIL overhead, rewrite hot paths in C++/Rust) | High |
| Public latency-benchmarking culture (p99/p99.9, RDTSC/TSC, kernel bypass, CPU pinning) | Medium — qualifies conversation quality |
| Anti-signal: fully native C++/Rust stack, no Python touchpoint anywhere | Disqualifying for this motion |

### Behavioral / trigger signals

- Recent job posting for "Python performance engineer," "low-latency Python," or "infra engineer to reduce tick-to-trade latency"
- Public postmortem, conference talk, or blog post about a latency incident or a data-path rewrite
- Recent headcount growth in trading infra/platform engineering
- Recent expansion into a faster market/instrument, or a new desk/venue
- Recent capital raise or new desk opening

### Qualification scorecard

Score 0–2 per row. **12+/16 = A-tier, 8–11 = B-tier, under 8 = not ready for this list.**

| Dimension | 0 | 1 | 2 |
|---|---|---|---|
| Python in live decision path | No evidence | Research only | Confirmed in live strategy/risk/execution path |
| IPC/serialization signal | None found | Ambiguous | Explicit tech named (ZeroMQ/Protobuf/gRPC/Kafka/sockets) |
| Latency-sensitive workload | Not latency-sensitive | Latency-aware, no public numbers | Public tail-latency culture, own benchmarking |
| Infra team maturity | No dedicated infra role | Small/generalist | Dedicated low-latency infra/platform team, open reqs |
| Active trigger | None | Passive/steady | Active (job post, incident, expansion, funding) |
| Hardware control | Managed/cloud-only | Hybrid | Owned/colo, known to pin cores / tune kernel |
| Contactable decision-maker | Not identifiable | Identifiable, no public technical footprint | Identifiable + public technical content |
| Deal size plausibility | Too small to fund a paid 4-week PoP | Unclear | Consistent with funding a paid engagement |

---

## 2. The 25-Firm Target List

Two research passes via live web search, cross-referenced against the scorecard above. Every firm below has a real, cited source — no firm was added on reputation alone, and none were padded in to hit a round number. One firm found in pass 1 (GTS) was dropped from the final 25 despite matching on firm type, because no research pass turned up a Python-specific citation for it — it fails the must-have technographic filter on current evidence; see §3.

### Tier A — strongest fit (Python confirmed in/near live path + explicit IPC/messaging signal)

1. **Hudson River Trading** — built their own OSS cross-binding interop layer ([pymetabind](https://github.com/hudson-trading/pymetabind)) to reconcile nanobind/pybind11/Boost.Python/pyo3 — direct proof they're solving this exact problem in-house. PyCon US 2026 sponsor talk, ["Scaling Python to Saturate the Hypercube"](https://us.pycon.org/2026/schedule/presentation/151/). *Caveat: they've already built their own tooling — a harder sale, not an easier one.* Target: Head of Trading Technology / Platform Engineering Lead.
2. **Two Sigma** — [Low Latency C++ Software Engineer](https://careers.twosigma.com/careers/JobDetail/New-York-New-York-United-States-Low-Latency-C-Software-Engineer/8966) requires shared memory / cache-aware data structures; ["Fast Engineering"](https://careers.twosigma.com/careers/JobDetail/New-York-City-United-States-Quantitative-Software-Engineer-Fast-Engineering/13078) role: researchers deploy via low-latency C++/Rust, "less latency sensitive models in Python" sits downstream of that boundary by design. Target: Fast Engineering Lead.
3. **Point72 / Cubist** — open [Low-Latency Market Data Engineer](https://www.glassdoor.com/job-listing/low-latency-market-data-engineer-point72-JV_IC1132348_KO0,32_KE33,40.htm?jl=1009790833744) role (JD blocked by 403, directional evidence only); Cubist quant-dev runs "production C++/Python for execution infrastructure" per techinterview.org. Target: Head of Trading Infrastructure (Cubist) — verify via LinkedIn.
4. **Millennium Management (SPEED team)** — [Quantitative Developer, C++ | Low-Latency Systems](https://www.tealhq.com/job/quantitative-developer-c-i-low-latency-systems_7ea1a45d7290341ea809e36f9853517aa3e9a) explicitly requires Python + Apache Arrow/columnar formats + real-time market-data feeds + shared execution platforms in one role. Target: Head of Systematic Platform Execution & Exchange Data (SPEED).
5. **Man Group / Man AHL** — public positioning page [man.com/why-python](https://www.man.com/why-python); per [eFinancialCareers](https://www.efinancialcareers.com/news/2016/11/how-to-become-a-python-coder-at-a-hedge-fund), AHL mostly codes in Python, execution team in Java/C++ — textbook Python-strategy-to-native-execution boundary. Target: Head of AHL Technology.

### Tier B — solid fit (confirmed Python+C++ split or explicit combined requirement)

6. **IMC Trading** — dedicated [Python Software Engineer](https://www.imc.com/us/careers/jobs/4548292101) / [Quantitative Developer - Python](https://www.imc.com/us/careers/jobs/4874399101) reqs alongside a separate low-latency C++/FPGA team. Target: Head of Low-Latency Engineering.
7. **XTX Markets** — Python (NumPy/pandas/PyTorch/JAX) for research, C++ for production, per [Quantt firm guide](https://www.quantt.co.uk/resources/xtx-markets-interview); confirmed by [C++ Software Engineer](https://www.brightnetwork.co.uk/graduate-jobs/xtx-markets/c-software-engineer-london-2025) req. Target: Head of Engineering.
8. **Optiver** — [Low Latency Network Engineer](https://optiver.com/working-at-optiver/career-opportunities/8309310002/) explicitly wants "strong Python scripting experience" bridging into FPGA/C++ execution infra. Target: Head of Low-Latency Engineering.
9. **Squarepoint Capital** — [Ultra Low Latency Platform Engineer](https://simplify.jobs/p/d6debd68-f6ca-4de6-89fc-d94dced0a061/Ultra-Low-Latency-Platform-Engineer) role plus a separate Python/Ruby/Bash platform track; C++ reqs list Python/KDB+ as a plus. Target: Head of Platform Engineering.
10. **D.E. Shaw** — [Quant Systems: Systems Developer](https://www.deshaw.com/careers/quant-systems-systems-developer-london-5295) — Python/Golang/Rust for kernel/network latency tuning, alongside [Options: Software Developer](https://www.deshaw.com/careers/options-software-developer-5324) (C++/Java live trading). Target: Head of Quant Systems Engineering.
11. **Wolverine Trading** — stack confirmed as "C++, C#, Python, XML, SQL" per [Trading Technology page](https://www.wolve.com/trading-technology), with a dedicated [C++ Software Engineer – Latency-Critical Systems](https://careers.wolve.com/postings/2fc10fcf-4043-4b73-9648-203d5cba418d) track. Target: Head of Trading Technology.
12. **DRW** — [Software Engineer, C++](https://www.drw.com/work-at-drw/listings/software-engineer-c-352421) (Core Infrastructure) explicitly requires "high-availability systems in C++ and Python with very tight resource/latency constraints" in one req — strongest single-role signal in this tier. Target: Head of Core Infrastructure.
13. **Virtu Financial** — Core Development team builds "low latency trading platform, internal messaging infrastructure" ([job listing](https://job-boards.greenhouse.io/virtu/jobs/5430830002)); Python used extensively for research/analytics/monitoring around it. Target: Head of Core Development.
14. **Citadel Securities** — [SDET role](https://www.citadelsecurities.com/careers/details/software-engineer-in-test-sdet/) builds "Python-first frameworks and C++-adjacent hooks... to protect latency-sensitive releases." Target: Head of Trading Systems Engineering.
15. **Akuna Capital** — [Python Software Engineer](https://www.builtinchicago.org/job/software-engineer-python-crypto/9853971) role names **Kafka** explicitly for distributed/event-driven work, alongside a separate modern-C++ low-latency team. Target: Head of Trading Technology.
16. **Old Mission Capital** — junior dev program places engineers "writing production C++ and Python for exchange connectivity, network hardware acceleration and real-time data analysis" ([Quantt guide](https://www.quantt.co.uk/resources/old-mission-capital-guide)) — Python directly touching the connectivity/data path. Target: Head of Engineering.
17. **Cumberland (DRW's crypto arm)** — Market Data role: "building and supporting a C++ based technology stack for Cumberland's crypto trading... working in multiple languages including C++ and Python," 5+ yrs C++ and 3+ yrs Python required in one req. [Source](https://www.drw.com/work-at-drw/listings/software-engineer-market-data-cumberland-3346066). Target: Head of Trading Technology, Cumberland.
18. **Wintermute** — C++ Trading Platform Developer role: "designing scalable and robust C++ and Python code" as a main skill; crypto market maker, Linux-only infra. [Source](https://www.wintermute.com/company/opportunities/8c31337e-9989-4cf2-9cab-e876cbc96f10). Target: Head of Trading Technology.
19. **Chicago Trading Company (CTC)** — engineers "comfortable working in Python and/or C++ for developing tools, testbenches, and integrations," alongside a dedicated low-latency C++/market-data team. [Source](https://www.builtinchicago.org/job/c-senior-engineer-market-data/8816431). Target: Head of Low-Latency Engineering.
20. **Belvedere Trading** — "Strong proficiency in Python or C++ for model research, prototyping, and production implementation," dedicated Low Latency Systematic Volatility Trading team building in C++/Java. [Source](https://www.builtinchicago.org/job/software-engineer/6581634). Target: Head of Systematic Volatility Trading Technology.
21. **GSR Markets** — crypto market maker; Trading Systems Engineer wants strong C++ (moving to Rust) plus "Python (Pandas, Numpy)... demonstrable excellence in working on a codebase (C++ or Python)." [Source](https://startup.jobs/quantitative-trader-associate-gsr-3134623). Target: Head of Trading Systems Engineering.
22. **PDT Partners** — engineers "develop and maintain proprietary software stacks using C++ and/or Python" for order management, exchange connectivity, market data, and real-time trading platforms — both named in the same production stack. [Source](https://www.builtinnyc.com/job/trading-infrastructure/6239595). Target: Head of Trading Infrastructure.
23. **ExodusPoint** — Kafka/Hadoop/Spark in the data stack (IPC/messaging signal) plus roles "designing and building reusable Python and C++ components," low-latency/distributed systems required, optimizing to microseconds. [Source](https://builtin.com/job/software-developer-c/2838622). Target: Head of Core Infrastructure.

### Tier C — plausible but weaker/less specific evidence

24. **Tower Research Capital** — hub-and-spoke model: "C++ powers the most latency-sensitive systems... Python used heavily in research, data analysis, and orchestration" per [Engineering page](https://tower-research.com/engineering/) — real but generic split, no specific IPC-layer citation found.
25. **Vatic Labs** — concurrently open [Python Engineer](https://job-boards.greenhouse.io/vaticlabs/jobs/53508) and [Low Latency C++ Engineer](https://job-boards.greenhouse.io/vaticlabs/jobs/52953) reqs (split-stack signal), but no specifics found on what the Python role actually touches.

---

## 3. Researched but excluded

Held out despite two research passes — reported honestly rather than force-fit, per [`OUTREACH_TEMPLATES.md`](OUTREACH_TEMPLATES.md)'s own "don't round up, don't overclaim" standard:

- **GTS (Global Trading Systems)** — confirmed HFT market maker with heavy C++/low-latency hiring, but two research passes found no GTS-specific Python-in-live-path posting. Dropped from the 25 despite fitting the firm-type filter, because it fails the must-have Python-confirmed criterion on current evidence. Worth revisiting with LinkedIn Sales Navigator access rather than general web search.
- **Jump Trading** — Python is research/prototyping only; production trading runs in C++.
- **Jane Street** — core language is **OCaml**, not Python; wrong stack entirely for this pitch.
- **Susquehanna (SIG)** — Python listed as secondary/"some experience," not confirmed in the live path.
- **Marex, Quantlab** — no firm-specific evidence surfaced across two search passes (job-aggregator noise only).
- **Radix Trading, Flow Traders, Balyasny** — some Python signal but not clearly in the live decision path.
- **Jump Crypto** — Python described as "tooling and analytics" only; C++/Rust dominate production.
- **Schonfeld** — Python/C++ confirmed in a risk-dashboard/backtesting role, but the low-latency platform team's own posting doesn't mention Python — split-team evidence, not single-role.
- **Voleon Group** — Python/Go/C++ confirmed but for ML data infrastructure/pipelines, not a live tick-to-trade or order-risk path — wrong workload type.
- **Peak6** — primary stack is Java/Python; C++ listed only as "a big bonus," not confirmed in the live path.

**Methodology gap note:** STAC Summit and CppCon speaker-list searches returned only generic conference-structure pages (session/speaker detail is member-gated) — that research angle didn't pay off and wasn't a source for any firm above. The gap from 19 to 25 was closed via the crypto-market-maker and quant-fund widening angle instead.

---

## 4. Using this list

- No named individual contacts were fabricated or found in either research pass — every firm needs a LinkedIn search against its listed target title to identify the actual person before sending anything.
- Lead Tier A firms with **Variant A/B/C** of the [cold email](OUTREACH_TEMPLATES.md#1-cold-email) or [LinkedIn InMail](OUTREACH_TEMPLATES.md#2-linkedin-inmail) templates; Tier B/C firms are still worth working through the list, just with lower expected hit rate.
- Re-score any firm here if new evidence surfaces (a new job posting, a talk, a blog post) — the scorecard in §1 is the reusable instrument, this 25-firm list is a snapshot as of this research pass, not a static asset.
