// Animus Core -- Institutional Ingestion Benchmark Harness (animus_bench)
//
// Single-producer/single-consumer throughput and tail-latency proof-point,
// built for live screen-share technical evaluations with trading desks and
// fintech founders. It measures ingress-to-egress transit time across the
// zero-copy SPSC ring buffer (animus-eval-kit/include/spsc_ring_buffer.hpp)
// under synthetic market-tick load, and proves -- not just claims -- zero
// dynamic heap allocation on the producer/consumer hot path.
//
// Zero-allocation proof strategy: operator new/delete are replaced (below)
// with instrumented wrappers that count every call, process-wide. Both
// worker threads run a warm-up handshake (pin to core, raise priority,
// signal "ready", then spin-wait) before the timed run starts, so every
// setup-phase allocation (thread launch, ring buffer backing store, iostream
// lazy init) completes and is captured in a *baseline* snapshot taken right
// before the timed window opens. The harness then re-reads the same counters
// immediately after both threads finish and reports the difference -- the
// count is a measurement of the exact hot-path window, not the whole
// process's lifetime.
//
// Percentiles are computed from a fixed-size, statically-allocated
// nanosecond histogram (LatencyHistogram below) rather than a per-sample
// buffer, so recording a latency sample on the consumer's hot path is a
// single array increment -- no heap growth regardless of run length.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <new>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>

#include "animus/thread_affinity.hpp"
#include "spsc_ring_buffer.hpp"

// ---------------------------------------------------------------------------
// Zero-allocation guard: replaces all six standard replaceable operator
// new/delete overloads with malloc/free plus an atomic call counter. See the
// header comment above for how the "hot-path" window is isolated from
// legitimate setup-phase allocations.
// ---------------------------------------------------------------------------
namespace {
std::atomic<uint64_t> g_heap_alloc_ops{0};
std::atomic<uint64_t> g_heap_dealloc_ops{0};
} // namespace

void* operator new(std::size_t size) {
    g_heap_alloc_ops.fetch_add(1, std::memory_order_relaxed);
    void* ptr = std::malloc(size == 0 ? 1 : size);
    if (ptr == nullptr) {
        throw std::bad_alloc();
    }
    return ptr;
}

void* operator new[](std::size_t size) {
    return ::operator new(size);
}

void operator delete(void* ptr) noexcept {
    g_heap_dealloc_ops.fetch_add(1, std::memory_order_relaxed);
    std::free(ptr);
}

void operator delete(void* ptr, std::size_t) noexcept {
    ::operator delete(ptr);
}

void operator delete[](void* ptr) noexcept {
    ::operator delete(ptr);
}

void operator delete[](void* ptr, std::size_t size) noexcept {
    ::operator delete(ptr, size);
}

namespace animus_bench {

// ---------------------------------------------------------------------------
// Wire frame: fixed-size, single-cache-line telemetry record. alignas(64)
// plus the natural field layout below pads sizeof(TelemetryFrame) up to
// exactly 64 bytes, so one frame never straddles two cache lines and an
// array of frames never causes false sharing between adjacent elements.
// ---------------------------------------------------------------------------
struct alignas(64) TelemetryFrame {
    uint64_t sequence_id;
    uint64_t timestamp_ns;
    char symbol[8];
    double price;
    uint32_t volume;
    uint8_t flags;
};
static_assert(sizeof(TelemetryFrame) == 64, "TelemetryFrame must occupy exactly one cache line");
static_assert(std::is_trivially_copyable_v<TelemetryFrame>, "TelemetryFrame must be trivially copyable for the SPSC ring");

constexpr uint8_t kFlagBurst = 0x01;

// ---------------------------------------------------------------------------
// Fixed-size nanosecond latency histogram. Static storage duration (lives in
// BSS, zero-initialized, no heap, no thread-stack pressure) so recording a
// sample on the consumer hot path costs one bounds check and one increment.
// Min/Max are tracked exactly regardless of bucket range; percentiles below
// kMaxTrackedNs are bucket-accurate to within kBucketWidthNs; a percentile
// that falls past the tracked range is reported as the true max instead of a
// silently wrong bucket value.
// ---------------------------------------------------------------------------
struct LatencyHistogram {
    static constexpr uint64_t kBucketWidthNs = 4;
    static constexpr uint64_t kNumBuckets = 131072; // 4 ns * 131072 = ~524,288 ns range
    static constexpr uint64_t kMaxTrackedNs = kBucketWidthNs * kNumBuckets;

    std::array<uint64_t, kNumBuckets> buckets{};
    uint64_t overflow_count = 0;
    uint64_t total_count = 0;
    uint64_t min_ns = UINT64_MAX;
    uint64_t max_ns = 0;

    void record(uint64_t latency_ns) noexcept {
        ++total_count;
        if (latency_ns < min_ns) min_ns = latency_ns;
        if (latency_ns > max_ns) max_ns = latency_ns;
        const uint64_t idx = latency_ns / kBucketWidthNs;
        if (idx < kNumBuckets) {
            ++buckets[idx];
        } else {
            ++overflow_count;
        }
    }

    uint64_t percentile(double pct) const noexcept {
        if (total_count == 0) return 0;
        const auto target = static_cast<uint64_t>(std::ceil((pct / 100.0) * static_cast<double>(total_count)));
        uint64_t cumulative = 0;
        for (uint64_t i = 0; i < kNumBuckets; ++i) {
            cumulative += buckets[i];
            if (cumulative >= target) {
                return i * kBucketWidthNs + (kBucketWidthNs / 2);
            }
        }
        return max_ns; // target fell in the overflow tail -- true max is the honest answer
    }
};

// Static storage: kNumBuckets * 8 bytes = ~1 MiB. Deliberately not a stack
// local (would eat a large slice of a default 1 MiB Windows thread stack)
// and not heap-allocated (would show up in the zero-allocation guard).
LatencyHistogram g_histogram;

struct BenchConfig {
    uint64_t rate = 10'000'000ULL;
    double duration_s = 3.0;
    bool burst = false;
};

struct RunResult {
    uint64_t frames_produced = 0;
    uint64_t frames_consumed = 0;
    uint64_t sequence_corruptions = 0;
    uint64_t hot_path_allocs = 0;
    uint64_t hot_path_deallocs = 0;
    double elapsed_s = 0.0;
    int producer_core = -1;
    int consumer_core = -1;
};

constexpr std::size_t kRingCapacity = 1u << 20; // 1,048,576 frames (~64 MiB backing store)

void print_usage(const char* argv0) {
    std::fprintf(stderr,
        "Usage: %s [--rate <msgs/sec>] [--duration <seconds>] [--burst]\n"
        "  --rate <msgs/sec>   Target sustained ingest rate (default: 10000000; supported: 5000000-15000000)\n"
        "  --duration <secs>   Length of the timed run in seconds (default: 3.0)\n"
        "  --burst             Emit synthetic market-depth bursts instead of a flat sustained rate\n",
        argv0);
}

bool parse_args(int argc, char** argv, BenchConfig& cfg) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--rate") {
            if (i + 1 >= argc) { std::fprintf(stderr, "error: --rate requires a value\n"); return false; }
            cfg.rate = std::strtoull(argv[++i], nullptr, 10);
        } else if (arg == "--duration") {
            if (i + 1 >= argc) { std::fprintf(stderr, "error: --duration requires a value\n"); return false; }
            cfg.duration_s = std::strtod(argv[++i], nullptr);
        } else if (arg == "--burst") {
            cfg.burst = true;
        } else if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            std::exit(0);
        } else {
            std::fprintf(stderr, "error: unrecognized argument '%s'\n", argv[i]);
            return false;
        }
    }
    if (cfg.rate == 0) {
        std::fprintf(stderr, "error: --rate must be > 0\n");
        return false;
    }
    if (cfg.duration_s <= 0.0) {
        std::fprintf(stderr, "error: --duration must be > 0\n");
        return false;
    }
    if (cfg.rate < 5'000'000ULL || cfg.rate > 15'000'000ULL) {
        std::fprintf(stderr, "note: --rate %" PRIu64 " is outside the supported 5,000,000-15,000,000 msgs/sec range; proceeding anyway.\n", cfg.rate);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Producer: paces synthetic market-tick frames at the target rate (or emits
// unpaced bursts with brief idle gaps in --burst mode), publishing into the
// SPSC ring. push() is retried (never dropped) on a full ring, so throughput
// backpressure shows up as latency, never as a lost frame.
// ---------------------------------------------------------------------------
void producer_thread_fn(animus::eval::SpscRingBuffer<TelemetryFrame>& ring,
                         const std::atomic<bool>& start_flag,
                         std::atomic<bool>& producer_ready,
                         std::atomic<bool>& producer_done,
                         std::atomic<int>& workers_finished,
                         const std::atomic<bool>& may_exit,
                         const BenchConfig& cfg,
                         int core_id,
                         uint64_t& frames_produced_out) {
    if (core_id >= 0) {
        animus::sys::pin_current_thread_to_core(static_cast<std::size_t>(core_id));
    }
    animus::sys::set_thread_high_priority();

    static constexpr const char* kSymbols[] = {"AAPL", "MSFT", "GOOG", "AMZN", "TSLA", "NVDA", "META", "BRKB"};
    constexpr int kNumSymbols = 8;
    constexpr uint64_t kBurstFrames = 4096;

    producer_ready.store(true, std::memory_order_release);
    while (!start_flag.load(std::memory_order_acquire)) {
        animus::cpu_relax();
    }

    const auto period = std::chrono::nanoseconds(1'000'000'000ULL / cfg.rate);
    const auto run_start = std::chrono::steady_clock::now();
    const auto run_end = run_start + std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::duration<double>(cfg.duration_s));
    auto next_deadline = run_start;

    uint64_t seq = 0;
    uint64_t rng_state = 0x9E3779B97F4A7C15ULL;
    double price = 100.0;
    uint64_t burst_counter = 0;

    while (std::chrono::steady_clock::now() < run_end) {
        rng_state ^= rng_state << 13;
        rng_state ^= rng_state >> 7;
        rng_state ^= rng_state << 17;

        TelemetryFrame frame{};
        frame.sequence_id = seq;
        const char* sym = kSymbols[rng_state % kNumSymbols];
        std::memcpy(frame.symbol, sym, std::strlen(sym) < sizeof(frame.symbol) ? std::strlen(sym) : sizeof(frame.symbol));

        const double drift = (static_cast<double>((rng_state >> 32) % 2001) - 1000.0) * 0.0001;
        price += drift;
        if (price < 1.0) price = 1.0;
        frame.price = price;
        frame.volume = static_cast<uint32_t>(100 + (rng_state % 9900));
        frame.flags = cfg.burst ? kFlagBurst : 0x00;

        // Timestamp taken immediately before publish -- the closest available
        // approximation of true wire ingress time for this synthetic frame.
        frame.timestamp_ns = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());

        while (!ring.push(frame)) {
            animus::cpu_relax();
        }
        ++seq;

        if (cfg.burst) {
            if (++burst_counter >= kBurstFrames) {
                burst_counter = 0;
                const auto gap_end = std::chrono::steady_clock::now() + std::chrono::microseconds(200);
                while (std::chrono::steady_clock::now() < gap_end) {
                    animus::cpu_relax();
                }
            }
        } else {
            next_deadline += period;
            while (std::chrono::steady_clock::now() < next_deadline) {
                animus::cpu_relax();
            }
        }
    }

    frames_produced_out = seq;
    producer_done.store(true, std::memory_order_release);

    // Hold this OS thread alive (no return -> no runtime thread-exit/TLS
    // teardown yet) until main has captured the post-run allocation
    // snapshot. Without this barrier, one thread's exit-time cleanup (e.g.
    // the C runtime freeing a per-thread errno/locale block) can race with
    // and pollute the other thread's still-open measurement window, since
    // both increment the same global allocation counters.
    workers_finished.fetch_add(1, std::memory_order_release);
    while (!may_exit.load(std::memory_order_acquire)) {
        animus::cpu_relax();
    }
}

// ---------------------------------------------------------------------------
// Consumer: drains the ring, verifies strict sequence continuity (any gap or
// reordering is counted as a corruption event), records ingress-to-egress
// transit latency into the static histogram, and drains to true emptiness
// after producer_done is observed (see the final-drain comment below for why
// that ordering is race-free).
// ---------------------------------------------------------------------------
void consumer_thread_fn(animus::eval::SpscRingBuffer<TelemetryFrame>& ring,
                         const std::atomic<bool>& start_flag,
                         std::atomic<bool>& consumer_ready,
                         const std::atomic<bool>& producer_done,
                         std::atomic<int>& workers_finished,
                         const std::atomic<bool>& may_exit,
                         LatencyHistogram& histogram,
                         int core_id,
                         uint64_t& frames_consumed_out,
                         uint64_t& corruptions_out) {
    if (core_id >= 0) {
        animus::sys::pin_current_thread_to_core(static_cast<std::size_t>(core_id));
    }
    animus::sys::set_thread_high_priority();

    consumer_ready.store(true, std::memory_order_release);
    while (!start_flag.load(std::memory_order_acquire)) {
        animus::cpu_relax();
    }

    uint64_t expected_seq = 0;
    uint64_t consumed = 0;
    uint64_t corruptions = 0;
    TelemetryFrame frame{};

    auto process_one = [&](const TelemetryFrame& f) {
        const auto now_ns = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        const uint64_t latency_ns = now_ns - f.timestamp_ns;
        if (f.sequence_id != expected_seq) {
            ++corruptions;
            expected_seq = f.sequence_id;
        }
        expected_seq = expected_seq + 1;
        ++consumed;
        histogram.record(latency_ns);
    };

    for (;;) {
        if (ring.pop(frame)) {
            process_one(frame);
            continue;
        }
        if (producer_done.load(std::memory_order_acquire)) {
            // producer_done was published (release) only after its final
            // push()'s release-store to head_; observing it here (acquire)
            // makes that final push visible, so one more drain attempt is
            // guaranteed to see it if it landed after our prior failed pop.
            if (ring.pop(frame)) {
                process_one(frame);
                continue;
            }
            break;
        }
        animus::cpu_relax();
    }

    frames_consumed_out = consumed;
    corruptions_out = corruptions;

    // See the matching comment in producer_thread_fn: block here (rather
    // than returning) until main has taken its post-run allocation
    // snapshot, so this thread's own exit-time runtime teardown can't leak
    // into the measured hot-path window either.
    workers_finished.fetch_add(1, std::memory_order_release);
    while (!may_exit.load(std::memory_order_acquire)) {
        animus::cpu_relax();
    }
}

std::string with_commas(uint64_t value) {
    const std::string digits = std::to_string(value);
    std::string out;
    out.reserve(digits.size() + digits.size() / 3);
    int since_group = 0;
    for (auto it = digits.rbegin(); it != digits.rend(); ++it) {
        if (since_group != 0 && since_group % 3 == 0) out.push_back(',');
        out.push_back(*it);
        ++since_group;
    }
    std::reverse(out.begin(), out.end());
    return out;
}

void print_report(const BenchConfig& cfg, const RunResult& r, const LatencyHistogram& hist) {
    const double drop_rate_pct = r.frames_produced == 0
        ? 0.0
        : std::max(0.0, (static_cast<double>(r.frames_produced) - static_cast<double>(r.frames_consumed))
                             / static_cast<double>(r.frames_produced) * 100.0);
    const double sustained_rate = r.elapsed_s > 0.0 ? static_cast<double>(r.frames_consumed) / r.elapsed_s : 0.0;
    const double pct_of_target = static_cast<double>(cfg.rate) > 0.0 ? (sustained_rate / static_cast<double>(cfg.rate)) * 100.0 : 0.0;

    std::printf("================================================================================\n");
    std::printf("                    ANIMUS CORE -- INGESTION BENCHMARK HARNESS\n");
    std::printf("================================================================================\n");
    std::printf("  Configuration\n");
    std::printf("  ------------------------------------------------------------------------------\n");
    std::printf("  Mode                     : %s\n", cfg.burst ? "Burst (variable-rate)" : "Sustained (paced)");
    std::printf("  Target Rate              : %20s msgs/sec\n", with_commas(cfg.rate).c_str());
    std::printf("  Duration                 : %20.2f s\n", cfg.duration_s);
    std::printf("  Ring Buffer Capacity     : %20s frames\n", with_commas(kRingCapacity).c_str());
    std::printf("  Producer Core            : %20s\n", r.producer_core >= 0 ? std::to_string(r.producer_core).c_str() : "unpinned");
    std::printf("  Consumer Core            : %20s\n", r.consumer_core >= 0 ? std::to_string(r.consumer_core).c_str() : "unpinned");
    std::printf("\n");
    std::printf("  Throughput & Integrity\n");
    std::printf("  ------------------------------------------------------------------------------\n");
    std::printf("  Total Frames Processed   : %20s frames\n", with_commas(r.frames_consumed).c_str());
    std::printf("  Sustained Ingest Rate    : %20s ops/sec  (%.2f%% of target)\n", with_commas(static_cast<uint64_t>(sustained_rate)).c_str(), pct_of_target);
    std::printf("  Packet Drop Rate         : %24.4f %%\n", drop_rate_pct);
    std::printf("  Sequence Corruption      : %20s frames\n", with_commas(r.sequence_corruptions).c_str());
    std::printf("\n");
    std::printf("  Memory Safety\n");
    std::printf("  ------------------------------------------------------------------------------\n");
    std::printf("  Hot-Path Heap Allocations: %20s calls\n", with_commas(r.hot_path_allocs).c_str());
    std::printf("  Hot-Path Heap Frees      : %20s calls\n", with_commas(r.hot_path_deallocs).c_str());
    std::printf("\n");
    std::printf("  Latency Profile (ns) -- Ingress to Egress Transit\n");
    std::printf("  ------------------------------------------------------------------------------\n");
    std::printf("  Min                      : %20s ns\n", with_commas(hist.total_count ? hist.min_ns : 0).c_str());
    std::printf("  p50 (median)             : %20s ns\n", with_commas(hist.percentile(50.0)).c_str());
    std::printf("  p90                      : %20s ns\n", with_commas(hist.percentile(90.0)).c_str());
    std::printf("  p99                      : %20s ns\n", with_commas(hist.percentile(99.0)).c_str());
    std::printf("  p99.9                    : %20s ns\n", with_commas(hist.percentile(99.9)).c_str());
    std::printf("  Max (tail jitter)        : %20s ns\n", with_commas(hist.max_ns).c_str());
    std::printf("================================================================================\n");

    if (drop_rate_pct == 0.0 && r.sequence_corruptions == 0 && r.hot_path_allocs == 0 && r.hot_path_deallocs == 0) {
        std::printf("  VERIFIED: 0 dropped frames, 0 corrupted frames, 0 hot-path heap allocations.\n");
        std::printf("================================================================================\n");
    }
}

int run(int argc, char** argv) {
    BenchConfig cfg;
    if (!parse_args(argc, argv, cfg)) {
        print_usage(argv[0]);
        return 1;
    }

    animus::eval::SpscRingBuffer<TelemetryFrame> ring(kRingCapacity);

    const unsigned hc = std::thread::hardware_concurrency();
    int producer_core = -1;
    int consumer_core = -1;
    if (hc >= 2) {
        producer_core = static_cast<int>(hc - 2);
        consumer_core = static_cast<int>(hc - 1);
    }

    std::atomic<bool> start_flag{false};
    std::atomic<bool> producer_ready{false};
    std::atomic<bool> consumer_ready{false};
    std::atomic<bool> producer_done{false};
    std::atomic<int> workers_finished{0};
    std::atomic<bool> may_exit{false};

    uint64_t frames_produced = 0;
    uint64_t frames_consumed = 0;
    uint64_t sequence_corruptions = 0;

    std::thread producer_thread(producer_thread_fn,
        std::ref(ring), std::cref(start_flag), std::ref(producer_ready), std::ref(producer_done),
        std::ref(workers_finished), std::cref(may_exit),
        std::cref(cfg), producer_core, std::ref(frames_produced));
    std::thread consumer_thread(consumer_thread_fn,
        std::ref(ring), std::cref(start_flag), std::ref(consumer_ready), std::cref(producer_done),
        std::ref(workers_finished), std::cref(may_exit),
        std::ref(g_histogram), consumer_core, std::ref(frames_consumed), std::ref(sequence_corruptions));

    while (!producer_ready.load(std::memory_order_acquire) || !consumer_ready.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    // Baseline snapshot: everything above this line (thread launch, ring
    // buffer allocation, pinning/priority setup) is setup phase and
    // deliberately excluded from the hot-path allocation count.
    const uint64_t alloc_baseline = g_heap_alloc_ops.load(std::memory_order_relaxed);
    const uint64_t dealloc_baseline = g_heap_dealloc_ops.load(std::memory_order_relaxed);

    const auto hot_path_start = std::chrono::steady_clock::now();
    start_flag.store(true, std::memory_order_release);

    // Both threads block after finishing their real work instead of
    // returning immediately (see the barrier comment in producer_thread_fn),
    // so this snapshot is guaranteed to land after all engine-logic
    // allocation activity and before either thread's runtime-teardown
    // allocation activity -- the two cannot be race-ordered against each
    // other because neither thread has been released to exit yet.
    while (workers_finished.load(std::memory_order_acquire) < 2) {
        std::this_thread::yield();
    }
    const auto hot_path_end = std::chrono::steady_clock::now();
    const uint64_t alloc_after = g_heap_alloc_ops.load(std::memory_order_relaxed);
    const uint64_t dealloc_after = g_heap_dealloc_ops.load(std::memory_order_relaxed);
    may_exit.store(true, std::memory_order_release);

    producer_thread.join();
    consumer_thread.join();

    RunResult result;
    result.frames_produced = frames_produced;
    result.frames_consumed = frames_consumed;
    result.sequence_corruptions = sequence_corruptions;
    result.hot_path_allocs = alloc_after - alloc_baseline;
    result.hot_path_deallocs = dealloc_after - dealloc_baseline;
    result.elapsed_s = std::chrono::duration<double>(hot_path_end - hot_path_start).count();
    result.producer_core = producer_core;
    result.consumer_core = consumer_core;

    print_report(cfg, result, g_histogram);
    return 0;
}

} // namespace animus_bench

int main(int argc, char** argv) {
    return animus_bench::run(argc, argv);
}
