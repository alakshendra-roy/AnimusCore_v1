// animus_sandbox/soak_harness.cpp
//
// Long-duration soak/stress driver for the same MpmcBoundedQueue<Event>
// primitive benchmark_harness.cpp measures in a single short burst. This
// file instead runs producers/consumers continuously for a caller-supplied
// duration, sampling throughput and end-to-end push-to-drain latency
// percentiles (p50/p90/p99/p99.99) on a fixed interval so tail-latency
// drift or thermal throttling over hours is visible, not just a single
// point-in-time number.
//
// Zero heap allocations on the hot path is enforced, not just claimed:
// operator new/delete below increment g_heap_alloc_count whenever
// g_tracking_hot_path is set, and that flag is only raised after all
// startup allocation (thread objects, the queue's backing vector, the
// histogram arrays) is done. Any allocation during the timed run shows up
// directly in SOAK_FINAL's heap_allocs_during_hot_path field.
//
// Usage: soak_harness <duration_seconds> <interval_seconds> [num_producers] [num_consumers]
#include "ring_buffer.hpp"
#include "sandbox_event.hpp"
#include "tsc_clock.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
    #define NOMINMAX
    #endif
    #include <windows.h>
    #if defined(_MSC_VER)
        #include <intrin.h> // _BitScanReverse64, used only under _MSC_VER in bucket_index_for below
    #endif
#elif defined(__linux__)
    #include <pthread.h>
    #include <sched.h>
#endif

namespace {

// -----------------------------------------------------------------------
// Hot-path heap allocation tracking (see file header).
// -----------------------------------------------------------------------
std::atomic<bool> g_tracking_hot_path{false};
std::atomic<std::int64_t> g_heap_alloc_count{0};

inline void note_alloc() noexcept {
    if (g_tracking_hot_path.load(std::memory_order_relaxed)) {
        g_heap_alloc_count.fetch_add(1, std::memory_order_relaxed);
    }
}

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
// Allocation-free latency histogram, two-tier resolution:
//   - fine buckets: 1ns resolution from 0..kFineBuckets-1 ns
//   - log2 buckets beyond that: kLogBuckets buckets, bucket j covering
//     [kFineBuckets*2^j, kFineBuckets*2^(j+1)) ns, reported as that
//     bucket's lower bound -- a conservative floor, not an exact
//     measurement, which is why bucket_is_exact() exists below.
//   - one true overflow bucket beyond the top log bucket (~137s --
//     effectively "stalled", not a real latency reading)
// An earlier flat 8192-bucket version stopped resolving at 8191ns, and a
// smoke test showed sustained full-throughput runs pegging p50 at that
// single overflow bucket -- true but useless ("somewhere above 8191ns").
// This resolves the full ns-to-~137s range at low, fixed memory cost,
// still with zero heap use on the hot path (see snapshot_and_reset).
// -----------------------------------------------------------------------
constexpr std::size_t kFineBuckets = 8192; // 0..8191 ns, 1ns resolution
constexpr std::size_t kLogBuckets = 24;    // 8192ns .. ~137s, doubling each bucket
constexpr std::size_t kTotalBuckets = kFineBuckets + kLogBuckets + 1;
constexpr std::size_t kTopOverflowIdx = kTotalBuckets - 1;

// Hot path (called once per dequeued event): the fine-bucket case below
// -- the overwhelming majority of real events -- is a bare compare plus
// cast, no floating-point library call. A first version of this soak
// harness called std::log2/std::pow per event for every sample, which
// was itself slow enough (under ASan instrumentation) to make the
// consumer the bottleneck instead of the producer: the ring buffer
// backed up to capacity and every event queued for as long as it took to
// drain ~1M backlogged slots, producing an entirely artificial
// tens-of-milliseconds "latency" that was really just this histogram's
// own measurement overhead. The log-bucket branch (rare -- only taken
// once latency has genuinely left the fine-resolution range) still uses
// one division, but replaces log2() with a single CLZ/BSR instruction.
inline std::size_t bucket_index_for(double latency_ns) noexcept {
    if (latency_ns < static_cast<double>(kFineBuckets)) {
        return static_cast<std::size_t>(latency_ns);
    }
    std::uint64_t ratio = static_cast<std::uint64_t>(latency_ns / static_cast<double>(kFineBuckets));
    if (ratio < 1) ratio = 1;
#if defined(_MSC_VER)
    unsigned long msb = 0;
    _BitScanReverse64(&msb, ratio);
    const unsigned j = static_cast<unsigned>(msb);
#else
    const unsigned j = 63u - static_cast<unsigned>(__builtin_clzll(ratio));
#endif
    if (j >= kLogBuckets) return kTopOverflowIdx;
    return kFineBuckets + j;
}

inline double bucket_floor_ns(std::size_t idx) noexcept {
    if (idx < kFineBuckets) return static_cast<double>(idx);
    if (idx >= kTopOverflowIdx) return static_cast<double>(kFineBuckets) * std::pow(2.0, static_cast<double>(kLogBuckets));
    const std::size_t j = idx - kFineBuckets;
    return static_cast<double>(kFineBuckets) * std::pow(2.0, static_cast<double>(j));
}

inline bool bucket_is_exact(std::size_t idx) noexcept { return idx < kFineBuckets; }

struct LatencyHistogram {
    std::vector<std::atomic<std::uint64_t>> buckets;
    LatencyHistogram() : buckets(kTotalBuckets) {
        for (auto& b : buckets) b.store(0, std::memory_order_relaxed);
    }
    inline void record(double latency_ns) noexcept {
        buckets[bucket_index_for(latency_ns)].fetch_add(1, std::memory_order_relaxed);
    }
    // Snapshots current counts into a caller-owned, already-sized buffer
    // and resets them to zero (interval, not cumulative, stats -- this is
    // what makes drift across hours visible). Deliberately takes a raw
    // pointer rather than returning a std::vector: this runs once per
    // sampling interval for the entire multi-hour soak, and the whole
    // point of the run is proving zero heap allocation anywhere inside
    // the tracked window, sampling code included -- not just inside the
    // producer/consumer loops.
    void snapshot_and_reset(std::uint64_t* out) noexcept {
        for (std::size_t i = 0; i < buckets.size(); ++i) {
            out[i] = buckets[i].exchange(0, std::memory_order_relaxed);
        }
    }
};

struct Percentiles {
    double p50, p90, p99, p9999;      // ns; a value >= kFineBuckets is a log-bucket floor, not exact -- see *_exact flags
    bool p50_exact, p90_exact, p99_exact, p9999_exact;
    bool p9999_is_overflow; // true only for the ~137s top overflow bucket (effectively stalled)
};

Percentiles compute_percentiles(const std::uint64_t* counts, std::size_t count_size) {
    std::uint64_t total = 0;
    for (std::size_t i = 0; i < count_size; ++i) total += counts[i];
    Percentiles out{0, 0, 0, 0, true, true, true, true, false};
    if (total == 0) return out;

    auto find_percentile = [&](double fraction) -> std::size_t {
        const std::uint64_t target = static_cast<std::uint64_t>(fraction * static_cast<double>(total));
        std::uint64_t cumulative = 0;
        for (std::size_t i = 0; i < count_size; ++i) {
            cumulative += counts[i];
            if (cumulative > target) return i;
        }
        return count_size - 1;
    };

    const std::size_t i50 = find_percentile(0.50);
    const std::size_t i90 = find_percentile(0.90);
    const std::size_t i99 = find_percentile(0.99);
    const std::size_t i9999 = find_percentile(0.9999);

    out.p50 = bucket_floor_ns(i50);   out.p50_exact = bucket_is_exact(i50);
    out.p90 = bucket_floor_ns(i90);   out.p90_exact = bucket_is_exact(i90);
    out.p99 = bucket_floor_ns(i99);   out.p99_exact = bucket_is_exact(i99);
    out.p9999 = bucket_floor_ns(i9999); out.p9999_exact = bucket_is_exact(i9999);
    out.p9999_is_overflow = (i9999 == kTopOverflowIdx);
    return out;
}

} // namespace

// operator new/delete overrides (global, unconditional -- the whole point
// is catching allocation anywhere in the process during the tracked
// window, not just inside this translation unit).
void* operator new(std::size_t sz) {
    note_alloc();
    if (void* p = std::malloc(sz)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t sz) {
    note_alloc();
    if (void* p = std::malloc(sz)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

int main(int argc, char** argv) {
    const long duration_s = argc > 1 ? std::atol(argv[1]) : 10800;
    const long interval_s = argc > 2 ? std::atol(argv[2]) : 900;
    const unsigned cpu_count = std::max(2u, std::thread::hardware_concurrency());
    // Deliberately under-subscribed relative to cpu_count (1/3 + 1/3, not
    // 1/2 + 1/2): saturating every logical core with spinning producer +
    // consumer threads leaves zero headroom for the OS scheduler and the
    // main sampling thread, which the first smoke test showed manifests
    // as self-inflicted scheduling latency in the tail (not a queue or
    // sanitizer cost) -- that's noise this soak isn't trying to measure.
    const unsigned num_producers = argc > 3 ? static_cast<unsigned>(std::atoi(argv[3])) : std::max(1u, cpu_count / 3);
    const unsigned num_consumers = argc > 4 ? static_cast<unsigned>(std::atoi(argv[4])) : std::max(1u, cpu_count / 3);
    constexpr std::size_t kCapacity = std::size_t(1) << 20; // 1,048,576 slots

    sandbox::MpmcBoundedQueue<sandbox::Event> queue(kCapacity);
    LatencyHistogram hist;
    std::vector<std::uint64_t> hist_scratch(kTotalBuckets); // pre-allocated once, reused by every sample
    const double cycles_per_ns = sandbox::calibrate_cycles_per_ns();
    const double ns_per_cycle = 1.0 / cycles_per_ns; // precomputed once: a per-event multiply, not a per-event divide

    std::atomic<bool> stop_producing{false};
    std::atomic<bool> drain_complete{false};
    std::atomic<std::uint64_t> produced{0};
    std::atomic<std::uint64_t> consumed{0};
    std::atomic<std::uint64_t> produced_since_sample{0};

    // Bucket-independent cross-check on the histogram-derived percentiles
    // below: plain running min/max, immune to any bucketing bug.
    std::atomic<double> g_min_latency_ns{1e18};
    std::atomic<double> g_max_latency_ns{0.0};
    auto update_min = [&](double v) {
        double cur = g_min_latency_ns.load(std::memory_order_relaxed);
        while (v < cur && !g_min_latency_ns.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {}
    };
    auto update_max = [&](double v) {
        double cur = g_max_latency_ns.load(std::memory_order_relaxed);
        while (v > cur && !g_max_latency_ns.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {}
    };

    std::printf("SOAK_START {\"duration_s\":%ld,\"interval_s\":%ld,\"producers\":%u,\"consumers\":%u,"
                "\"queue_capacity\":%zu,\"cpu_count\":%u,\"cycles_per_ns\":%.4f}\n",
        duration_s, interval_s, num_producers, num_consumers, kCapacity, cpu_count, cycles_per_ns);
    std::fflush(stdout);

    std::vector<std::thread> producers;
    producers.reserve(num_producers);
    for (unsigned t = 0; t < num_producers; ++t) {
        producers.emplace_back([&, t] {
            pin_to_core(static_cast<int>(t % cpu_count));
            std::uint64_t seq = 0;
            while (!stop_producing.load(std::memory_order_relaxed)) {
                sandbox::Event ev{};
                ev.sequence = seq++;
                ev.dispatch_tsc = sandbox::read_tsc();
                ev.producer_id = t;
                ev.value = 0;
                if (queue.enqueue(ev)) {
                    produced.fetch_add(1, std::memory_order_relaxed);
                    produced_since_sample.fetch_add(1, std::memory_order_relaxed);
                } else {
                    std::this_thread::yield(); // ring transiently full under burst; back off, don't spin-corrupt cache lines
                }
            }
        });
    }

    // Latency is sampled, not measured on every dequeue. A first version
    // measured (TSC read + histogram record) on every single event, and a
    // bisection test proved that under ASan instrumentation this made the
    // consumer slower than the producer -- the ring backed up to capacity
    // and every event queued for as long as it took to drain it, producing
    // an entirely artificial tens-of-milliseconds "latency" that was
    // really this harness's own measurement overhead (confirmed by a
    // stripped-down consumer with no per-event measurement keeping
    // perfect pace with the same producer). 1-in-64 sampling is the
    // standard fix for this class of problem in real low-latency
    // telemetry systems: `consumed` is still incremented on every dequeue
    // (cheap, needed for the produced==consumed correctness check), but
    // the TSC read + histogram touch only runs for sampled events.
    constexpr std::uint64_t kSampleMask = 63; // 1-in-64

    std::vector<std::thread> consumers;
    consumers.reserve(num_consumers);
    for (unsigned t = 0; t < num_consumers; ++t) {
        consumers.emplace_back([&, t] {
            pin_to_core(static_cast<int>((num_producers + t) % cpu_count));
            sandbox::Event ev;
            std::uint64_t local_dequeues = 0;
            while (!drain_complete.load(std::memory_order_relaxed)) {
                if (queue.dequeue(ev)) {
                    if ((local_dequeues++ & kSampleMask) == 0) {
                        const std::uint64_t now_tsc = sandbox::read_tsc();
                        const double latency_ns =
                            static_cast<double>(now_tsc - ev.dispatch_tsc) * ns_per_cycle;
                        const double clamped = latency_ns < 0.0 ? 0.0 : latency_ns;
                        hist.record(clamped);
                        update_min(clamped);
                        update_max(clamped);
                    }
                    consumed.fetch_add(1, std::memory_order_relaxed);
                } else {
                    std::this_thread::yield();
                }
            }
        });
    }

    // All startup allocation (queue backing store, histogram vectors,
    // thread objects, the producer/consumer vectors themselves) is done --
    // arm the hot-path allocation tripwire now, before the timed window.
    g_tracking_hot_path.store(true, std::memory_order_relaxed);

    const auto t_start = std::chrono::steady_clock::now();
    const auto t_end = t_start + std::chrono::seconds(duration_s);
    auto t_next_sample = t_start + std::chrono::seconds(interval_s);
    unsigned sample_index = 0;

    while (true) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= t_end) break;
        const auto wake_at = std::min(t_next_sample, t_end);
        std::this_thread::sleep_until(wake_at);
        if (std::chrono::steady_clock::now() < t_next_sample) continue; // woke early for t_end check; loop will exit above
        ++sample_index;
        hist.snapshot_and_reset(hist_scratch.data());
        const auto pct = compute_percentiles(hist_scratch.data(), hist_scratch.size());
        const std::uint64_t interval_pushes = produced_since_sample.exchange(0, std::memory_order_relaxed);
        const double elapsed_since_start_s =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();

        std::printf("SOAK_INTERVAL {\"sample\":%u,\"elapsed_s\":%.1f,\"interval_pushes\":%llu,"
                    "\"throughput_pushes_per_sec\":%.0f,"
                    "\"p50_ns\":%.0f,\"p50_exact\":%s,\"p90_ns\":%.0f,\"p90_exact\":%s,"
                    "\"p99_ns\":%.0f,\"p99_exact\":%s,\"p9999_ns\":%.0f,\"p9999_exact\":%s,"
                    "\"p9999_overflow\":%s,\"heap_allocs_so_far\":%lld,"
                    "\"total_produced\":%llu,\"total_consumed\":%llu}\n",
            sample_index, elapsed_since_start_s,
            static_cast<unsigned long long>(interval_pushes),
            static_cast<double>(interval_pushes) / static_cast<double>(interval_s),
            pct.p50, pct.p50_exact ? "true" : "false",
            pct.p90, pct.p90_exact ? "true" : "false",
            pct.p99, pct.p99_exact ? "true" : "false",
            pct.p9999, pct.p9999_exact ? "true" : "false",
            pct.p9999_is_overflow ? "true" : "false",
            static_cast<long long>(g_heap_alloc_count.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(produced.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(consumed.load(std::memory_order_relaxed)));
        std::fflush(stdout);

        t_next_sample += std::chrono::seconds(interval_s);
    }

    stop_producing.store(true, std::memory_order_relaxed);
    for (auto& th : producers) th.join();

    // Drain whatever is still queued (producers have stopped, so this
    // terminates once consumed catches up to produced -- no fixed sleep).
    while (consumed.load(std::memory_order_relaxed) < produced.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    drain_complete.store(true, std::memory_order_relaxed);
    for (auto& th : consumers) th.join();

    hist.snapshot_and_reset(hist_scratch.data());
    const auto pct = compute_percentiles(hist_scratch.data(), hist_scratch.size());
    g_tracking_hot_path.store(false, std::memory_order_relaxed);
    const double total_elapsed_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();
    const bool correctness_ok = produced.load() == consumed.load();

    std::printf("SOAK_FINAL {\"total_elapsed_s\":%.1f,\"total_produced\":%llu,\"total_consumed\":%llu,"
                "\"correctness_ok\":%s,\"heap_allocs_during_hot_path\":%lld,"
                "\"final_window_p50_ns\":%.0f,\"final_window_p90_ns\":%.0f,\"final_window_p99_ns\":%.0f,"
                "\"final_window_p9999_ns\":%.0f,\"final_window_p9999_exact\":%s,"
                "\"run_min_latency_ns\":%.0f,\"run_max_latency_ns\":%.0f}\n",
        total_elapsed_s,
        static_cast<unsigned long long>(produced.load()),
        static_cast<unsigned long long>(consumed.load()),
        correctness_ok ? "true" : "false",
        static_cast<long long>(g_heap_alloc_count.load(std::memory_order_relaxed)),
        pct.p50, pct.p90, pct.p99, pct.p9999, pct.p9999_exact ? "true" : "false",
        g_min_latency_ns.load(), g_max_latency_ns.load());
    std::fflush(stdout);

    return correctness_ok ? 0 : 1;
}
