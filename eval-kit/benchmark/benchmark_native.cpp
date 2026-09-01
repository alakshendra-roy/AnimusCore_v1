// Animus Engine -- Evaluation Kit
// Native round-trip latency benchmark for eval-kit/include/animus.hpp's
// RingBuffer<T>.
//
// Methodology: two threads, two rings, one ping-pong round trip per
// sample. The producer thread stamps a token with a serialized RDTSC
// reading, pushes it into `request_ring`, then blocks (busy-spins) until
// the same token comes back out of `response_ring`. The consumer thread
// does nothing but pop from `request_ring` and immediately push the same
// token into `response_ring` -- pure echo, no work, so the measured
// interval is dominated by the ring buffer's own cross-core handoff cost
// (cache-coherency traffic + consumer poll latency), not application
// logic. 1,000,000 round trips are timed after a warm-up phase, and the
// per-round-trip cycle counts are reported as a P50/P90/P99/P99.9
// histogram, both in raw TSC cycles and in nanoseconds via a measured
// (not assumed) cycles-per-nanosecond calibration.
//
// RDTSC serialization: __rdtsc() alone can be reordered by the CPU
// relative to surrounding instructions (it is not a serializing
// instruction), which would let work outside the measured region leak
// into the sample. _mm_lfence() is a lightweight serializing fence for
// this purpose (unlike CPUID, it does not trash the pipeline) -- one
// before the start read so nothing prior is still in flight when the
// clock is sampled, and one after the end read so the end timestamp
// itself is not reordered ahead of the ring pop it is meant to follow.

#include "../include/animus.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#if defined(_MSC_VER)
    #include <intrin.h>
#else
    #include <x86intrin.h>
#endif

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

constexpr uint64_t kWarmupRoundTrips = 100'000;
constexpr uint64_t kMeasuredRoundTrips = 1'000'000;
constexpr std::size_t kRingCapacity = 4096;

// Serialized RDTSC: lfence before the read so no earlier instruction is
// still in flight when the clock is sampled, lfence after so the read
// itself cannot be reordered past whatever the caller does next.
inline uint64_t rdtsc_begin() noexcept {
    _mm_lfence();
    const uint64_t t = __rdtsc();
    _mm_lfence();
    return t;
}

inline uint64_t rdtsc_end() noexcept {
    _mm_lfence();
    const uint64_t t = __rdtsc();
    _mm_lfence();
    return t;
}

// Best-effort thread pinning. Returns false (silently) on any platform or
// call failure -- an unpinned benchmark run is still meaningful, just
// noisier, so this never aborts the run.
bool pin_current_thread_to_core(int core_id) noexcept {
    if (core_id < 0) return false;
#if defined(_WIN32)
    const DWORD_PTR mask = static_cast<DWORD_PTR>(1) << core_id;
    return SetThreadAffinityMask(GetCurrentThread(), mask) != 0;
#elif defined(__linux__)
    cpu_set_t cpu_set;
    CPU_ZERO(&cpu_set);
    CPU_SET(core_id, &cpu_set);
    return pthread_setaffinity_np(pthread_self(), sizeof(cpu_set), &cpu_set) == 0;
#else
    (void)core_id;
    return false;
#endif
}

// Measures TSC frequency by racing the TSC against std::chrono's
// steady_clock over a fixed wall-clock window. Assumed, not measured,
// cycle rates are a common source of misleading "ns" figures on systems
// where the TSC does not run at the advertised base clock (turbo,
// power-saving states the invariant TSC is designed to ignore, or
// virtualized/emulated environments) -- measuring it directly avoids
// that whole class of error.
double calibrate_cycles_per_ns() noexcept {
    constexpr auto kCalibrationWindow = std::chrono::milliseconds(200);

    const auto wall_start = std::chrono::steady_clock::now();
    const uint64_t tsc_start = rdtsc_begin();

    while (std::chrono::steady_clock::now() - wall_start < kCalibrationWindow) {
        // busy-wait -- a sleep-based calibration window risks the OS
        // descheduling this thread across a core migration, which would
        // corrupt the TSC delta on non-synchronized-TSC systems.
    }

    const uint64_t tsc_end = rdtsc_end();
    const auto wall_end = std::chrono::steady_clock::now();

    const double elapsed_ns = std::chrono::duration<double, std::nano>(wall_end - wall_start).count();
    const double elapsed_cycles = static_cast<double>(tsc_end - tsc_start);
    return elapsed_cycles / elapsed_ns;
}

struct PercentileReport {
    uint64_t p50 = 0;
    uint64_t p90 = 0;
    uint64_t p99 = 0;
    uint64_t p999 = 0;
    uint64_t min = 0;
    uint64_t max = 0;
};

PercentileReport summarize(std::vector<uint64_t>& samples_cycles) noexcept {
    std::sort(samples_cycles.begin(), samples_cycles.end());
    const std::size_t n = samples_cycles.size();

    auto at_percentile = [&](double p) -> uint64_t {
        std::size_t idx = static_cast<std::size_t>(p * static_cast<double>(n - 1));
        if (idx >= n) idx = n - 1;
        return samples_cycles[idx];
    };

    PercentileReport report;
    report.p50 = at_percentile(0.50);
    report.p90 = at_percentile(0.90);
    report.p99 = at_percentile(0.99);
    report.p999 = at_percentile(0.999);
    report.min = samples_cycles.front();
    report.max = samples_cycles.back();
    return report;
}

void print_row(const char* label, uint64_t cycles, double cycles_per_ns) {
    const double ns = static_cast<double>(cycles) / cycles_per_ns;
    std::printf("  %-8s %10llu cycles   %10.1f ns\n",
        label, static_cast<unsigned long long>(cycles), ns);
}

} // namespace

int main(int argc, char** argv) {
    int producer_core = 0;
    int consumer_core = 1;
    if (argc >= 3) {
        producer_core = std::atoi(argv[1]);
        consumer_core = std::atoi(argv[2]);
    }

    std::printf("Animus Engine -- Native Ring Buffer Round-Trip Benchmark\n");
    std::printf("==========================================================\n");
    std::printf("Warm-up round trips:   %llu\n", static_cast<unsigned long long>(kWarmupRoundTrips));
    std::printf("Measured round trips:  %llu\n", static_cast<unsigned long long>(kMeasuredRoundTrips));
    std::printf("Ring capacity:         %zu slots\n", kRingCapacity);
    std::printf("Requested affinity:    producer=core %d, consumer=core %d\n\n", producer_core, consumer_core);

    animus::RingBuffer<uint64_t> request_ring(kRingCapacity);
    animus::RingBuffer<uint64_t> response_ring(kRingCapacity);

    std::atomic<bool> consumer_ready{ false };
    std::atomic<bool> stop{ false };

    std::thread consumer([&]() {
        pin_current_thread_to_core(consumer_core);
        consumer_ready.store(true, std::memory_order_release);
        uint64_t token;
        while (!stop.load(std::memory_order_relaxed)) {
            if (request_ring.pop(token)) {
                while (!response_ring.push(token)) {
                    // ring momentarily full -- retry; the producer is the
                    // only reader of response_ring and it is about to pop.
                }
            }
        }
    });

    pin_current_thread_to_core(producer_core);
    while (!consumer_ready.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    const double cycles_per_ns = calibrate_cycles_per_ns();
    std::printf("TSC calibration:       %.4f cycles/ns (~%.3f GHz)\n\n", cycles_per_ns, cycles_per_ns);

    // Warm-up: same round-trip path, unmeasured, so instruction/data
    // caches, branch predictors, and the consumer's spin loop are all hot
    // before the timed region begins.
    for (uint64_t i = 0; i < kWarmupRoundTrips; ++i) {
        const uint64_t token = i;
        while (!request_ring.push(token)) {}
        uint64_t echoed;
        while (!response_ring.pop(echoed)) {}
    }

    std::vector<uint64_t> samples_cycles;
    samples_cycles.reserve(kMeasuredRoundTrips);

    for (uint64_t i = 0; i < kMeasuredRoundTrips; ++i) {
        const uint64_t start = rdtsc_begin();
        while (!request_ring.push(start)) {}
        uint64_t echoed;
        while (!response_ring.pop(echoed)) {}
        const uint64_t end = rdtsc_end();
        samples_cycles.push_back(end - start);
    }

    stop.store(true, std::memory_order_relaxed);
    // Unstick the consumer if it is blocked mid-loop waiting on a full
    // response_ring after the last measured sample already drained it.
    uint64_t drain;
    while (response_ring.pop(drain)) {}
    consumer.join();

    const PercentileReport report = summarize(samples_cycles);

    std::printf("Round-trip latency (producer push -> consumer echo -> producer pop):\n");
    std::printf("  ---------------------------------------------\n");
    print_row("min", report.min, cycles_per_ns);
    print_row("p50", report.p50, cycles_per_ns);
    print_row("p90", report.p90, cycles_per_ns);
    print_row("p99", report.p99, cycles_per_ns);
    print_row("p99.9", report.p999, cycles_per_ns);
    print_row("max", report.max, cycles_per_ns);
    std::printf("  ---------------------------------------------\n");
    std::printf("\nNote: this is a full round trip (two ring crossings). Divide by two for a\n");
    std::printf("rough one-way estimate; it will not be exact, since push->pop latency is not\n");
    std::printf("perfectly symmetric under a busy-spin consumer.\n");

    return 0;
}
