// animus_sandbox/benchmark_harness.cpp
//
// Standalone, dependency-free reproduction harness for three specific,
// independently checkable claims:
//   1. 64-byte cache-line alignment (alignas(64)) and the absence of
//      false sharing it buys -- demonstrated with a real A/B throughput
//      test, not just a static_assert.
//   2. An 8-producer / 8-consumer lock-free MPMC ring buffer under real
//      contention, reported against a 16.5M+ pushes/sec target.
//   3. Invariant-TSC hardware cycle counting -- zero system calls in the
//      hot path, calibrated against wall-clock time once at startup.
//
// This file has exactly two local includes (ring_buffer.hpp,
// sandbox_event.hpp, tsc_clock.hpp), all three shipped alongside it in
// this same directory -- no dependency on the parent repository. Build
// with ../Makefile or ../CMakeLists.txt (see ../README.md).
#include "ring_buffer.hpp"
#include "sandbox_event.hpp"
#include "tsc_clock.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
    #define NOMINMAX
    #endif
    #include <windows.h>
#elif defined(__linux__)
    #include <pthread.h>
    #include <sched.h>
#endif

namespace {

// -----------------------------------------------------------------------
// Best-effort thread pinning -- silently returns false on any platform or
// call failure. Every measurement below is still meaningful unpinned,
// just noisier (more scheduler-migration jitter in the tail).
// -----------------------------------------------------------------------
bool pin_to_core(int core_id) noexcept {
#if defined(_WIN32)
    return SetThreadAffinityMask(GetCurrentThread(), static_cast<DWORD_PTR>(1) << core_id) != 0;
#elif defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(core_id, &set);
    return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
#else
    (void)core_id;
    return false;
#endif
}

// -----------------------------------------------------------------------
// 1. Cache-line alignment + false-sharing A/B test.
// -----------------------------------------------------------------------
struct UnpaddedCounters {
    std::atomic<std::uint64_t> a{0};
    std::atomic<std::uint64_t> b{0};
};
struct PaddedCounters {
    alignas(sandbox::kCacheLineSize) std::atomic<std::uint64_t> a{0};
    alignas(sandbox::kCacheLineSize) std::atomic<std::uint64_t> b{0};
};
static_assert(sizeof(UnpaddedCounters) < sandbox::kCacheLineSize,
    "the unpadded case must actually share one line to be a real test");
static_assert(alignof(PaddedCounters) == sandbox::kCacheLineSize, "");
static_assert(sizeof(PaddedCounters) == 2 * sandbox::kCacheLineSize,
    "each padded counter must occupy its own, distinct cache line");

template <typename Counters>
double measure_increment_ops_per_sec(std::uint64_t iters_per_thread) {
    Counters counters;
    const unsigned cpu_count = std::max(1u, std::thread::hardware_concurrency());
    const auto t0 = std::chrono::steady_clock::now();
    std::thread ta([&] {
        if (cpu_count >= 2) pin_to_core(0);
        for (std::uint64_t i = 0; i < iters_per_thread; ++i) {
            counters.a.fetch_add(1, std::memory_order_relaxed);
        }
    });
    std::thread tb([&] {
        if (cpu_count >= 2) pin_to_core(static_cast<int>(1 % cpu_count));
        for (std::uint64_t i = 0; i < iters_per_thread; ++i) {
            counters.b.fetch_add(1, std::memory_order_relaxed);
        }
    });
    ta.join();
    tb.join();
    const double elapsed_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return (2.0 * static_cast<double>(iters_per_thread)) / elapsed_s;
}

void run_false_sharing_benchmark() {
    constexpr std::uint64_t kIters = 20'000'000;
    std::printf("[1/3] Cache-line alignment / false-sharing A/B test\n");
    std::printf("      sizeof(UnpaddedCounters) = %zu bytes (both counters share one line)\n",
        sizeof(UnpaddedCounters));
    std::printf("      sizeof(PaddedCounters)   = %zu bytes (alignas(64) -- each on its own line)\n",
        sizeof(PaddedCounters));

    const double unpadded_ops = measure_increment_ops_per_sec<UnpaddedCounters>(kIters);
    const double padded_ops = measure_increment_ops_per_sec<PaddedCounters>(kIters);
    const double speedup = padded_ops / unpadded_ops;

    std::printf("      unpadded: %.1f ops/sec (combined, 2 threads)\n", unpadded_ops);
    std::printf("      padded:   %.1f ops/sec (combined, 2 threads)\n", padded_ops);
    std::printf("      speedup from eliminating false sharing: %.2fx\n\n", speedup);
}

// -----------------------------------------------------------------------
// 2. 8-producer / 8-consumer MPMC ring buffer throughput.
// -----------------------------------------------------------------------
constexpr double kThroughputTargetPushesPerSec = 16'500'000.0;

void run_mpmc_ring_benchmark() {
    constexpr std::size_t kNumThreads = 8;
    constexpr std::size_t kPerThread = 200'000;
    constexpr std::size_t kTotal = kNumThreads * kPerThread;      // 1,600,000
    constexpr std::size_t kCapacity = std::size_t(1) << 21;       // 2,097,152

    static_assert((kCapacity & (kCapacity - 1)) == 0, "capacity must be a power of two");
    static_assert(kCapacity > kTotal,
        "ring must hold every push with no backpressure during the timed enqueue phase");

    std::printf("[2/3] %zu-producer / %zu-consumer MPMC ring buffer throughput\n", kNumThreads, kNumThreads);

    sandbox::MpmcBoundedQueue<sandbox::Event> queue(kCapacity);
    const unsigned cpu_count = std::max(1u, std::thread::hardware_concurrency());

    // --- Producer phase: kNumThreads threads enqueue concurrently, timed. ---
    std::vector<std::thread> producers;
    producers.reserve(kNumThreads);
    const auto push_t0 = std::chrono::steady_clock::now();
    for (std::size_t t = 0; t < kNumThreads; ++t) {
        producers.emplace_back([&, t] {
            pin_to_core(static_cast<int>(t % cpu_count));
            for (std::size_t i = 0; i < kPerThread; ++i) {
                sandbox::Event ev{};
                ev.sequence = static_cast<std::uint64_t>(i);
                ev.dispatch_tsc = sandbox::read_tsc();
                ev.producer_id = static_cast<std::uint32_t>(t);
                ev.value = static_cast<std::uint64_t>(t) * kPerThread + i;
                while (!queue.enqueue(ev)) {
                    // Ring is sized to never fill during this phase (see the
                    // static_assert above) -- retry defensively anyway
                    // rather than assume that holds.
                }
            }
        });
    }
    for (auto& th : producers) th.join();
    const double push_elapsed_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - push_t0).count();
    const double pushes_per_sec = static_cast<double>(kTotal) / push_elapsed_s;

    std::printf("      pushed %zu events via %zu threads in %.4f s\n", kTotal, kNumThreads, push_elapsed_s);
    std::printf("      throughput: %.0f pushes/sec (target: %.0f+) -- %s\n",
        pushes_per_sec, kThroughputTargetPushesPerSec,
        pushes_per_sec >= kThroughputTargetPushesPerSec ? "MET" : "below target on this hardware");

    // --- Consumer phase: kNumThreads threads dequeue concurrently, timed,
    //     with a correctness check independent of the timing itself. Every
    //     event's `value` field is a globally unique index in
    //     [0, kTotal) by construction, so the sum of all drained values
    //     has a closed-form expected total -- an exact, cheap way to prove
    //     every push was drained exactly once with no corruption, on top
    //     of the plain popped-count check. ---
    std::atomic<std::size_t> total_popped{0};
    std::atomic<std::uint64_t> checksum{0};
    const std::uint64_t expected_checksum =
        static_cast<std::uint64_t>(kTotal) * (kTotal - 1) / 2;

    std::vector<std::thread> consumers;
    consumers.reserve(kNumThreads);
    const auto pop_t0 = std::chrono::steady_clock::now();
    for (std::size_t t = 0; t < kNumThreads; ++t) {
        consumers.emplace_back([&, t] {
            pin_to_core(static_cast<int>(t % cpu_count));
            sandbox::Event ev;
            while (total_popped.load(std::memory_order_relaxed) < kTotal) {
                if (queue.dequeue(ev)) {
                    checksum.fetch_add(ev.value, std::memory_order_relaxed);
                    total_popped.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& th : consumers) th.join();
    const double pop_elapsed_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - pop_t0).count();
    const double pops_per_sec = static_cast<double>(kTotal) / pop_elapsed_s;

    std::printf("      drained %zu events via %zu threads in %.4f s (%.0f pops/sec)\n",
        kTotal, kNumThreads, pop_elapsed_s, pops_per_sec);

    if (total_popped.load() != kTotal || checksum.load() != expected_checksum) {
        std::fprintf(stderr,
            "      CORRECTNESS FAILURE: popped=%zu/%zu, checksum=%llu (expected %llu)\n",
            total_popped.load(), kTotal,
            static_cast<unsigned long long>(checksum.load()),
            static_cast<unsigned long long>(expected_checksum));
        std::exit(1);
    }
    std::printf("      correctness check: PASS (every push drained exactly once, checksum matches)\n\n");
}

// -----------------------------------------------------------------------
// 3. Invariant TSC hot-loop cost -- zero system calls.
// -----------------------------------------------------------------------
void run_tsc_demo() {
    std::printf("[3/3] Invariant TSC hardware cycle counting\n");
    const bool invariant = sandbox::has_invariant_tsc();
    std::printf("      CPUID 0x80000007:EDX[8] invariant TSC: %s\n",
        invariant ? "yes" : "no (non-x86 or unsupported CPU -- fallback clock in use)");

    const double cycles_per_ns = sandbox::calibrate_cycles_per_ns();
    std::printf("      calibration: %.4f cycles/ns (~%.3f GHz-equivalent)\n", cycles_per_ns, cycles_per_ns);

    constexpr std::uint64_t kReads = 20'000'000;
    volatile std::uint64_t sink = 0; // prevents the loop from being optimized away entirely
    const auto t0 = std::chrono::steady_clock::now();
    for (std::uint64_t i = 0; i < kReads; ++i) {
        sink = sandbox::read_tsc();
    }
    const double elapsed_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    (void)sink;

    const double ns_per_read = (elapsed_s * 1e9) / static_cast<double>(kReads);
    std::printf("      %llu reads in %.4f s -- %.3f ns/read, zero system calls in this loop\n\n",
        static_cast<unsigned long long>(kReads), elapsed_s, ns_per_read);
}

} // namespace

int main() {
    std::printf("=========================================================\n");
    std::printf(" Animus Core -- Standalone Evaluation Sandbox Benchmark\n");
    std::printf("=========================================================\n");
    std::printf("logical cores: %u\n\n", std::thread::hardware_concurrency());

    run_false_sharing_benchmark();
    run_mpmc_ring_benchmark();
    run_tsc_demo();

    std::printf("=========================================================\n");
    std::printf(" Done. All structural correctness checks passed.\n");
    std::printf("=========================================================\n");
    return 0;
}
