// Animus Evaluation Kit -- SPSC Ring Buffer Benchmark Harness
//
// Standalone, bare-metal-oriented C++20 benchmark for
// animus::eval::SpscRingBuffer<T> (../include/spsc_ring_buffer.hpp).
// Zero external runtime dependencies beyond pthreads and the C++ standard
// library.
//
// Methodology
// -----------
// Two phases, each pinning the producer thread to Core 2 and the consumer
// thread to Core 3 (overridable via argv) with pthread_setaffinity_np, so
// every cross-thread handoff is a genuine cross-core cache-coherency
// transaction rather than same-core context-switch noise:
//
//   1. Latency phase: the producer stamps each event with a serialized
//      __rdtsc() reading and pushes it; the consumer pops it and
//      immediately stamps the receive side with another serialized
//      __rdtsc() reading. latency = recv_cycles - send_cycles is one ring
//      crossing (enqueue -> cross-core visibility -> dequeue), not a round
//      trip. Both threads busy-spin at full rate with no artificial pacing,
//      so under the steady-state equilibrium of two pinned, uncontended
//      cores the ring stays near-empty and the measured latency reflects
//      cache-coherency handoff cost, not queueing delay. Samples are
//      converted from raw TSC cycles to nanoseconds via a measured (not
//      assumed) cycles-per-nanosecond calibration and reported as a
//      p50/p90/p99/p99.9/max histogram.
//
//   2. Saturation phase: the producer pushes as fast as possible for a
//      fixed wall-clock window; push() never drops -- a full ring is
//      retried, never discarded -- so total consumer-side pops divided by
//      elapsed time is the sustained, zero-drop saturation throughput.
//
// RDTSC serialization
// --------------------
// __rdtsc() alone is not a serializing instruction: the CPU may reorder it
// relative to surrounding instructions, letting work outside the intended
// measurement window leak into a sample. Each timestamp here is wrapped
// with _mm_lfence() (a lightweight serializing fence -- unlike CPUID, it
// does not flush the pipeline) on both sides, plus a std::atomic_thread_
// fence(std::memory_order_seq_cst) compiler fence so the optimizer itself
// cannot hoist or sink surrounding loads/stores across the timestamp. This
// targets an invariant-TSC CPU (all modern x86_64 server/desktop parts);
// __rdtsc() on such a part runs at a fixed rate independent of P-states,
// C-states, and frequency scaling, which is what makes cycle counts
// meaningful without pinning core frequency.

#include "../include/spsc_ring_buffer.hpp"

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

constexpr int kDefaultProducerCore = 2;
constexpr int kDefaultConsumerCore = 3;

constexpr uint64_t kWarmupSamples = 200'000;
constexpr uint64_t kMeasuredLatencySamples = 2'000'000;
constexpr std::size_t kRingCapacity = 4096;
constexpr std::chrono::seconds kSaturationWindow{ 2 };

// One ring slot: a sequence number (unused by the harness itself, but
// present so the payload is more than a bare uint64_t -- representative of
// a small market-data tick) plus the send-side timestamp the consumer
// subtracts its own receive-side timestamp from.
struct alignas(16) Event {
    uint64_t sequence;
    uint64_t send_cycles;
};
static_assert(std::is_trivially_copyable_v<Event>,
    "Event is stored in SpscRingBuffer<T> by plain assignment");

// Serialized RDTSC: an lfence before the read drains any earlier
// instruction still in flight so it cannot be counted inside this sample;
// an lfence after prevents the read itself from being reordered past
// whatever the caller does next. The seq_cst atomic_thread_fence on both
// sides is a pure compiler barrier here (invariant-TSC hardware needs no
// runtime action from it) that stops the optimizer from moving surrounding
// loads/stores across the timestamp.
inline uint64_t rdtsc_fenced() noexcept {
    std::atomic_thread_fence(std::memory_order_seq_cst);
    _mm_lfence();
    const uint64_t t = __rdtsc();
    _mm_lfence();
    std::atomic_thread_fence(std::memory_order_seq_cst);
    return t;
}

// Pins the calling thread to `core_id`. Linux: pthread_setaffinity_np
// against the calling thread's own pthread_self() handle. Windows: best-
// effort SetThreadAffinityMask fallback so the harness still builds and
// runs (unpinned-equivalent precision) on a non-Linux development machine;
// the README documents Linux as the only supported measurement target.
// Returns false (silently -- an unpinned run is still meaningful, just
// noisier) on any platform or call failure.
bool pin_current_thread_to_core(int core_id) noexcept {
    if (core_id < 0) return false;
#if defined(__linux__)
    cpu_set_t cpu_set;
    CPU_ZERO(&cpu_set);
    CPU_SET(core_id, &cpu_set);
    return ::pthread_setaffinity_np(::pthread_self(), sizeof(cpu_set), &cpu_set) == 0;
#elif defined(_WIN32)
    const DWORD_PTR mask = static_cast<DWORD_PTR>(1) << core_id;
    return ::SetThreadAffinityMask(::GetCurrentThread(), mask) != 0;
#else
    (void)core_id;
    return false;
#endif
}

// Measures TSC frequency by racing the TSC against std::chrono::
// steady_clock over a fixed wall-clock window. An assumed (rather than
// measured) cycle rate is a common source of misleading "ns" figures on
// systems where the TSC does not run at the advertised base clock (turbo,
// power-saving states the invariant TSC is designed to ignore, or
// virtualized/emulated environments); measuring it directly avoids that
// whole class of error.
double calibrate_cycles_per_ns() noexcept {
    constexpr auto kCalibrationWindow = std::chrono::milliseconds(200);

    const auto wall_start = std::chrono::steady_clock::now();
    const uint64_t tsc_start = rdtsc_fenced();

    while (std::chrono::steady_clock::now() - wall_start < kCalibrationWindow) {
        // Busy-wait: a sleep-based calibration window risks the OS
        // descheduling this thread across a core migration, which would
        // corrupt the TSC delta on systems without a cross-socket
        // synchronized TSC.
    }

    const uint64_t tsc_end = rdtsc_fenced();
    const auto wall_end = std::chrono::steady_clock::now();

    const double elapsed_ns = std::chrono::duration<double, std::nano>(wall_end - wall_start).count();
    const double elapsed_cycles = static_cast<double>(tsc_end - tsc_start);
    return elapsed_cycles / elapsed_ns;
}

struct PercentileReport {
    uint64_t min = 0;
    uint64_t p50 = 0;
    uint64_t p90 = 0;
    uint64_t p99 = 0;
    uint64_t p999 = 0;
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
    report.min = samples_cycles.front();
    report.p50 = at_percentile(0.50);
    report.p90 = at_percentile(0.90);
    report.p99 = at_percentile(0.99);
    report.p999 = at_percentile(0.999);
    report.max = samples_cycles.back();
    return report;
}

void print_latency_row(const char* label, uint64_t cycles, double cycles_per_ns) {
    const double ns = static_cast<double>(cycles) / cycles_per_ns;
    std::printf("  %-8s %12llu cycles   %12.1f ns\n",
        label, static_cast<unsigned long long>(cycles), ns);
}

} // namespace

int main(int argc, char** argv) {
    int producer_core = kDefaultProducerCore;
    int consumer_core = kDefaultConsumerCore;
    if (argc >= 3) {
        producer_core = std::atoi(argv[1]);
        consumer_core = std::atoi(argv[2]);
    }

    std::printf("Animus Evaluation Kit -- SPSC Ring Buffer Benchmark\n");
    std::printf("====================================================\n");
    std::printf("Warm-up samples:        %llu\n", static_cast<unsigned long long>(kWarmupSamples));
    std::printf("Measured latency samples: %llu\n", static_cast<unsigned long long>(kMeasuredLatencySamples));
    std::printf("Saturation window:      %llds\n", static_cast<long long>(kSaturationWindow.count()));
    std::printf("Ring capacity:          %zu slots\n", kRingCapacity);
    std::printf("Requested affinity:     producer=core %d, consumer=core %d\n\n", producer_core, consumer_core);

    // ---- Phase 1: one-way handoff latency --------------------------------
    {
        animus::eval::SpscRingBuffer<Event> ring(kRingCapacity);

        std::atomic<bool> consumer_ready{ false };
        std::atomic<bool> stop{ false };
        std::vector<uint64_t> latency_samples_cycles;
        latency_samples_cycles.reserve(kMeasuredLatencySamples);

        std::thread consumer([&]() {
            const bool pinned = pin_current_thread_to_core(consumer_core);
            if (!pinned) {
                std::fprintf(stderr, "[warn] consumer: failed to pin to core %d\n", consumer_core);
            }
            consumer_ready.store(true, std::memory_order_release);

            Event ev;
            uint64_t measured = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                if (!ring.pop(ev)) {
                    continue;
                }
                const uint64_t recv_cycles = rdtsc_fenced();
                if (ev.sequence >= kWarmupSamples && measured < kMeasuredLatencySamples) {
                    latency_samples_cycles.push_back(recv_cycles - ev.send_cycles);
                    ++measured;
                }
            }
        });

        const bool producer_pinned = pin_current_thread_to_core(producer_core);
        if (!producer_pinned) {
            std::fprintf(stderr, "[warn] producer: failed to pin to core %d\n", producer_core);
        }
        while (!consumer_ready.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        const double cycles_per_ns = calibrate_cycles_per_ns();
        std::printf("TSC calibration:        %.4f cycles/ns (~%.3f GHz)\n\n", cycles_per_ns, cycles_per_ns);

        const uint64_t total_events = kWarmupSamples + kMeasuredLatencySamples;
        for (uint64_t seq = 0; seq < total_events; ++seq) {
            Event ev{ seq, rdtsc_fenced() };
            while (!ring.push(ev)) {
                // Ring momentarily full -- retry; never drop.
            }
        }

        // Wait for the consumer to drain everything it can see, then stop
        // it. latency_samples_cycles.size() may fall slightly short of
        // kMeasuredLatencySamples if the consumer is still catching up;
        // that is fine for percentile reporting.
        while (ring.size_approx() > 0) {
            std::this_thread::yield();
        }
        stop.store(true, std::memory_order_relaxed);
        consumer.join();

        if (latency_samples_cycles.empty()) {
            std::fprintf(stderr, "error: no latency samples captured\n");
            return 1;
        }

        const PercentileReport report = summarize(latency_samples_cycles);

        std::printf("One-way handoff latency (producer push -> consumer pop), %zu samples:\n",
            latency_samples_cycles.size());
        std::printf("  -------------------------------------------------\n");
        print_latency_row("min", report.min, cycles_per_ns);
        print_latency_row("p50", report.p50, cycles_per_ns);
        print_latency_row("p90", report.p90, cycles_per_ns);
        print_latency_row("p99", report.p99, cycles_per_ns);
        print_latency_row("p99.9", report.p999, cycles_per_ns);
        print_latency_row("max", report.max, cycles_per_ns);
        std::printf("  -------------------------------------------------\n\n");
    }

    // ---- Phase 2: saturation throughput -----------------------------------
    {
        animus::eval::SpscRingBuffer<Event> ring(kRingCapacity);

        std::atomic<bool> consumer_ready{ false };
        std::atomic<bool> stop{ false };
        std::atomic<uint64_t> consumed_count{ 0 };
        uint64_t push_retry_count = 0;

        std::thread consumer([&]() {
            pin_current_thread_to_core(consumer_core);
            consumer_ready.store(true, std::memory_order_release);
            Event ev;
            uint64_t local_consumed = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                if (ring.pop(ev)) {
                    ++local_consumed;
                }
            }
            // Drain whatever the producer published before it observed
            // `stop` -- otherwise the throughput count would silently
            // exclude in-flight events that were never dropped, only not
            // yet read.
            while (ring.pop(ev)) {
                ++local_consumed;
            }
            consumed_count.store(local_consumed, std::memory_order_relaxed);
        });

        pin_current_thread_to_core(producer_core);
        while (!consumer_ready.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        uint64_t sequence = 0;
        const auto run_start = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - run_start < kSaturationWindow) {
            Event ev{ sequence, 0 };
            while (!ring.push(ev)) {
                ++push_retry_count; // ring momentarily full -- retried, never dropped
            }
            ++sequence;
        }
        const auto run_end = std::chrono::steady_clock::now();

        stop.store(true, std::memory_order_relaxed);
        consumer.join();

        const double elapsed_s = std::chrono::duration<double>(run_end - run_start).count();
        const uint64_t consumed = consumed_count.load(std::memory_order_relaxed);
        const double events_per_sec = static_cast<double>(consumed) / elapsed_s;
        const uint64_t dropped = sequence - consumed;

        std::printf("Saturation throughput (%.3fs sustained run):\n", elapsed_s);
        std::printf("  -------------------------------------------------\n");
        std::printf("  events pushed:        %llu\n", static_cast<unsigned long long>(sequence));
        std::printf("  events consumed:      %llu\n", static_cast<unsigned long long>(consumed));
        std::printf("  events dropped:       %llu%s\n", static_cast<unsigned long long>(dropped),
            dropped == 0 ? "  (zero-drop: push() retries under backpressure, never discards)" : "");
        std::printf("  full-ring retries:    %llu\n", static_cast<unsigned long long>(push_retry_count));
        std::printf("  throughput:           %.3f M events/sec\n", events_per_sec / 1'000'000.0);
        std::printf("  -------------------------------------------------\n");
    }

    return 0;
}
