# Animus Core — Pilot Evaluation & Verification Checklist

**Classification:** Institutional Pilot Onboarding & Verification Document
**Audience:** Quantitative Trading Infrastructure Engineers, Co-Location Architects, and Systems Audit Teams conducting a technical qualification of the Animus Core SPSC Ring Buffer (`include/animus/shm_ipc.hpp`) and the Python Zero-Copy SDK (`sdk/python/`).
**Companion documents:** [`EVALUATION_KIT.md`](EVALUATION_KIT.md) (benchmark tear-sheet + full prerequisites) · [`EVALUATION_GUIDE.md`](EVALUATION_GUIDE.md) (public-ABI reproduction guide) · [`../eval_kit/README.md`](../eval_kit/README.md) (turnkey tarball quickstart) · [`PILOT_PROGRAM.md`](PILOT_PROGRAM.md) (the paid engagement this checklist gates entry into).

---

## How to Use This Checklist

Each section below is an independently verifiable gate. Work top to bottom — Sections 1–2 qualify the *host*, Section 3 qualifies the *build/telemetry*, and Section 4 is the reference matrix for anything that fails either gate. Do not carry forward a "PASS" from a prior evaluation cycle without re-running Section 3 against the exact tarball and SHA-256 you were issued for **this** pilot window; binaries are rebuilt per release and are not interchangeable across kits. Record results in the Section 5 sign-off table for the audit trail.

---

## 1. Hardware & Environment Prerequisites

| Layer | Requirement | Verification Command |
|---|---|---|
| **CPU microarchitecture** | x86_64, Intel Core 12th Gen or newer, or AMD Zen 3 / Zen 4 / Zen 5 | `lscpu \| grep 'Model name'` |
| **TSC (Time-Stamp Counter)** | **Invariant / constant TSC required.** `harness_benchmark`'s enqueue-latency percentiles are RDTSC-timestamped and calibrated once against wall clock at startup — a non-invariant TSC (P-state/C-state frequency-scaled, or unsynchronized across sockets) silently corrupts cross-core latency comparisons without raising an error. | `grep -m1 -o 'constant_tsc\|nonstop_tsc' /proc/cpuinfo` (expect a match) and `cat /sys/devices/system/clocksource/clocksource0/current_clocksource` (expect `tsc`, not `hpet` or `acpi_pm`) |
| **Operating system** | Ubuntu 22.04 / 24.04 / 26.04 LTS, or native RHEL / Rocky Linux 9 (not a container base image lacking `/dev/shm` — see §4) | `cat /etc/os-release` |
| **Compiler / language standard** | GCC 13+ or Clang 17+ with C++20 support. Required for `sdk/python`'s nanobind extension and `animus-eval-kit`'s own CMake target (both build against `-std=c++20`); the pre-built `harness_benchmark` binary shipped in the tarball needs no local compiler at all. | `g++ --version` / `clang++ --version`; confirm `-std=c++20` is accepted: `echo 'int main(){}' \| g++ -std=c++20 -x c++ - -o /tmp/probe20 && echo OK` |
| **Python interop** | CPython 3.10 through 3.14, with `numpy >= 1.26` for the zero-copy `to_numpy()` consumption path. (The SDK's own package floor is broader — `requires-python >= 3.8`, numpy optional — this narrower range is what this checklist's acceptance thresholds in §3 were validated against.) | `python3 --version`; `python3 -c "import numpy; print(numpy.__version__)"` |
| **POSIX shared memory** | `/dev/shm` present, writable, and sized for the ring capacity under test (default capacity 1,048,576 slots) | `df -h /dev/shm` |

---

## 2. Host Isolation & Kernel Tuning Checklist

Every item below reduces OS-scheduler jitter on the two cores the producer and consumer will occupy. None are required for functional correctness — the ring is correct on an untuned host — but the §3 latency thresholds assume this section is complete. Skipping it does not fail the eval; it does invalidate a tail-latency (p99/p99.9) comparison against those thresholds.

- [ ] **CPU isolation** — remove the target cores from the general SMP scheduling domain and disable their periodic timer tick. Add to the kernel command line (`/etc/default/grub` → `GRUB_CMDLINE_LINUX`, then `update-grub` / `grub2-mkconfig -o /boot/grub2/grub.cfg`, then reboot):

  ```bash
  GRUB_CMDLINE_LINUX="isolcpus=2,3 nohz_full=2,3 rcu_nocbs=2,3"
  ```

  Verify after reboot:

  ```bash
  cat /sys/devices/system/cpu/isolated        # expect: 2-3
  ```

- [ ] **Physical-core topology check (before pinning)** — never pin producer and consumer to two hyperthread siblings of the same physical core; they share L1/L2 and will under-report contention as latency:

  ```bash
  lscpu -e   # confirm cores 2 and 3 show different values in the CORE column
  ```

- [ ] **Core affinity & thread pinning** — pin producer and consumer to separate, isolated, physical cores (ideally the same NUMA node):

  ```bash
  # Producer (isolated core 2)
  taskset -c 2 ./bin/harness_benchmark --name animus_pilot_eval \
      --events 10000000 --core 2 --mode overwrite &

  # Consumer (isolated core 3) -- verify_stream.py has no internal
  # pinning flag of its own; pin it externally via taskset, always
  taskset -c 3 python3 scripts/verify_stream.py --name animus_pilot_eval --events 10000000
  ```

- [ ] **Real-time scheduling priority** — elevate the producer (and, for a zero-loss backpressure run, the consumer) to `SCHED_FIFO` so it preempts rather than waits behind remaining kernel-level work:

  ```bash
  sudo chrt -f 99 taskset -c 2 ./bin/harness_benchmark --core 2 --mode backpressure
  ```

- [ ] **Disable CPU frequency scaling** — pin the isolated cores to their maximum P-state so enqueue latency isn't measured across a frequency transition:

  ```bash
  for c in 2 3; do
    echo performance | sudo tee /sys/devices/system/cpu/cpu$c/cpufreq/scaling_governor
  done
  cpupower -c 2,3 frequency-info | grep 'current policy'   # confirm 'performance'
  ```

---

## 3. Air-Gapped / Turnkey Verification Runbook

The evaluation kit (`animus-eval-kit-linux-x86_64.tar.gz`) requires no compiler, no `cmake`, and no network access on the evaluation machine — every step below runs fully air-gapped once the tarball is on the box.

- [ ] **Integrity validation before extraction.** Never extract or execute a tarball whose hash you have not independently confirmed against the value your Animus point of contact issued at delivery time:

  ```bash
  sha256sum animus-eval-kit-linux-x86_64.tar.gz
  # compare the printed hash, byte for byte, against the value provided
  # out-of-band (delivery email / pilot intake channel) -- a mismatch
  # means do not proceed; request a re-issued tarball and hash instead
  ```

- [ ] **Unpack and inspect the build manifest** — confirms the exact source commit, compiler, and target Python the bundled wheels were built against:

  ```bash
  tar xzf animus-eval-kit-linux-x86_64.tar.gz
  cd animus-eval-kit-linux-x86_64
  cat MANIFEST.txt
  ```

- [ ] **One-command turnkey pass** — creates an isolated venv, installs the bundled wheels, runs the producer to completion, drains it with the nanobind consumer, and prints a single PASS/FAIL verdict:

  ```bash
  ./run_demo.sh
  ```

- [ ] **Headless, explicit-parameter verification** (for a CI/audit pipeline, or to apply the §2 core pinning above rather than the demo script's un-pinned default):

  ```bash
  ./bin/harness_benchmark --name animus_pilot_eval --events 10000000 \
      --mode overwrite --json producer_report.json

  python3 scripts/verify_stream.py --name animus_pilot_eval \
      --events 10000000 --idle-timeout-s 3
  ```

  `verify_stream.py` exits non-zero on any integrity failure, making it directly usable as a CI gate: `./run_demo.sh || exit 1`.

- [ ] **Telemetry Acceptance Thresholds** — evaluate the printed producer and consumer summary tables against:

  | Metric | Acceptance Threshold | Source |
  |---|---|---|
  | Producer enqueue latency, p50 | **< 25 ns** | `harness_benchmark` stdout / `producer_report.json` |
  | Producer enqueue latency, p99 | **< 35 ns** | `harness_benchmark` stdout / `producer_report.json` |
  | Producer throughput (saturation, overwrite mode) | **> 20M msgs/sec** | `harness_benchmark` stdout |
  | Python zero-copy ingestion, sustained rate | **> 12M frames/sec** | `sdk/python/bench_python_throughput.py` |
  | Frame / sequence corruption | **0** | `verify_stream.py` — `Data integrity: OK` and `Gaps == dropped_count? yes` |

  *These thresholds are the pass bar, not a promise of exact reproduction. A reference run captured on isolated hardware per §2 (see `benchmarks/reports/ANIMUS_BENCHMARK_REPORT.html`) measured p50 21 ns / p99 25 ns enqueue latency, 22.99M events/sec eval-kit throughput, and 14.19M frames/sec sustained Python ingestion — all clearing the thresholds above with margin. A host that has skipped §2 will typically still clear p50/throughput but show materially worse p99.9/max tail latency; that divergence is expected OS-scheduler noise, not by itself a fail condition — see the Methodology Note in `ANIMUS_BENCHMARK_REPORT.html` before escalating on tail figures alone.*

---

## 4. Troubleshooting & Differential Diagnosis Matrix

| Symptom | Likely Root Cause | Diagnostic Command | Resolution |
|---|---|---|---|
| `verify_stream.py` reports sequence gaps with `Gaps == dropped_count? NO` | A real defect, not expected overwrite-mode loss — the consumer's own gap count disagrees with the producer's authoritative drop counter | Re-run with a larger `--idle-timeout-s`; check `dmesg -T \| tail -50` for scheduler/OOM events during the run | If the mismatch persists on a clean, isolated host, this is an escalation to Animus engineering — attach `producer_report.json` and the full `verify_stream.py` output |
| p50 latency is normal but p99.9 / max is very high | Cores are not isolated — the general kernel scheduler is time-slicing or interrupting the pinned thread | `cat /proc/interrupts \| grep -E '^| 2:| 3:'`; `chrt -p $(pgrep harness_benchmark)` | Complete §2 in full (`isolcpus`/`nohz_full`/`rcu_nocbs` + `chrt -f 99`); re-run before comparing against thresholds |
| Latency variance is erratic across otherwise-identical runs (cache false sharing) | Producer and consumer pinned to two hyperthread siblings of the *same* physical core — they share L1/L2, so each side's cache-line traffic evicts the other's | `lscpu -e` — compare the `CORE` column for the two pinned CPU numbers | Re-pin to two distinct **physical** cores, ideally on the same NUMA node (§2.3) |
| Throughput is inconsistent under sustained load (hyperthreading interference) | The SMT sibling of an isolated core is still being scheduled by the OS for unrelated work, stealing execution ports from the pinned thread | `cat /sys/devices/system/cpu/cpu<N>/topology/thread_siblings_list` | Isolate both siblings of each pinned physical core, or disable SMT for the benchmark window: `echo off \| sudo tee /sys/devices/system/cpu/smt/control` |
| `ShmRing::create(...) failed` / permission denied on `/dev/shm` | `/dev/shm` missing, mounted read-only, or too small — common on hardened containers and some CI runners | `df -h /dev/shm`; `ls -ld /dev/shm` | Docker: `--shm-size=64m` or larger; Kubernetes: mount an `emptyDir` volume with `medium: Memory` at `/dev/shm` |
| `"segment with this name already exists"` | A prior run's or crashed process's segment was not cleaned up (the producer does not unlink on exit by default, so a late consumer can still attach) | `ls -la /dev/shm/animus_*` | `rm -f /dev/shm/<segment-name>` before re-running |
| `pip install wheels/animus_native_stream-*.whl` rejects the wheel | The evaluation machine's Python ABI/platform tag doesn't match the interpreter the kit's compiled extension was built against | `python3 --version`; `cat MANIFEST.txt` (records the exact build-time Python) | Install the matching interpreter, or request a kit rebuilt against your target `python3` |
| No hugepage-backed shared memory available / TLB pressure suspected at large ring capacities | Standard `/dev/shm` segments are ordinary `tmpfs`, not `hugetlbfs`-backed — this is expected: the shipped binary does not require or configure hugepages | `grep Hugepagesize /proc/meminfo`; `cat /proc/sys/vm/nr_hugepages` | Not a defect. Reserving explicit `hugetlbfs` pages system-wide is optional, advanced tuning for large-capacity rings only — raise with your Animus point of contact before assuming a hugepage-backed allocation mode exists |

---

## 5. Pilot Evaluation Sign-Off

| Checklist Section | Verified By | Date | Result |
|---|---|---|---|
| §1 — Hardware & Environment Prerequisites | | | ☐ PASS &nbsp;☐ FAIL |
| §2 — Host Isolation & Kernel Tuning | | | ☐ PASS &nbsp;☐ FAIL &nbsp;☐ SKIPPED (functional-only eval) |
| §3 — Air-Gapped Verification Runbook (thresholds met) | | | ☐ PASS &nbsp;☐ FAIL |
| §4 — Escalations opened, if any | | | ☐ NONE &nbsp;☐ OPEN — see attached |

**Overall verdict:** ☐ CLEARED FOR PILOT PROGRAM &nbsp;&nbsp; ☐ ESCALATE TO ANIMUS ENGINEERING BEFORE PROCEEDING

*Retain this completed checklist alongside `producer_report.json` and the full `verify_stream.py` transcript as the audit record for this evaluation window.*
