// SPDX-License-Identifier: Apache-2.0
// bench_ring_tsc.cpp -- profiles AnimusCore ring operations with bench_tsc.hpp, and checks the profiler.
//
// Six measurements on one pinned thread, each reported as p50/p90/p99/p99.9 in TSC ticks and ns:
//   1. an empty callable: the timer's own floor, what every other row is measured against;
//   2. a spin of exactly 1000 ns by CLOCK_MONOTONIC_RAW: the profiler's self-check. The TSC-derived
//      reading must come out at 1000 ns plus at most about one clock read, which validates the
//      ticks/ns calibration and the timer-cost subtraction against a quantity known independently;
//   3. animus::SpscRingBuffer push+pop on one thread;
//   4. animus::LockFreeRingBuffer (MPMC) push+pop on one thread;
//   5. one clock_gettime(CLOCK_MONOTONIC_RAW) call, for scale;
//   6. animus::sys::ipc::SpscQueue enqueue+dequeue on one thread (capacity 1,024, one 64-byte record).
// Rows 3, 4 and 6 are UNCONTENDED single-thread costs of a push immediately followed by a pop of the same
// record. They say nothing about cross-core latency or throughput under contention, and, as with any
// figure from this tool, are specific to this CPU, this core (hybrid CPUs differ by core type), this
// compiler and this host. Quote a number only with that context, never as a property of the library.
//
// Exit status: 0 if the self-checks pass (pinned and stayed pinned, calibration spread small, the 1000
// ns spin reads 990..1300 ns, ring operations returned what was pushed), 1 if not, 2 on a setup error.
//
// Build and run (Linux x86-64; see bench_tsc.hpp). From the package root:
//     clang++ -std=c++17 -O3 -march=native -Wall -Wextra -Werror -pedantic -pthread -I include src/bench_ring_tsc.cpp -o bench_ring_tsc
//     ./bench_ring_tsc [--cpu N] [--samples N] [--warmup N]
// or: make bench
#include "animus/aligned_block.hpp"
#include "animus/bench_tsc.hpp"
#include "animus/ring_buffers.hpp"
#include "animus/spsc_queue.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include <sched.h>
#include <unistd.h>

namespace {

int g_failures = 0;
void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

// A profiler configured for the fastest possible setup: for tests that care about behaviour, not timing.
animus::bench::ProfilerConfig quick_config(std::size_t samples, int cpu) {
    animus::bench::ProfilerConfig c;
    c.samples = samples;
    c.warmup = 0;
    c.cpu = cpu;
    c.require_invariant_tsc = false;
    c.calibration_windows = 1;
    c.calibration_window_ns = 5000000ull;
    c.overhead_samples = 1000;
    return c;
}

// Nearest-rank percentiles against answers worked out by hand.
bool percentile_math_is_right() {
    using animus::bench::detail::rank_value;
    std::vector<std::uint64_t> thousand(1000);
    for (std::size_t i = 0; i < thousand.size(); ++i) thousand[i] = i + 1;
    const std::uint64_t ten[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
    const std::uint64_t two[] = { 1, 2 };
    const std::uint64_t one[] = { 42 };
    return rank_value(thousand.data(), 1000, 500) == 500 && rank_value(thousand.data(), 1000, 900) == 900 &&
           rank_value(thousand.data(), 1000, 990) == 990 && rank_value(thousand.data(), 1000, 999) == 999 &&
           rank_value(ten, 10, 500) == 5 && rank_value(ten, 10, 900) == 9 && rank_value(ten, 10, 990) == 10 &&
           rank_value(ten, 10, 999) == 10 && rank_value(two, 2, 500) == 1 && rank_value(two, 2, 900) == 2 &&
           rank_value(one, 1, 500) == 42 && rank_value(one, 1, 999) == 42;
}

// p99.9 needs a sample ABOVE its rank to mean anything: n = 999 must say "unresolved", n = 1000 "resolved".
bool p999_flag_flips_at_1000() {
    animus::bench::Profiler small(quick_config(999, animus::bench::ScopedPin::kNoPin));
    animus::bench::Profiler enough(quick_config(1000, animus::bench::ScopedPin::kNoPin));
    if (!small.ok() || !enough.ok()) return false;
    small.run([] { asm volatile("" : : : "memory"); });
    enough.run([] { asm volatile("" : : : "memory"); });
    return !small.finish().p999_resolved && enough.finish().p999_resolved;
}

// A profiler that cannot pin must refuse to measure rather than quietly measure a drifting thread.
bool refuses_when_it_cannot_pin() {
    animus::bench::Profiler beyond_setsize(quick_config(100, 100000));
    animus::bench::Profiler no_such_cpu(quick_config(100, 1000));
    bool ran = false;
    beyond_setsize.run([&] { ran = true; });
    return !beyond_setsize.ok() && beyond_setsize.error()[0] != '\0' && !no_such_cpu.ok() && !ran &&
           beyond_setsize.finish().n == 0;
}

// Pinning narrows the mask to one CPU and must give the original mask back on scope exit.
bool pin_is_undone_on_scope_exit() {
    cpu_set_t before, during, after;
    CPU_ZERO(&before);
    CPU_ZERO(&during);
    CPU_ZERO(&after);
    if (::sched_getaffinity(0, sizeof before, &before) != 0) return false;
    {
        animus::bench::ScopedPin pin;
        if (!pin.pin(animus::bench::ScopedPin::kCurrentCpu)) return false;
        if (::sched_getaffinity(0, sizeof during, &during) != 0) return false;
    }
    if (::sched_getaffinity(0, sizeof after, &after) != 0) return false;
    return CPU_COUNT(&during) == 1 && CPU_EQUAL(&before, &after);
}

// Negative control for the CPU-change detector: an unpinned profiler whose callable moves the thread to
// the other CPU on every call must report (nearly) one change per sample. Returns false on failure;
// `skipped` is set when fewer than two CPUs are allowed, in which case there is nothing to test.
bool cpu_change_detector_sees_forced_migrations(bool& skipped) {
    skipped = false;
    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    if (::sched_getaffinity(0, sizeof allowed, &allowed) != 0) { skipped = true; return true; }
    int a = -1, b = -1;
    for (int c = 0; c < CPU_SETSIZE && b < 0; ++c) {
        if (!CPU_ISSET(c, &allowed)) continue;
        if (a < 0) a = c; else b = c;
    }
    if (b < 0) { skipped = true; return true; }

    constexpr std::size_t kSamples = 2000;
    animus::bench::Profiler prof(quick_config(kSamples, animus::bench::ScopedPin::kNoPin));
    if (!prof.ok()) return false;
    bool to_a = false;
    prof.run([&] {
        cpu_set_t one;
        CPU_ZERO(&one);
        CPU_SET(to_a ? a : b, &one);
        to_a = !to_a;
        (void)::sched_setaffinity(0, sizeof one, &one); // returns on the new CPU
    });
    const animus::bench::LatencyReport r = prof.finish("forced migrations");
    (void)::sched_setaffinity(0, sizeof allowed, &allowed); // put the thread's mask back
    return r.cpu_changes >= kSamples - 2;
}

} // namespace

int main(int argc, char** argv) {
    animus::bench::ProfilerConfig cfg;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--cpu") == 0 && i + 1 < argc) {
            cfg.cpu = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--samples") == 0 && i + 1 < argc) {
            cfg.samples = static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
        } else if (std::strcmp(argv[i], "--warmup") == 0 && i + 1 < argc) {
            cfg.warmup = static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
        } else {
            std::fprintf(stderr, "usage: %s [--cpu N] [--samples N] [--warmup N]\n", argv[0]);
            return 2;
        }
    }

    // Behavioural checks first, while this thread still has its original affinity mask: the profiler
    // below pins the thread, and the detector check needs two CPUs to move between. Verdicts print last.
    const bool math_ok = percentile_math_is_right();
    const bool flag_ok = p999_flag_flips_at_1000();
    const bool refuse_ok = refuses_when_it_cannot_pin();
    const bool restore_ok = pin_is_undone_on_scope_exit();
    bool detector_skipped = false;
    const bool detector_ok = cpu_change_detector_sees_forced_migrations(detector_skipped);

    animus::bench::Profiler prof(cfg);
    if (!prof.ok()) {
        std::fprintf(stderr, "profiler setup failed: %s\n", prof.error());
        return 2;
    }

    // 1. The floor.
    prof.run([] { asm volatile("" : : : "memory"); });
    const animus::bench::LatencyReport floor_report = prof.finish("1. empty callable (the timer's own floor)");
    animus::bench::print_report(floor_report);

    // 2. A spin of 1000 ns by the same clock the TSC was calibrated against.
    prof.run([] {
        const std::uint64_t t0 = animus::bench::mono_raw_ns();
        while (animus::bench::mono_raw_ns() - t0 < 1000) { /* spin */ }
    });
    const animus::bench::LatencyReport spin = prof.finish("2. spin until CLOCK_MONOTONIC_RAW has advanced 1000 ns");
    std::printf("\n");
    animus::bench::print_report(spin);

    // 3. SPSC push+pop of one TelemetryPayload.
    animus::SpscRingBuffer<animus::TelemetryPayload> spsc(1024);
    animus::TelemetryPayload in{}, out{};
    std::uint64_t seq = 0, bad = 0;
    prof.run([&] {
        in.timestamp_cycles = ++seq;
        spsc.push(in);
        spsc.pop(out);
        bad += out.timestamp_cycles != seq;
    });
    const animus::bench::LatencyReport r_spsc = prof.finish("3. animus::SpscRingBuffer<TelemetryPayload>: push + pop, one thread");
    std::printf("\n");
    animus::bench::print_report(r_spsc);

    // 4. MPMC push+pop.
    animus::LockFreeRingBuffer<animus::TelemetryPayload> mpmc(1024);
    std::uint64_t seq2 = 0, bad2 = 0;
    prof.run([&] {
        in.timestamp_cycles = ++seq2;
        mpmc.push(in);
        mpmc.pop(out);
        bad2 += out.timestamp_cycles != seq2;
    });
    const animus::bench::LatencyReport r_mpmc = prof.finish("4. animus::LockFreeRingBuffer<TelemetryPayload>: push + pop, one thread");
    std::printf("\n");
    animus::bench::print_report(r_mpmc);

    // 5. One clock read, for scale.
    std::uint64_t sink = 0;
    prof.run([&] { sink += animus::bench::mono_raw_ns(); });
    const animus::bench::LatencyReport clock_read = prof.finish("5. one clock_gettime(CLOCK_MONOTONIC_RAW)");
    std::printf("\n");
    animus::bench::print_report(clock_read);

    // 6. The header-based SPSC queue. One 64-byte record, capacity 1,024, uncontended, one thread.
    using Payload = animus::TelemetryPayload;
    constexpr std::size_t kCap = 1024;

    // SpscQueue enqueue + dequeue.
    using Queue = animus::sys::ipc::SpscQueue<Payload>;
    animus::bench::AlignedBlock queue_block(Queue::required_bytes(kCap));
    Queue q_prod, q_cons;
    if (!queue_block || !Queue::init(queue_block.data(), queue_block.size(), kCap, q_prod) ||
        !Queue::attach(queue_block.data(), queue_block.size(), q_cons)) {
        std::fprintf(stderr, "SpscQueue setup failed\n");
        return 2;
    }
    std::uint64_t seq3 = 0, bad3 = 0;
    prof.run([&] {
        in.timestamp_cycles = ++seq3;
        q_prod.try_push(in);
        q_cons.try_pop(out);
        bad3 += out.timestamp_cycles != seq3;
    });
    const animus::bench::LatencyReport r_queue = prof.finish("6. animus::sys::ipc::SpscQueue<TelemetryPayload>: enqueue + dequeue, one thread");
    std::printf("\n");
    animus::bench::print_report(r_queue);


    std::printf("\nself-checks\n");
    const animus::bench::LatencyReport* all[] = { &floor_report, &spin, &r_spsc, &r_mpmc, &clock_read, &r_queue };
    bool pinned_ok = true;
    for (const animus::bench::LatencyReport* r : all) {
        if (cfg.cpu != animus::bench::ScopedPin::kNoPin) pinned_ok = pinned_ok && r->pinned && r->pin_held && r->cpu_changes == 0;
    }
    check(pinned_ok, "every run stayed on its pinned CPU (verified by sched_getcpu() and IA32_TSC_AUX)");
    check(floor_report.calibration.valid && floor_report.calibration.spread_ppm < 5000.0,
          "TSC calibration produced a ratio, with under 5000 ppm spread between windows");
    const double spin_ns = spin.ns(spin.net(spin.p50));
    char what[160];
    std::snprintf(what, sizeof what, "the 1000 ns spin reads %.1f ns at p50 (expected 990..1300: 1000 plus about one clock read)", spin_ns);
    check(spin_ns >= 990.0 && spin_ns <= 1300.0, what);
    check(bad == 0 && bad2 == 0 && bad3 == 0 && sink != 0, "ring operations returned the record that was pushed, every time");
    check(math_ok, "nearest-rank p50/p90/p99/p99.9 match hand-worked answers (n = 1, 2, 10, 1000)");
    check(flag_ok, "p99.9 is reported as unresolved at 999 samples and resolved at 1000");
    check(refuse_ok, "a profiler that cannot pin refuses to measure (CPU id out of range, CPU that does not exist)");
    check(restore_ok, "ScopedPin narrows the affinity mask to one CPU and restores the original mask on scope exit");
    check(detector_ok, detector_skipped ? "CPU-change detector: NOT TESTED (this process is allowed fewer than two CPUs)"
                                        : "the CPU-change detector reports a change for (nearly) every forced migration");

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASSED" : "FAILED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
