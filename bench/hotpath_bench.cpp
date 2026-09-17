// AnimusCore -- standalone hot-path benchmark harness.
//
// Measures push/pop latency of animus::SpscRingBuffer<T> (the single-producer/
// single-consumer ring buffer defined in animus.hpp) using raw TSC cycle
// counts (__rdtsc()), and reports the P50/P90/P99/P99.9 distribution in both
// cycles and nanoseconds. Zero external dependencies beyond the C++17
// standard library, pthreads, and animus.hpp itself -- no build system, no
// third-party benchmarking library, no CLI parsing.
//
// Build: see build_bench.sh / build_bench.bat in this directory.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <thread>
#include <vector>

#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#include <x86intrin.h>
#else
#error "hotpath_bench.cpp requires an x86/x86_64 target for __rdtsc()."
#endif

#include "animus.hpp"

namespace {

// ---------------------------------------------------------------------
// Heap allocation watchdog: overrides the global operator new/delete to
// count allocations while g_tracking is set, so the timed hot path can
// assert it performed zero heap allocations rather than merely assuming
// it (SpscRingBuffer's push()/pop() do not allocate by design -- see
// animus.hpp -- this is a runtime check of that design guarantee, not a
// substitute for it).
// ---------------------------------------------------------------------
std::atomic<bool> g_tracking{false};
std::atomic<uint64_t> g_alloc_count{0};

inline void record_alloc() {
    if (g_tracking.load(std::memory_order_relaxed)) {
        g_alloc_count.fetch_add(1, std::memory_order_relaxed);
    }
}

} // namespace

// GCC's -Wmismatched-new-delete flags the malloc()/free() pairing below as
// suspicious once it inlines through libstdc++'s allocator at -O3 -- it
// expects a plain operator-new-backed allocation to be freed via operator
// delete, which is exactly what happens here (this operator delete IS the
// one that runs; it just implements itself via free()). False positive for
// this well-known malloc/free-backed override idiom, scoped tightly to
// these six definitions.
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif
void* operator new(std::size_t sz) {
    record_alloc();
    if (void* p = std::malloc(sz ? sz : 1)) {
        return p;
    }
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

// ---------------------------------------------------------------------
// Serialized TSC read: lfence before and after rdtsc pins the read
// against out-of-order execution so it can't drift outside the
// push()/pop() call it's timing -- without this, the CPU can retire the
// rdtsc early or late relative to neighboring instructions and the
// measured interval is meaningless at single-digit-nanosecond scale.
// ---------------------------------------------------------------------
inline uint64_t serialized_rdtsc() {
    _mm_lfence();
    uint64_t t = __rdtsc();
    _mm_lfence();
    return t;
}

#if !defined(_MSC_VER)
bool has_invariant_tsc() {
    unsigned max_ext = __get_cpuid_max(0x80000000, nullptr);
    if (max_ext < 0x80000007) {
        return false;
    }
    unsigned eax, ebx, ecx, edx;
    __cpuid(0x80000007, eax, ebx, ecx, edx);
    return (edx & (1u << 8)) != 0; // CPUID.80000007H:EDX[8] = invariant TSC
}
#endif

// Calibrates TSC cycles-per-nanosecond by racing rdtsc against
// steady_clock over several short windows and taking the median, which
// is far less noisy than a single measurement (a single window can be
// skewed by a scheduler preemption landing inside it).
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

        const double wall_ns =
            std::chrono::duration<double, std::nano>(wall_end - wall_start).count();
        const double tsc_delta = static_cast<double>(tsc_end - tsc_start);
        samples.push_back(tsc_delta / wall_ns);
    }

    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2];
}

struct LatencyStats {
    uint64_t p50;
    uint64_t p90;
    uint64_t p99;
    uint64_t p999;
};

LatencyStats compute_percentiles(std::vector<uint64_t>& samples) {
    std::sort(samples.begin(), samples.end());
    const size_t n = samples.size();
    auto at = [&](double p) -> uint64_t {
        size_t idx = static_cast<size_t>(p * static_cast<double>(n));
        if (idx >= n) idx = n - 1;
        return samples[idx];
    };
    return LatencyStats{at(0.50), at(0.90), at(0.99), at(0.999)};
}

void print_stats_row(const char* label, uint64_t cycles, double cycles_per_ns) {
    std::printf("  %-8s %14llu cyc   %12.2f ns\n",
                label,
                static_cast<unsigned long long>(cycles),
                static_cast<double>(cycles) / cycles_per_ns);
}

} // namespace

int main() {
    constexpr size_t kWarmupIters = 100000;
    constexpr size_t kTimedIters = 1000000;
    constexpr size_t kRingCapacity = 4096; // generous headroom so the consumer
                                            // (draining continuously on its own
                                            // thread) practically never lets the
                                            // ring fill under the producer

    std::printf("=====================================================\n");
    std::printf(" AnimusCore -- SPSC Ring Buffer Hot-Path Benchmark\n");
    std::printf("=====================================================\n");
    std::printf("Warmup iterations : %zu\n", kWarmupIters);
    std::printf("Timed iterations  : %zu\n", kTimedIters);
    std::printf("Ring capacity     : %zu\n\n", kRingCapacity);

#if !defined(_MSC_VER)
    if (!has_invariant_tsc()) {
        std::fprintf(stderr,
                      "WARNING: CPUID 0x80000007:EDX[8] reports no invariant TSC on this "
                      "CPU -- cycle counts below may not be directly comparable across "
                      "cores or CPU frequency states.\n\n");
    }
#endif

    const double cycles_per_ns = calibrate_cycles_per_ns();
    std::printf("Calibrated TSC frequency: %.4f GHz (%.4f cycles/ns)\n\n",
                cycles_per_ns, cycles_per_ns);

    animus::SpscRingBuffer<uint64_t> ring(kRingCapacity);

    std::vector<uint64_t> push_latencies(kTimedIters);
    std::vector<uint64_t> pop_latencies(kTimedIters);

    // Launched before the allocation watchdog is armed: std::thread's own
    // bookkeeping allocates on construction, and that allocation is not
    // part of the hot path under test.
    std::thread consumer([&]() {
        uint64_t val;
        size_t drained = 0;
        while (drained < kWarmupIters) {
            if (ring.pop(val)) {
                ++drained;
            }
        }
        for (size_t i = 0; i < kTimedIters;) {
            uint64_t t0, t1;
            bool ok;
            do {
                t0 = serialized_rdtsc();
                ok = ring.pop(val);
                t1 = serialized_rdtsc();
            } while (!ok);
            pop_latencies[i] = t1 - t0;
            ++i;
        }
    });

    for (size_t i = 0; i < kWarmupIters; ++i) {
        while (!ring.push(static_cast<uint64_t>(i))) {
            // ring momentarily full; spin
        }
    }

    g_alloc_count.store(0, std::memory_order_relaxed);
    g_tracking.store(true, std::memory_order_relaxed);

    for (size_t i = 0; i < kTimedIters; ++i) {
        const uint64_t value = static_cast<uint64_t>(i);
        uint64_t t0, t1;
        bool ok;
        do {
            t0 = serialized_rdtsc();
            ok = ring.push(value);
            t1 = serialized_rdtsc();
        } while (!ok);
        push_latencies[i] = t1 - t0;
    }

    g_tracking.store(false, std::memory_order_relaxed);
    const uint64_t allocs_observed = g_alloc_count.load(std::memory_order_relaxed);

    consumer.join();

    const LatencyStats push_stats = compute_percentiles(push_latencies);
    const LatencyStats pop_stats = compute_percentiles(pop_latencies);

    std::printf("push() latency (single-producer, %zu samples):\n", kTimedIters);
    print_stats_row("P50", push_stats.p50, cycles_per_ns);
    print_stats_row("P90", push_stats.p90, cycles_per_ns);
    print_stats_row("P99", push_stats.p99, cycles_per_ns);
    print_stats_row("P99.9", push_stats.p999, cycles_per_ns);

    std::printf("\npop() latency (single-consumer, %zu samples):\n", kTimedIters);
    print_stats_row("P50", pop_stats.p50, cycles_per_ns);
    print_stats_row("P90", pop_stats.p90, cycles_per_ns);
    print_stats_row("P99", pop_stats.p99, cycles_per_ns);
    print_stats_row("P99.9", pop_stats.p999, cycles_per_ns);

    std::printf("\nHeap allocation check (timed push region, %zu calls): ", kTimedIters);
    if (allocs_observed == 0) {
        std::printf("PASS -- zero heap allocations\n");
    } else {
        std::printf("FAIL -- %llu allocation(s) observed\n",
                     static_cast<unsigned long long>(allocs_observed));
    }

    return allocs_observed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
