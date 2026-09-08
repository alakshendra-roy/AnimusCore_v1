// Animus ITCH 5.0 Adapter -- Ingestion Benchmark Harness (bench_itch_ingest)
//
// Single-producer/single-consumer proof point for zero-allocation NASDAQ
// TotalView-ITCH 5.0 ingestion into the Animus SPSC ring buffer
// (animus-eval-kit/include/spsc_ring_buffer.hpp). Generates 10,000,000
// synthetic ITCH messages spread across all eight core message types
// (adapters/itch50/include/itch50_messages.hpp), decodes each with the
// zero-allocation codec (itch50_codec.hpp) into a uniform 64-byte
// ItchFrame (itch50_ring_frame.hpp), and pushes it onto the ring -- then
// measures, per message, exactly the parse-plus-enqueue cost with a
// calibrated RDTSC clock, and separately reports sustained end-to-end
// throughput across the whole pipeline.
//
// Same three proof techniques as benchmarks/animus_harness.cpp, applied to
// this new message domain rather than duplicated by reference:
//   1. Zero-allocation proof: operator new/delete replaced with
//      instrumented malloc/free wrappers, counted globally; a baseline is
//      snapshotted right before the timed window opens (after thread
//      launch, ring allocation, and pinning/priority setup have already
//      run) so only genuine hot-path allocation activity is reported.
//   2. Calibrated RDTSC timing: a serialized (lfence-fenced) RDTSC read on
//      x86, calibrated against std::chrono::steady_clock over a dedicated
//      200ms window at startup (same methodology as
//      benchmarks/harness_benchmark.cpp), falling back to
//      clock_gettime(CLOCK_MONOTONIC_RAW) on non-x86 targets.
//   3. Static, BSS-resident latency histogram (no heap growth regardless
//      of run length) for percentile computation.
//
// What this measures is deliberately narrow: the synthetic wire-message
// ENCODE step (fabricating realistic big-endian ITCH bytes to then decode)
// happens on the stack, untimed, immediately before the timed window for
// that message opens -- a real deployment never encodes, it receives
// already-encoded bytes off the wire, so timing that step here would
// inflate the reported number with benchmark-harness-only work. Only
// decode() + ring push are inside the timed window, matching this
// harness's own name: parse + enqueue overhead, nothing else.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <string_view>
#include <thread>

#include "animus/thread_affinity.hpp"
#include "spsc_ring_buffer.hpp"

#include "itch50_bswap.hpp"
#include "itch50_codec.hpp"
#include "itch50_messages.hpp"
#include "itch50_ring_frame.hpp"

#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
    #define ANIMUS_ITCH_ARCH_X86 1
#else
    #define ANIMUS_ITCH_ARCH_X86 0
#endif

#if ANIMUS_ITCH_ARCH_X86
    #if defined(_MSC_VER)
        #include <intrin.h>
    #else
        #include <x86intrin.h>
    #endif
#endif

#if !defined(_WIN32)
    #include <time.h>
#endif

// ---------------------------------------------------------------------------
// Zero-allocation guard -- identical technique to benchmarks/animus_harness.cpp;
// kept as its own file-local copy (not shared via a header) because the
// standard replaceable operator new/delete overloads are process-global by
// nature and each benchmark binary defines its own, exactly once.
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
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* ptr) noexcept {
    g_heap_dealloc_ops.fetch_add(1, std::memory_order_relaxed);
    std::free(ptr);
}
void operator delete(void* ptr, std::size_t) noexcept { ::operator delete(ptr); }
void operator delete[](void* ptr) noexcept { ::operator delete(ptr); }
void operator delete[](void* ptr, std::size_t size) noexcept { ::operator delete(ptr, size); }

namespace bench_itch_ingest {

using adapters::itch50::ItchFrame;
using adapters::itch50::MessageType;

// ---------------------------------------------------------------------------
// Calibrated hardware clock -- same methodology as
// benchmarks/harness_benchmark.cpp's sample_clock()/calibrate_units_per_ns().
// ---------------------------------------------------------------------------
#if ANIMUS_ITCH_ARCH_X86
inline uint64_t sample_clock() noexcept {
    _mm_lfence();
    const uint64_t t = __rdtsc();
    _mm_lfence();
    return t;
}
double calibrate_units_per_ns() noexcept {
    constexpr auto kCalibrationWindow = std::chrono::milliseconds(200);
    const auto wall_start = std::chrono::steady_clock::now();
    const uint64_t tsc_start = sample_clock();
    while (std::chrono::steady_clock::now() - wall_start < kCalibrationWindow) {
        // Busy-wait deliberately: a sleep here risks a core migration
        // corrupting the calibration delta (see harness_benchmark.cpp).
    }
    const uint64_t tsc_end = sample_clock();
    const auto wall_end = std::chrono::steady_clock::now();
    const double elapsed_ns = std::chrono::duration<double, std::nano>(wall_end - wall_start).count();
    return static_cast<double>(tsc_end - tsc_start) / elapsed_ns;
}
const char* clock_source_name() noexcept { return "RDTSC (lfence-serialized, calibrated)"; }
#else
inline uint64_t sample_clock() noexcept {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ull + static_cast<uint64_t>(ts.tv_nsec);
}
double calibrate_units_per_ns() noexcept { return 1.0; }
const char* clock_source_name() noexcept { return "clock_gettime(CLOCK_MONOTONIC_RAW)"; }
#endif

// ---------------------------------------------------------------------------
// Fixed-size nanosecond latency histogram -- BSS-resident, no heap growth.
// Same bucket scheme as benchmarks/animus_harness.cpp's LatencyHistogram.
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
        return max_ns;
    }
};

LatencyHistogram g_histogram; // static storage: ~1 MiB, never heap-allocated

struct BenchConfig {
    uint64_t message_count = 10'000'000ULL;
    size_t ring_capacity = 1u << 20; // 1,048,576 frames
};

struct MessageTypeCounts {
    uint64_t system_event = 0, add_order = 0, add_order_mpid = 0, order_executed = 0,
             order_executed_with_price = 0, order_cancel = 0, order_delete = 0, order_replace = 0;
    uint64_t total() const noexcept {
        return system_event + add_order + add_order_mpid + order_executed +
               order_executed_with_price + order_cancel + order_delete + order_replace;
    }
};

struct RunResult {
    uint64_t messages_produced = 0;
    uint64_t messages_consumed = 0;
    uint64_t sequence_corruptions = 0;
    uint64_t decode_failures = 0;
    uint64_t hot_path_allocs = 0;
    uint64_t hot_path_deallocs = 0;
    double elapsed_s = 0.0;
    int producer_core = -1;
    int consumer_core = -1;
    MessageTypeCounts counts;
};

void print_usage(const char* argv0) {
    std::fprintf(stderr,
        "Usage: %s [--messages <count>] [--capacity <slots>]\n"
        "  --messages <count>   Total synthetic ITCH messages to generate (default: 10000000)\n"
        "  --capacity <slots>   SPSC ring capacity in frames, rounded up to a power of two (default: 1048576)\n",
        argv0);
}

bool parse_args(int argc, char** argv, BenchConfig& cfg) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--messages") {
            if (i + 1 >= argc) { std::fprintf(stderr, "error: --messages requires a value\n"); return false; }
            cfg.message_count = std::strtoull(argv[++i], nullptr, 10);
        } else if (arg == "--capacity") {
            if (i + 1 >= argc) { std::fprintf(stderr, "error: --capacity requires a value\n"); return false; }
            cfg.ring_capacity = static_cast<size_t>(std::strtoull(argv[++i], nullptr, 10));
        } else if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            std::exit(0);
        } else {
            std::fprintf(stderr, "error: unrecognized argument '%s'\n", argv[i]);
            return false;
        }
    }
    if (cfg.message_count == 0) {
        std::fprintf(stderr, "error: --messages must be > 0\n");
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Synthetic ITCH wire-message generator. Produces realistic (but not
// referentially-consistent -- see itch50_ring_frame.hpp's own scope note)
// big-endian ITCH bytes for one of the eight core message types, chosen by
// a fixed distribution loosely modeled on a liquid single-name symbol's
// real-world message mix (heavy on Add Order / Order Delete / Order
// Executed, light on System Event / Order Executed With Price). Writes
// into a caller-owned stack buffer of at least itch50::kMaxWireMessageSize
// bytes -- no heap allocation, consistent with every other step on this
// benchmark's hot path.
// ---------------------------------------------------------------------------
struct Generator {
    uint64_t rng_state = 0x9E3779B97F4A7C15ULL;
    uint64_t next_order_ref = 1;
    uint64_t next_match_number = 1;
    uint64_t itch_clock_ns = 34'200'000'000'000ULL; // 09:30:00.000 in ns-since-midnight, a plausible market-open instant

    static constexpr const char* kSymbols[] = {"AAPL    ", "MSFT    ", "GOOG    ", "AMZN    ",
                                                "TSLA    ", "NVDA    ", "META    ", "BRKB    "};
    static constexpr int kNumSymbols = 8;

    uint64_t next_rand() noexcept {
        rng_state ^= rng_state << 13;
        rng_state ^= rng_state >> 7;
        rng_state ^= rng_state << 17;
        return rng_state;
    }

    // Returns the number of bytes written into `buf` (>= kMaxWireMessageSize
    // capacity required) and the message type chosen.
    std::size_t generate(uint8_t* buf, MessageType& type_out) noexcept {
        using namespace adapters::itch50;

        const uint32_t roll = static_cast<uint32_t>(next_rand() % 100);
        MessageType type;
        if (roll < 30)      type = MessageType::AddOrderNoMPID;          // 30%
        else if (roll < 35) type = MessageType::AddOrderWithMPID;        // 5%
        else if (roll < 55) type = MessageType::OrderExecuted;           // 20%
        else if (roll < 58) type = MessageType::OrderExecutedWithPrice;  // 3%
        else if (roll < 70) type = MessageType::OrderCancel;             // 12%
        else if (roll < 95) type = MessageType::OrderDelete;             // 25%
        else if (roll < 99) type = MessageType::OrderReplace;            // 4%
        else                type = MessageType::SystemEvent;             // 1%
        type_out = type;

        itch_clock_ns += 100 + (next_rand() % 5000); // monotonically increasing, sub-microsecond jitter
        const uint16_t stock_locate = static_cast<uint16_t>(1 + (next_rand() % 8000));
        const uint16_t tracking_number = static_cast<uint16_t>(next_rand() % 65536);
        const uint32_t shares = static_cast<uint32_t>(1 + (next_rand() % 1000));
        const uint32_t price = static_cast<uint32_t>(500000 + (next_rand() % 2000000)); // $50.0000-$250.0000, 4 implied decimals
        const char* symbol = kSymbols[next_rand() % kNumSymbols];

        std::size_t len = 0;
        buf[0] = static_cast<uint8_t>(type);
        put_be16(buf + 1, stock_locate);
        put_be16(buf + 3, tracking_number);
        put_be48_ns(buf + 5, itch_clock_ns);

        switch (type) {
        case MessageType::SystemEvent: {
            static constexpr char kEventCodes[] = {'O', 'S', 'Q', 'M', 'E', 'C', 'A', 'R'};
            buf[11] = static_cast<uint8_t>(kEventCodes[next_rand() % 8]);
            len = sizeof(SystemEventMsg);
            break;
        }
        case MessageType::AddOrderNoMPID: {
            const uint64_t order_ref = next_order_ref++;
            put_be64(buf + 11, order_ref);
            buf[19] = (next_rand() % 2) ? 'B' : 'S';
            put_be32(buf + 20, shares);
            std::memcpy(buf + 24, symbol, 8);
            put_be32(buf + 32, price);
            len = sizeof(AddOrderMsg);
            break;
        }
        case MessageType::AddOrderWithMPID: {
            const uint64_t order_ref = next_order_ref++;
            put_be64(buf + 11, order_ref);
            buf[19] = (next_rand() % 2) ? 'B' : 'S';
            put_be32(buf + 20, shares);
            std::memcpy(buf + 24, symbol, 8);
            put_be32(buf + 32, price);
            std::memcpy(buf + 36, "ANIM", 4);
            len = sizeof(AddOrderMPIDMsg);
            break;
        }
        case MessageType::OrderExecuted: {
            put_be64(buf + 11, next_order_ref == 1 ? 1 : (next_rand() % next_order_ref) + 1);
            put_be32(buf + 19, shares);
            put_be64(buf + 23, next_match_number++);
            len = sizeof(OrderExecutedMsg);
            break;
        }
        case MessageType::OrderExecutedWithPrice: {
            put_be64(buf + 11, next_order_ref == 1 ? 1 : (next_rand() % next_order_ref) + 1);
            put_be32(buf + 19, shares);
            put_be64(buf + 23, next_match_number++);
            buf[31] = (next_rand() % 10 == 0) ? 'N' : 'Y';
            put_be32(buf + 32, price);
            len = sizeof(OrderExecutedWithPriceMsg);
            break;
        }
        case MessageType::OrderCancel: {
            put_be64(buf + 11, next_order_ref == 1 ? 1 : (next_rand() % next_order_ref) + 1);
            put_be32(buf + 19, shares);
            len = sizeof(OrderCancelMsg);
            break;
        }
        case MessageType::OrderDelete: {
            put_be64(buf + 11, next_order_ref == 1 ? 1 : (next_rand() % next_order_ref) + 1);
            len = sizeof(OrderDeleteMsg);
            break;
        }
        case MessageType::OrderReplace: {
            const uint64_t original_ref = next_order_ref == 1 ? 1 : (next_rand() % next_order_ref) + 1;
            const uint64_t new_ref = next_order_ref++;
            put_be64(buf + 11, original_ref);
            put_be64(buf + 19, new_ref);
            put_be32(buf + 27, shares);
            put_be32(buf + 31, price);
            len = sizeof(OrderReplaceMsg);
            break;
        }
        }
        return len;
    }
};

// ---------------------------------------------------------------------------
// Producer: generates (untimed) + decodes + pushes (timed) each synthetic
// ITCH message. Retries push on a full ring rather than dropping, so
// backpressure shows up as latency, never as a lost message.
// ---------------------------------------------------------------------------
void producer_thread_fn(animus::eval::SpscRingBuffer<ItchFrame>& ring,
                         const std::atomic<bool>& start_flag,
                         std::atomic<bool>& producer_ready,
                         std::atomic<bool>& producer_done,
                         std::atomic<int>& workers_finished,
                         const std::atomic<bool>& may_exit,
                         const BenchConfig& cfg,
                         double units_per_ns,
                         int core_id,
                         RunResult& result_out) {
    if (core_id >= 0) {
        animus::sys::pin_current_thread_to_core(static_cast<std::size_t>(core_id));
    }
    animus::sys::set_thread_high_priority();

    Generator gen;
    MessageTypeCounts counts;
    uint64_t decode_failures = 0;

    producer_ready.store(true, std::memory_order_release);
    while (!start_flag.load(std::memory_order_acquire)) {
        animus::cpu_relax();
    }

    uint8_t wire_buf[adapters::itch50::kMaxWireMessageSize];
    uint64_t seq = 0;
    for (uint64_t i = 0; i < cfg.message_count; ++i) {
        MessageType type;
        const std::size_t wire_len = gen.generate(wire_buf, type); // untimed -- see file header comment

        ItchFrame frame{};
        const uint64_t t0 = sample_clock();
        const bool ok = adapters::itch50::decode(wire_buf, wire_len, frame);
        if (ok) {
            frame.sequence_id = seq;
            frame.recv_timestamp_ns = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
            while (!ring.push(frame)) {
                animus::cpu_relax();
            }
        }
        const uint64_t t1 = sample_clock();

        if (ok) {
            const double latency_ns = static_cast<double>(t1 - t0) / units_per_ns;
            g_histogram.record(static_cast<uint64_t>(latency_ns));
            ++seq;
            switch (type) {
            case MessageType::SystemEvent: ++counts.system_event; break;
            case MessageType::AddOrderNoMPID: ++counts.add_order; break;
            case MessageType::AddOrderWithMPID: ++counts.add_order_mpid; break;
            case MessageType::OrderExecuted: ++counts.order_executed; break;
            case MessageType::OrderExecutedWithPrice: ++counts.order_executed_with_price; break;
            case MessageType::OrderCancel: ++counts.order_cancel; break;
            case MessageType::OrderDelete: ++counts.order_delete; break;
            case MessageType::OrderReplace: ++counts.order_replace; break;
            }
        } else {
            ++decode_failures;
        }
    }

    result_out.messages_produced = seq;
    result_out.decode_failures = decode_failures;
    result_out.counts = counts;
    producer_done.store(true, std::memory_order_release);

    // Same exit barrier as benchmarks/animus_harness.cpp: block until main
    // has captured the post-run allocation snapshot, so this thread's own
    // exit-time runtime teardown can't leak into the measured window.
    workers_finished.fetch_add(1, std::memory_order_release);
    while (!may_exit.load(std::memory_order_acquire)) {
        animus::cpu_relax();
    }
}

// ---------------------------------------------------------------------------
// Consumer: drains the ring, verifies strict sequence continuity (any gap
// or reorder is a corruption event) -- proving ring ordering integrity
// independent of the ITCH-level order_ref_number fields, which the
// synthetic generator does not guarantee are referentially consistent
// (see itch50_ring_frame.hpp's scope note).
// ---------------------------------------------------------------------------
void consumer_thread_fn(animus::eval::SpscRingBuffer<ItchFrame>& ring,
                         const std::atomic<bool>& start_flag,
                         std::atomic<bool>& consumer_ready,
                         const std::atomic<bool>& producer_done,
                         std::atomic<int>& workers_finished,
                         const std::atomic<bool>& may_exit,
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
    ItchFrame frame{};

    auto process_one = [&](const ItchFrame& f) {
        if (f.sequence_id != expected_seq) {
            ++corruptions;
            expected_seq = f.sequence_id;
        }
        expected_seq = expected_seq + 1;
        ++consumed;
    };

    for (;;) {
        if (ring.pop(frame)) {
            process_one(frame);
            continue;
        }
        if (producer_done.load(std::memory_order_acquire)) {
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
    const double sustained_rate = r.elapsed_s > 0.0 ? static_cast<double>(r.messages_consumed) / r.elapsed_s : 0.0;

    std::printf("================================================================================\n");
    std::printf("           ANIMUS ITCH 5.0 ADAPTER -- INGESTION BENCHMARK HARNESS\n");
    std::printf("================================================================================\n");
    std::printf("  Configuration\n");
    std::printf("  ------------------------------------------------------------------------------\n");
    std::printf("  Protocol                 : %20s\n", "NASDAQ TotalView-ITCH 5.0");
    std::printf("  Clock Source             : %20s\n", clock_source_name());
    std::printf("  Messages Requested       : %20s\n", with_commas(cfg.message_count).c_str());
    std::printf("  Ring Buffer Capacity     : %20s frames\n", with_commas(cfg.ring_capacity).c_str());
    std::printf("  Producer Core            : %20s\n", r.producer_core >= 0 ? std::to_string(r.producer_core).c_str() : "unpinned");
    std::printf("  Consumer Core            : %20s\n", r.consumer_core >= 0 ? std::to_string(r.consumer_core).c_str() : "unpinned");
    std::printf("\n");
    std::printf("  Message Type Distribution (generated)\n");
    std::printf("  ------------------------------------------------------------------------------\n");
    std::printf("  'S' System Event         : %20s\n", with_commas(r.counts.system_event).c_str());
    std::printf("  'A' Add Order (no MPID)  : %20s\n", with_commas(r.counts.add_order).c_str());
    std::printf("  'F' Add Order (MPID)     : %20s\n", with_commas(r.counts.add_order_mpid).c_str());
    std::printf("  'E' Order Executed       : %20s\n", with_commas(r.counts.order_executed).c_str());
    std::printf("  'C' Order Executed+Price : %20s\n", with_commas(r.counts.order_executed_with_price).c_str());
    std::printf("  'X' Order Cancel         : %20s\n", with_commas(r.counts.order_cancel).c_str());
    std::printf("  'D' Order Delete         : %20s\n", with_commas(r.counts.order_delete).c_str());
    std::printf("  'U' Order Replace        : %20s\n", with_commas(r.counts.order_replace).c_str());
    std::printf("\n");
    std::printf("  Throughput & Integrity\n");
    std::printf("  ------------------------------------------------------------------------------\n");
    std::printf("  Total Messages Processed : %20s\n", with_commas(r.messages_consumed).c_str());
    std::printf("  Sustained Ingest Rate    : %17.3f M msgs/sec\n", sustained_rate / 1e6);
    std::printf("  Decode Failures          : %20s\n", with_commas(r.decode_failures).c_str());
    std::printf("  Sequence Corruption      : %20s frames\n", with_commas(r.sequence_corruptions).c_str());
    std::printf("\n");
    std::printf("  Memory Safety\n");
    std::printf("  ------------------------------------------------------------------------------\n");
    std::printf("  Hot-Path Heap Allocations: %20s calls\n", with_commas(r.hot_path_allocs).c_str());
    std::printf("  Hot-Path Heap Frees      : %20s calls\n", with_commas(r.hot_path_deallocs).c_str());
    std::printf("\n");
    std::printf("  Latency Profile (ns) -- Parse + Enqueue Overhead\n");
    std::printf("  ------------------------------------------------------------------------------\n");
    std::printf("  Min                      : %20s ns\n", with_commas(hist.total_count ? hist.min_ns : 0).c_str());
    std::printf("  p50 (median)             : %20s ns\n", with_commas(hist.percentile(50.0)).c_str());
    std::printf("  p90                      : %20s ns\n", with_commas(hist.percentile(90.0)).c_str());
    std::printf("  p99                      : %20s ns\n", with_commas(hist.percentile(99.0)).c_str());
    std::printf("  p99.9                    : %20s ns\n", with_commas(hist.percentile(99.9)).c_str());
    std::printf("  Max (tail jitter)        : %20s ns\n", with_commas(hist.max_ns).c_str());
    std::printf("================================================================================\n");

    if (r.decode_failures == 0 && r.sequence_corruptions == 0 && r.hot_path_allocs == 0 && r.hot_path_deallocs == 0) {
        std::printf("  VERIFIED: 0 decode failures, 0 corrupted frames, 0 hot-path heap allocations.\n");
        std::printf("================================================================================\n");
    }
}

int run(int argc, char** argv) {
    BenchConfig cfg;
    if (!parse_args(argc, argv, cfg)) {
        print_usage(argv[0]);
        return 1;
    }

    const double units_per_ns = calibrate_units_per_ns();
    animus::eval::SpscRingBuffer<ItchFrame> ring(cfg.ring_capacity);

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

    RunResult result;
    uint64_t frames_consumed = 0;
    uint64_t sequence_corruptions = 0;

    std::thread producer_thread(producer_thread_fn,
        std::ref(ring), std::cref(start_flag), std::ref(producer_ready), std::ref(producer_done),
        std::ref(workers_finished), std::cref(may_exit),
        std::cref(cfg), units_per_ns, producer_core, std::ref(result));
    std::thread consumer_thread(consumer_thread_fn,
        std::ref(ring), std::cref(start_flag), std::ref(consumer_ready), std::cref(producer_done),
        std::ref(workers_finished), std::cref(may_exit),
        consumer_core, std::ref(frames_consumed), std::ref(sequence_corruptions));

    while (!producer_ready.load(std::memory_order_acquire) || !consumer_ready.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    // Baseline snapshot: thread launch, ring allocation, and pinning/priority
    // setup above this line are excluded from the hot-path allocation count.
    const uint64_t alloc_baseline = g_heap_alloc_ops.load(std::memory_order_relaxed);
    const uint64_t dealloc_baseline = g_heap_dealloc_ops.load(std::memory_order_relaxed);

    const auto hot_path_start = std::chrono::steady_clock::now();
    start_flag.store(true, std::memory_order_release);

    while (workers_finished.load(std::memory_order_acquire) < 2) {
        std::this_thread::yield();
    }
    const auto hot_path_end = std::chrono::steady_clock::now();
    const uint64_t alloc_after = g_heap_alloc_ops.load(std::memory_order_relaxed);
    const uint64_t dealloc_after = g_heap_dealloc_ops.load(std::memory_order_relaxed);
    may_exit.store(true, std::memory_order_release);

    producer_thread.join();
    consumer_thread.join();

    result.messages_consumed = frames_consumed;
    result.sequence_corruptions = sequence_corruptions;
    result.hot_path_allocs = alloc_after - alloc_baseline;
    result.hot_path_deallocs = dealloc_after - dealloc_baseline;
    result.elapsed_s = std::chrono::duration<double>(hot_path_end - hot_path_start).count();
    result.producer_core = producer_core;
    result.consumer_core = consumer_core;

    print_report(cfg, result, g_histogram);
    return 0;
}

} // namespace bench_itch_ingest

int main(int argc, char** argv) {
    return bench_itch_ingest::run(argc, argv);
}
