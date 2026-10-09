# Telemetry harness re-measurement, 2026-10-04 UTC

`benchmarks/telemetry_benchmark.cpp` (BENCHMARKS.md, Phase 28) re-run on the machine it was first measured on, with the cores chosen explicitly.
The harness source is unchanged and is not part of any claim that the 34 verification gates make; this folder is a separate record.

## Why

Phase 28 published p50 53.3 ns, p99 64.9 ns and 47.3 M msgs/s and described the producer and consumer as pinned to separate physical cores.
The harness's default cores are logical processors 0 and 1. On this CPU those are the two hyperthreads of ONE physical core (`topology.txt`).

* On that default pair the harness reproduces the Phase 28 figures (table 2: p50 41&ndash;57 ns, p99 53&ndash;79 ns, 46.6&ndash;48.5 M msgs/s).
* On separate physical Performance cores, which is what "cross-core" means, it gives table 1: p50 103&ndash;185 ns (median 153), p99 158&ndash;289 ns (median 220) and 17.8&ndash;27.6 M msgs/s (median 21.5)
  &mdash; about three times the latency and less than half the flood throughput.

The site and the datasheet should quote table 1 for a cross-core handoff and say so; table 2 is a same-core figure.

## Protocol

* 40 runs back to back: 2 builds x 4 core pairs x 5 runs (2026-10-04 18:45:14 to 2026-10-04 18:45:49 UTC).
* Builds exactly as BENCHMARKS.md documents: `g++ -std=c++23 -O3 -DNDEBUG -pthread ... -lstdc++exp` (MinGW-w64 g++ 15.2.0, MSYS2 UCRT64) and `cl /std:c++latest /EHsc /O2 /DNDEBUG` (MSVC 19.51).
* Separate physical Performance cores: logical 0 / 2, 4 / 6 and 8 / 10 (physical cores 0 / 1, 2 / 3 and 4 / 5). Same-core reference: logical 0 / 1.
* Machine: Intel Core i7-14650HX (8 Performance cores with Hyper-Threading + 8 Efficiency cores), Windows, on AC power, power plan "Razer Cortex Power Plan".
  Not an isolated or tickless core set: Chrome, Edge and Visual Studio were running, so the tail (p99.9, max) is scheduler noise, as the harness itself says.
  Throughput varies from run to run.

## Results (min / **median** / max)

Table 1: producer and consumer on separate physical Performance cores (cross-core)

| Configuration | runs | p50 ns | p90 ns | p99 ns | p99.9 ns | flood M msgs/s |
|---|---|---|---|---|---|---|
| Separate physical P-cores, both builds pooled | 30 | 102.9 / **152.7** / 184.8 | 134.8 / **186.0** / 241.8 | 158.3 / **219.7** / 289.4 | 2388 / **2479** / 2572 | 17.8 / **21.5** / 27.6 |
| &nbsp;&nbsp;MinGW g++ 15.2 | 15 | 124.4 / **176.9** / 184.8 | 145.1 / **196.3** / 225.3 | 169.9 / **225.7** / 267.4 | 2388 / **2509** / 2572 | 17.8 / **21.3** / 24.2 |
| &nbsp;&nbsp;MSVC 19.51 | 15 | 102.9 / **144.7** / 177.7 | 134.8 / **181.5** / 241.8 | 158.3 / **207.5** / 289.4 | 2388 / **2464** / 2545 | 18.6 / **22.3** / 27.6 |
| &nbsp;&nbsp;logical 0 / 2 | 10 | 137.2 / **172.8** / 181.1 | 181.1 / **193.5** / 241.8 | 217.0 / **225.7** / 289.4 | 2464 / **2541** / 2572 | 18.6 / **21.5** / 24.4 |
| &nbsp;&nbsp;logical 4 / 6 | 10 | 144.7 / **175.1** / 184.8 | 181.5 / **194.9** / 225.3 | 206.7 / **225.3** / 267.4 | 2457 / **2487** / 2517 | 17.8 / **20.5** / 27.6 |
| &nbsp;&nbsp;logical 8 / 10 | 10 | 102.9 / **121.1** / 138.1 | 134.8 / **144.9** / 164.1 | 158.3 / **174.4** / 191.8 | 2388 / **2397** / 2441 | 20.6 / **22.7** / 27.0 |

Table 2: producer and consumer on the two hyperthreads of one physical core (the harness default pair, a same-core figure)

| Configuration | runs | p50 ns | p90 ns | p99 ns | p99.9 ns | flood M msgs/s |
|---|---|---|---|---|---|---|
| One physical core, two hyperthreads (logical 0 / 1), both builds | 10 | 41.3 / **48.4** / 57.5 | 46.3 / **54.4** / 68.6 | 53.3 / **62.8** / 78.5 | 109 / **149** / 284 | 46.6 / **47.2** / 48.5 |
| &nbsp;&nbsp;MinGW g++ 15.2 | 5 | 55.0 / **56.6** / 57.5 | 61.2 / **65.7** / 68.6 | 70.7 / **76.1** / 78.5 | 159 / **197** / 284 | 46.6 / **47.8** / 48.5 |
| &nbsp;&nbsp;MSVC 19.51 | 5 | 41.3 / **41.7** / 41.7 | 46.3 / **47.1** / 47.5 | 53.3 / **54.2** / 55.0 | 109 / **135** / 138 | 46.7 / **47.0** / 47.3 |

`summary.txt` is the full per-build, per-pair listing. Every run's raw output is `<build>_<kind>_<producer>-<consumer>_run<N>.txt` and `.json`.
The one line in each `.txt` that printed the absolute path of its `.json` ("JSON report written to: ...") was reduced to the file name; nothing else was edited.

## Reproduce

```
g++ -std=c++23 -O3 -DNDEBUG -pthread -o telemetry_gxx.exe benchmarks/telemetry_benchmark.cpp -lstdc++exp
telemetry_gxx.exe 0 2 run.json          # producer on logical 0, consumer on logical 2 (separate physical cores)
telemetry_gxx.exe 0 1 run.json          # the default pair: two hyperthreads of one core
python benchmarks/reports/telemetry_rerun_2026-10-04/summarise_runs.py benchmarks/reports/telemetry_rerun_2026-10-04
```

Check your own CPU's topology before choosing cores: on Windows, `GetLogicalProcessorInformationEx(RelationProcessorCore)` lists which logical processors share a core.
