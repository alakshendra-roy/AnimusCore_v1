// poc_eval_harness.cpp -- institutional POC evaluation harness.
//
// Self-contained, single-file benchmark driven by run_poc_eval.sh /
// run_poc_eval.bat at the repository root. Compiles against
// AnimusCore_v1/animus.hpp (the core single-header engine) only -- no other
// repository headers, no third-party benchmarking library. Reuses the same
// measurement techniques already proven out in bench/hotpath_bench.cpp and
// animus_sandbox/soak_harness.cpp (serialized rdtsc, TSC calibration via a
// steady_clock race, and a global operator-new/delete watchdog that turns
// "zero heap allocation on the hot path" from a claim into a runtime-checked
// fact) rather than reinventing them.
//
// This program measures two distinct things, back to back, inside one
// `duration_seconds`-long run (default 10s, matching the POC soak spec):
//
//   Phase A -- SPSC ingest latency. One producer, one consumer, on
//   animus::SpscRingBuffer<Event> (no CAS, no cross-producer contention --
//   the tightest latency the core ring design can offer). Reports the
//   push()-call latency ("ingest") and, separately, "tick-to-telemetry"
//   latency: wall time from when an event's timestamp was stamped by the
//   producer to when the consumer observed it after pop(), i.e. the full
//   producer -> ring -> consumer path, not just the push() call itself.
//
//   Phase B -- MPMC throughput. N producer threads (default: half the
//   logical cores, minimum 2) contending on animus::LockFreeRingBuffer<Event>
//   (the Vyukov MPMC ring), one consumer draining. Reports aggregate
//   pushes/sec under real multi-producer contention, plus the same latency
//   percentiles as Phase A for comparison.
//
// Every percentile reported comes from samples actually measured on this
// machine during this run -- there is no hardcoded or simulated result path.
// The PASS/FAIL lines at the bottom of the scorecard compare a measured
// number against the target the POC spec names (sub-15ns median ingest,
// sub-100ns median tick-to-telemetry); if the target isn't met on the
// machine this runs on, the scorecard prints FAIL, not a rounded-up number.
//
// Optional core pinning (3rd CLI arg, `pin_base_core`): when given, every
// producer/consumer thread in both phases is pinned via
// animus::sys::pin_current_thread_to_core_exclusive (include/animus/
// thread_affinity.hpp) -- the *_exclusive variant specifically, not the
// plain affinity-only pin, per that header's own documented Phase 14
// finding: pinning a thread to a core without also raising its scheduling
// priority can make P99.99 WORSE, not better, because a preempted-but-
// pinned thread has nowhere else to go until its one core frees up, unlike
// an unpinned thread that can migrate to an idle core. Each phase's threads
// get distinct core indices (see run_spsc_phase/run_mpmc_phase) so the
// producer and consumer never share a physical core with each other.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <thread>
#include <vector>

#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#include <x86intrin.h>
#endif

#include "../include/animus/thread_affinity.hpp"

#include "../AnimusCore_v1/animus.hpp"

namespace {

// ---------------------------------------------------------------------
// Heap allocation watchdog (same idiom as bench/hotpath_bench.cpp and
// animus_sandbox/soak_harness.cpp): overrides global operator new/delete
// to count allocations while g_tracking is armed, so the scorecard's
// "zero heap allocations" line is a runtime-verified fact for THIS run,
// not an assumption carried over from reading the header.
// ---------------------------------------------------------------------
std::atomic<bool> g_tracking{false};
std::atomic<uint64_t> g_alloc_count{0};

inline void record_alloc() {
    if (g_tracking.load(std::memory_order_relaxed)) {
        g_alloc_count.fetch_add(1, std::memory_order_relaxed);
    }
}

} // namespace

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif
void* operator new(std::size_t sz) {
    record_alloc();
    if (void* p = std::malloc(sz ? sz : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t sz) { return ::operator new(sz); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace {

// Event payload: a timestamp stamped at creation (used for the
// "tick-to-telemetry" measurement) plus a sequence number (used to verify
// every pushed event was drained exactly once -- a correctness check, not
// just a throughput number).
struct Event {
    uint64_t seq;
    uint64_t push_tsc;
};

inline uint64_t serialized_rdtsc() {
    _mm_lfence();
    uint64_t t = __rdtsc();
    _mm_lfence();
    return t;
}

bool has_invariant_tsc() {
#if defined(_MSC_VER)
    int regs[4];
    __cpuid(regs, 0x80000000);
    if (static_cast<unsigned>(regs[0]) < 0x80000007u) return false;
    __cpuid(regs, 0x80000007);
    return (regs[3] & (1 << 8)) != 0;
#else
    unsigned max_ext = __get_cpuid_max(0x80000000, nullptr);
    if (max_ext < 0x80000007) return false;
    unsigned eax, ebx, ecx, edx;
    __cpuid(0x80000007, eax, ebx, ecx, edx);
    return (edx & (1u << 8)) != 0;
#endif
}

std::string cpu_brand_string() {
    char brand[49] = {};
#if defined(_MSC_VER)
    int regs[4];
    __cpuid(regs, 0x80000000);
    if (static_cast<unsigned>(regs[0]) < 0x80000004u) return "unknown CPU";
    for (int i = 0; i < 3; ++i) {
        __cpuid(regs, 0x80000002 + i);
        std::memcpy(brand + i * 16, regs, sizeof(regs));
    }
#else
    unsigned max_ext = __get_cpuid_max(0x80000000, nullptr);
    if (max_ext < 0x80000004) return "unknown CPU";
    unsigned regs[4];
    for (int i = 0; i < 3; ++i) {
        __cpuid(0x80000002 + i, regs[0], regs[1], regs[2], regs[3]);
        std::memcpy(brand + i * 16, regs, sizeof(regs));
    }
#endif
    std::string s(brand);
    while (!s.empty() && s.front() == ' ') s.erase(s.begin());
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s.empty() ? "unknown CPU" : s;
}

// Calibrates TSC cycles-per-nanosecond by racing rdtsc against
// steady_clock over several short windows and taking the median.
double calibrate_cycles_per_ns() {
    constexpr int kSamples = 7;
    constexpr auto kWindow = std::chrono::milliseconds(150);
    std::vector<double> samples;
    samples.reserve(kSamples);
    for (int i = 0; i < kSamples; ++i) {
        const auto wall_start = std::chrono::steady_clock::now();
        const uint64_t tsc_start = serialized_rdtsc();
        std::this_thread::sleep_for(kWindow);
        const uint64_t tsc_end = serialized_rdtsc();
        const auto wall_end = std::chrono::steady_clock::now();
        const double wall_ns = std::chrono::duration<double, std::nano>(wall_end - wall_start).count();
        const double tsc_delta = static_cast<double>(tsc_end - tsc_start);
        samples.push_back(tsc_delta / wall_ns);
    }
    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2];
}

struct LatencyStats {
    uint64_t p50, p99, p999;
    size_t sample_count;
};

LatencyStats compute_percentiles(std::vector<uint64_t>& samples) {
    if (samples.empty()) return LatencyStats{0, 0, 0, 0};
    std::sort(samples.begin(), samples.end());
    const size_t n = samples.size();
    auto at = [&](double p) -> uint64_t {
        size_t idx = static_cast<size_t>(p * static_cast<double>(n));
        if (idx >= n) idx = n - 1;
        return samples[idx];
    };
    return LatencyStats{at(0.50), at(0.99), at(0.999), n};
}

void print_ns_row(const char* label, uint64_t cycles, double cycles_per_ns) {
    std::printf("  %-9s %10.2f ns\n", label, static_cast<double>(cycles) / cycles_per_ns);
}

void print_rule() {
    std::printf("---------------------------------------------------------------\n");
}

// ---------------------------------------------------------------------
// Phase A: SPSC ingest + tick-to-telemetry latency.
// Sampling is periodic (every kSampleStride-th event), not every single
// one, so a multi-second run at tens of millions of events/sec doesn't
// grow the sample vectors unboundedly -- the sampled subset is still
// large enough (millions of points for a several-second phase) for
// stable P50/P99/P99.9 estimates.
// ---------------------------------------------------------------------
struct SpscResult {
    LatencyStats push_latency;      // ns, from the timed push() call itself
    LatencyStats tick_to_telemetry; // ns, from event creation to consumer observation
    uint64_t total_pushed;
    uint64_t total_popped;
    bool sequence_intact; // every popped seq strictly increasing, none skipped
    bool pin_requested;
    bool producer_pinned;
    bool consumer_pinned;
};

SpscResult run_spsc_phase(double phase_seconds, double cycles_per_ns, long pin_base_core) {
    constexpr size_t kRingCapacity = 8192;
    constexpr uint64_t kSampleStride = 8;

    animus::SpscRingBuffer<Event> ring(kRingCapacity);

    std::atomic<bool> stop{false};
    std::vector<uint64_t> push_samples;
    std::vector<uint64_t> tick_samples;
    push_samples.reserve(4'000'000);
    tick_samples.reserve(4'000'000);

    std::atomic<uint64_t> total_popped{0};
    std::atomic<bool> sequence_intact{true};
    std::atomic<bool> consumer_pinned{false};

    const bool pin_requested = pin_base_core >= 0;

    std::thread consumer([&]() {
        if (pin_requested) {
            consumer_pinned.store(
                animus::sys::pin_current_thread_to_core_exclusive(static_cast<size_t>(pin_base_core) + 1),
                std::memory_order_relaxed);
        }
        Event ev;
        uint64_t expected_seq = 0;
        for (;;) {
            if (ring.pop(ev)) {
                const uint64_t pop_tsc = serialized_rdtsc();
                if (ev.seq != expected_seq) sequence_intact.store(false, std::memory_order_relaxed);
                expected_seq = ev.seq + 1;
                if ((ev.seq % kSampleStride) == 0 && tick_samples.size() < tick_samples.capacity()) {
                    tick_samples.push_back(pop_tsc - ev.push_tsc);
                }
                total_popped.fetch_add(1, std::memory_order_relaxed);
            } else if (stop.load(std::memory_order_relaxed)) {
                break; // producer is done and the ring is empty -- nothing left to drain
            }
        }
    });

    bool producer_pinned = false;
    if (pin_requested) {
        producer_pinned = animus::sys::pin_current_thread_to_core_exclusive(static_cast<size_t>(pin_base_core));
    }

    g_tracking.store(false, std::memory_order_relaxed);
    g_alloc_count.store(0, std::memory_order_relaxed);
    g_tracking.store(true, std::memory_order_relaxed);

    const auto phase_start = std::chrono::steady_clock::now();
    const auto phase_deadline = phase_start + std::chrono::duration<double>(phase_seconds);
    uint64_t seq = 0;
    while (std::chrono::steady_clock::now() < phase_deadline) {
        const uint64_t t0 = serialized_rdtsc();
        Event ev{seq, t0};
        bool ok;
        do {
            ok = ring.push(ev);
        } while (!ok);
        const uint64_t t1 = serialized_rdtsc();
        if ((seq % kSampleStride) == 0 && push_samples.size() < push_samples.capacity()) {
            push_samples.push_back(t1 - t0);
        }
        ++seq;
    }
    g_tracking.store(false, std::memory_order_relaxed);
    stop.store(true, std::memory_order_relaxed);
    consumer.join();

    SpscResult result;
    result.push_latency = compute_percentiles(push_samples);
    result.tick_to_telemetry = compute_percentiles(tick_samples);
    result.total_pushed = seq;
    result.total_popped = total_popped.load(std::memory_order_relaxed);
    result.sequence_intact = sequence_intact.load(std::memory_order_relaxed) && (result.total_pushed == result.total_popped);
    result.pin_requested = pin_requested;
    result.producer_pinned = producer_pinned;
    result.consumer_pinned = consumer_pinned.load(std::memory_order_relaxed);
    (void)cycles_per_ns;
    return result;
}

// ---------------------------------------------------------------------
// Phase B: MPMC throughput under real multi-producer contention.
// ---------------------------------------------------------------------
struct MpmcResult {
    LatencyStats push_latency;
    uint64_t total_pushed;
    uint64_t total_popped;
    double wall_seconds;
    unsigned producer_threads;
    bool pin_requested;
    bool consumer_pinned;
    unsigned producers_pinned;
};

MpmcResult run_mpmc_phase(double phase_seconds, unsigned producer_threads, long pin_base_core) {
    constexpr size_t kRingCapacity = 1u << 16;
    constexpr uint64_t kSampleStride = 8;

    animus::LockFreeRingBuffer<Event> ring(kRingCapacity);

    std::atomic<bool> stop{false};
    std::atomic<uint64_t> global_seq{0};
    std::atomic<uint64_t> total_popped{0};
    const bool pin_requested = pin_base_core >= 0;
    std::atomic<bool> consumer_pinned{false};
    std::atomic<unsigned> producers_pinned{0};

    std::vector<std::vector<uint64_t>> per_thread_samples(producer_threads);
    for (auto& v : per_thread_samples) v.reserve(2'000'000 / (producer_threads ? producer_threads : 1));

    std::thread consumer([&]() {
        if (pin_requested) {
            // Consumer gets pin_base_core; producers get pin_base_core+1..
            // +producer_threads (see below) -- distinct cores so the
            // consumer never shares a physical core with any producer.
            consumer_pinned.store(
                animus::sys::pin_current_thread_to_core_exclusive(static_cast<size_t>(pin_base_core)),
                std::memory_order_relaxed);
        }
        Event ev;
        while (!stop.load(std::memory_order_relaxed)) {
            if (ring.pop(ev)) total_popped.fetch_add(1, std::memory_order_relaxed);
        }
        while (ring.pop(ev)) total_popped.fetch_add(1, std::memory_order_relaxed);
    });

    std::vector<std::thread> producers;
    producers.reserve(producer_threads);

    g_tracking.store(false, std::memory_order_relaxed);
    g_alloc_count.store(0, std::memory_order_relaxed);

    // Producer threads are constructed (and spin-wait on `start`) BEFORE the
    // allocation watchdog is armed -- std::thread's own internal bookkeeping
    // allocates on construction on some standard-library implementations,
    // and that startup allocation is not part of the hot path under test
    // (same reasoning bench/hotpath_bench.cpp documents for its consumer
    // thread). Only the timed push()/pop() region below is tracked. Pinning
    // itself (a direct OS syscall, no heap allocation) is safe to perform
    // either side of the watchdog arm -- done here, before the `start` wait,
    // purely so a pin failure is resolved well before the timed region.
    std::atomic<bool> start{false};
    std::chrono::steady_clock::time_point phase_deadline{};
    for (unsigned t = 0; t < producer_threads; ++t) {
        producers.emplace_back([&, t]() {
            if (pin_requested) {
                if (animus::sys::pin_current_thread_to_core_exclusive(static_cast<size_t>(pin_base_core) + 1 + t)) {
                    producers_pinned.fetch_add(1, std::memory_order_relaxed);
                }
            }
            std::vector<uint64_t>& samples = per_thread_samples[t];
            while (!start.load(std::memory_order_acquire)) {
                // wait for the deadline to be published
            }
            while (std::chrono::steady_clock::now() < phase_deadline) {
                const uint64_t seq = global_seq.fetch_add(1, std::memory_order_relaxed);
                const uint64_t t0 = serialized_rdtsc();
                Event ev{seq, t0};
                while (!ring.push(ev)) {
                    // ring momentarily full under contention; spin
                }
                const uint64_t t1 = serialized_rdtsc();
                if ((seq % kSampleStride) == 0 && samples.size() < samples.capacity()) {
                    samples.push_back(t1 - t0);
                }
            }
        });
    }

    // All producer threads exist and are spinning on `start` -- arm the
    // watchdog and publish the deadline together, then release them.
    const auto phase_start = std::chrono::steady_clock::now();
    phase_deadline = phase_start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(phase_seconds));
    g_tracking.store(true, std::memory_order_relaxed);
    start.store(true, std::memory_order_release);

    for (auto& p : producers) p.join();
    const auto wall_end = std::chrono::steady_clock::now();
    g_tracking.store(false, std::memory_order_relaxed);

    stop.store(true, std::memory_order_relaxed);
    consumer.join();

    std::vector<uint64_t> all_samples;
    for (auto& v : per_thread_samples) {
        all_samples.insert(all_samples.end(), v.begin(), v.end());
    }

    MpmcResult result;
    result.push_latency = compute_percentiles(all_samples);
    result.total_pushed = global_seq.load(std::memory_order_relaxed);
    result.total_popped = total_popped.load(std::memory_order_relaxed);
    result.wall_seconds = std::chrono::duration<double>(wall_end - phase_start).count();
    result.producer_threads = producer_threads;
    result.pin_requested = pin_requested;
    result.consumer_pinned = consumer_pinned.load(std::memory_order_relaxed);
    result.producers_pinned = producers_pinned.load(std::memory_order_relaxed);
    return result;
}

} // namespace

int main(int argc, char** argv) {
    double duration_seconds = 10.0;
    if (argc > 1) {
        const double parsed = std::atof(argv[1]);
        if (parsed > 0.0) duration_seconds = parsed;
    }

    unsigned hw_threads = std::thread::hardware_concurrency();
    if (hw_threads == 0) hw_threads = 4;
    unsigned producer_threads = hw_threads > 4 ? hw_threads / 2 : 2;
    if (argc > 2) {
        const int parsed = std::atoi(argv[2]);
        if (parsed > 0) producer_threads = static_cast<unsigned>(parsed);
    }

    // Optional 3rd arg: base logical core index for exclusive pinning (see
    // this file's header comment). -1 (default) disables pinning entirely,
    // reproducing an ordinary unpinned run for comparison.
    long pin_base_core = -1;
    if (argc > 3) {
        pin_base_core = std::atol(argv[3]);
        if (pin_base_core < 0) pin_base_core = -1;
    }

    const double phase_seconds = duration_seconds / 2.0;

    print_rule();
    std::printf(" AnimusCore -- POC Evaluation Harness\n");
    print_rule();
    std::printf("CPU                : %s\n", cpu_brand_string().c_str());
    std::printf("Logical cores      : %u\n", hw_threads);
    const bool invariant_tsc = has_invariant_tsc();
    std::printf("Invariant TSC      : %s\n", invariant_tsc ? "yes (CPUID.80000007H:EDX[8])" : "NOT DETECTED -- cross-core cycle comparisons may be unreliable");
    const double cycles_per_ns = calibrate_cycles_per_ns();
    std::printf("TSC frequency      : %.4f GHz (calibrated against steady_clock)\n", cycles_per_ns);
    std::printf("Requested duration : %.1f s (%.1f s Phase A / %.1f s Phase B)\n", duration_seconds, phase_seconds, phase_seconds);
    std::printf("MPMC producers     : %u threads + 1 consumer\n", producer_threads);
    if (pin_base_core >= 0) {
        std::printf("Core pinning       : ENABLED, base core %ld (animus::sys::pin_current_thread_to_core_exclusive)\n\n", pin_base_core);
    } else {
        std::printf("Core pinning       : disabled (unpinned run -- pass a 3rd arg, e.g. `2`, to enable)\n\n");
    }

    std::printf("Phase A: SPSC ingest latency (animus::SpscRingBuffer<Event>)...\n");
    const SpscResult spsc = run_spsc_phase(phase_seconds, cycles_per_ns, pin_base_core);
    const uint64_t spsc_allocs = g_alloc_count.load(std::memory_order_relaxed);

    std::printf("Phase B: MPMC throughput under contention (animus::LockFreeRingBuffer<Event>)...\n\n");
    const MpmcResult mpmc = run_mpmc_phase(phase_seconds, producer_threads, pin_base_core);
    const uint64_t mpmc_allocs = g_alloc_count.load(std::memory_order_relaxed);

    print_rule();
    std::printf(" SCORECARD\n");
    print_rule();

    std::printf("[Phase A -- SPSC Ingest]\n");
    if (spsc.pin_requested) {
        std::printf("  core pinning         : producer=%s consumer=%s\n",
                    spsc.producer_pinned ? "PINNED" : "FAILED", spsc.consumer_pinned ? "PINNED" : "FAILED");
    }
    std::printf("  events pushed        : %llu\n", static_cast<unsigned long long>(spsc.total_pushed));
    std::printf("  events popped        : %llu\n", static_cast<unsigned long long>(spsc.total_popped));
    std::printf("  sequence intact      : %s\n", spsc.sequence_intact ? "yes (every event drained exactly once, in order)" : "NO -- loss or reorder detected");
    std::printf("  push() latency (n=%zu samples):\n", spsc.push_latency.sample_count);
    print_ns_row("P50", spsc.push_latency.p50, cycles_per_ns);
    print_ns_row("P99", spsc.push_latency.p99, cycles_per_ns);
    print_ns_row("P99.9", spsc.push_latency.p999, cycles_per_ns);
    std::printf("  tick-to-telemetry latency (creation -> consumer observed, n=%zu samples):\n", spsc.tick_to_telemetry.sample_count);
    print_ns_row("P50", spsc.tick_to_telemetry.p50, cycles_per_ns);
    print_ns_row("P99", spsc.tick_to_telemetry.p99, cycles_per_ns);
    print_ns_row("P99.9", spsc.tick_to_telemetry.p999, cycles_per_ns);
    std::printf("  heap allocations during timed region: %s\n\n",
                spsc_allocs == 0 ? "0 (PASS)" : "FAIL -- see count above");

    std::printf("[Phase B -- MPMC Throughput]\n");
    if (mpmc.pin_requested) {
        std::printf("  core pinning         : producers=%u/%u consumer=%s\n",
                    mpmc.producers_pinned, mpmc.producer_threads, mpmc.consumer_pinned ? "PINNED" : "FAILED");
    }
    std::printf("  producer threads     : %u\n", mpmc.producer_threads);
    std::printf("  wall time            : %.3f s\n", mpmc.wall_seconds);
    std::printf("  events pushed        : %llu\n", static_cast<unsigned long long>(mpmc.total_pushed));
    std::printf("  events popped        : %llu\n", static_cast<unsigned long long>(mpmc.total_popped));
    const double pushes_per_sec = mpmc.wall_seconds > 0.0 ? static_cast<double>(mpmc.total_pushed) / mpmc.wall_seconds : 0.0;
    std::printf("  MPMC pushes/sec      : %.0f\n", pushes_per_sec);
    std::printf("  push() latency under contention (n=%zu samples):\n", mpmc.push_latency.sample_count);
    print_ns_row("P50", mpmc.push_latency.p50, cycles_per_ns);
    print_ns_row("P99", mpmc.push_latency.p99, cycles_per_ns);
    print_ns_row("P99.9", mpmc.push_latency.p999, cycles_per_ns);
    std::printf("  heap allocations during timed region: %s\n\n",
                mpmc_allocs == 0 ? "0 (PASS)" : "FAIL -- see count above");

    print_rule();
    std::printf(" TARGET VALIDATION (measured on this machine, this run)\n");
    print_rule();
    const double spsc_p50_ns = static_cast<double>(spsc.push_latency.p50) / cycles_per_ns;
    const double tick_p50_ns = static_cast<double>(spsc.tick_to_telemetry.p50) / cycles_per_ns;
    const bool ingest_pass = spsc_p50_ns < 15.0;
    const bool tick_pass = tick_p50_ns < 100.0;
    std::printf("  P50 SPSC ingest latency       : %8.2f ns   target < 15 ns    [%s]\n",
                spsc_p50_ns, ingest_pass ? "PASS" : "FAIL");
    std::printf("  P50 tick-to-telemetry latency : %8.2f ns   target < 100 ns   [%s]\n",
                tick_p50_ns, tick_pass ? "PASS" : "FAIL");
    std::printf("  Heap allocations (both phases): %8llu           target = 0        [%s]\n",
                static_cast<unsigned long long>(spsc_allocs + mpmc_allocs),
                (spsc_allocs + mpmc_allocs) == 0 ? "PASS" : "FAIL");
    std::printf("  Event integrity (Phase A)     : %8s           target = intact   [%s]\n",
                spsc.sequence_intact ? "intact" : "BROKEN",
                spsc.sequence_intact ? "PASS" : "FAIL");
    print_rule();
    if (pin_base_core >= 0) {
        std::printf("Note: this run used core pinning + elevated thread priority (see \"core\n");
        std::printf("pinning\" lines above) but NOT kernel-level isolation (isolcpus/nohz_full/\n");
        std::printf("rcu_nocbs, Linux-only) or a NUMA-pinned allocation -- results are still\n");
        std::printf("specific to this host and background load, not a Reference Topology number.\n");
        std::printf("See docs/technical_eval/COMPATIBILITY_TUNING_GUIDE.md Sec.2 for full kernel\n");
        std::printf("tuning beyond what this process can do on its own.\n");
    } else {
        std::printf("Note: results are specific to the CPU, OS scheduler state, and background\n");
        std::printf("load of the machine this ran on. Re-run with a dedicated/isolated core\n");
        std::printf("(see docs/technical_eval/COMPATIBILITY_TUNING_GUIDE.md, and pass a 3rd CLI\n");
        std::printf("arg to enable this harness's own core pinning) for production-representative\n");
        std::printf("numbers -- a shared, non-isolated core under a general-purpose OS scheduler\n");
        std::printf("will show materially higher tail latency than this.\n");
    }

    const bool overall_pass = ingest_pass && tick_pass && (spsc_allocs + mpmc_allocs) == 0 && spsc.sequence_intact;
    return overall_pass ? EXIT_SUCCESS : EXIT_FAILURE;
}
