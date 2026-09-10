// Animus Evaluation Kit -- Standalone Replay Benchmark
//
// Independent evaluation harness for libanimus: this file includes only
// include/animus/engine.hpp (AnimusEngine's public surface) -- no internal
// Animus source, no proprietary wire codec, nothing beyond what any client
// compiling against the shipped header and library would have.
//
// Methodology
// -----------
// 1,000,000 synthetic WireTick packets (plus a smaller warm-up burst) are
// generated into memory before the timed run starts, so neither malloc nor
// disk I/O appears anywhere in the measured loop. The producer thread
// (pinned to one isolated core) stamps each packet's send_timestamp_ns
// immediately before calling ingest_packet(), and AnimusEngine stamps
// recv_timestamp_ns the instant the packet is pushed onto the ring (see
// src/engine.cpp) -- the difference between those two timestamps is the
// tick-to-ring latency this harness reports as a p50/p90/p99/p99.9
// histogram. The consumer thread (pinned to a second isolated core) drains
// the ring concurrently with the producer's send loop via poll(), so the
// measurement reflects genuine cross-core cache-coherency handoff cost
// under concurrent load, not a serialized push-then-pop benchmark.
//
// clock_gettime(CLOCK_MONOTONIC_RAW) is used directly (Linux) rather than a
// calibrated cycle counter: it already returns nanoseconds, is immune to
// NTP slewing, and needs no cycles-per-ns calibration step -- one fewer
// potential source of measurement error for a kit meant to be run
// independently and audited by a client's own engineers.

#include "animus/engine.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#if defined(__linux__)
    #include <time.h>
#endif

namespace {

using animus::eval::AnimusEngine;
using animus::eval::MarketDataEntry;
using animus::eval::WireTick;

constexpr uint32_t kDefaultProducerCore = 2;
constexpr uint32_t kDefaultConsumerCore = 3;
constexpr uint64_t kWarmupPackets = 50'000;
constexpr uint64_t kMeasuredPackets = 1'000'000;
constexpr uint64_t kTotalPackets = kWarmupPackets + kMeasuredPackets;
constexpr size_t kRingCapacity = 1u << 20; // 1,048,576 slots -- large relative to kTotalPackets so a healthy consumer sees zero drops

// Matches src/engine.cpp's own now_ns(): CLOCK_MONOTONIC_RAW on Linux
// (unaffected by NTP slewing, which matters for a value feeding a latency
// measurement), std::chrono::steady_clock elsewhere for portability on a
// non-Linux development machine. Both sides of every latency sample this
// harness reports come from the same clock domain within the same process,
// so this local copy staying in sync with engine.cpp's is what makes the
// subtraction meaningful.
inline uint64_t now_ns() noexcept {
#if defined(__linux__)
    struct timespec ts;
    ::clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ull + static_cast<uint64_t>(ts.tv_nsec);
#else
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
#endif
}

// Deterministic synthetic tick generator -- no RNG on the construction
// path, but fields still vary packet-to-packet so the payload is
// representative of a real, non-repeating tick stream. send_timestamp_ns
// is left at zero here; the producer loop overwrites it immediately before
// each ingest_packet() call so it reflects the actual send instant, not
// pre-generation time.
std::vector<WireTick> generate_synthetic_packets(uint64_t count) {
    std::vector<WireTick> packets(count);
    for (uint64_t i = 0; i < count; ++i) {
        packets[i] = WireTick{
            0, // send_timestamp_ns, stamped at send time
            i, // sequence
            static_cast<int64_t>(100'000'000'000ll + static_cast<int64_t>((i % 4096) * 10'000)), // price_fixed
            static_cast<uint32_t>(i % 512),          // symbol_id
            static_cast<uint32_t>(100 + (i % 900)),  // quantity
            static_cast<uint8_t>(i & 1),             // side
            static_cast<uint8_t>(0),                 // msg_type: new
            {0, 0},                                  // reserved
        };
    }
    return packets;
}

struct PercentileReport {
    uint64_t min = 0, p50 = 0, p90 = 0, p99 = 0, p999 = 0, max = 0;
};

PercentileReport summarize(std::vector<uint64_t>& samples_ns) {
    std::sort(samples_ns.begin(), samples_ns.end());
    const size_t n = samples_ns.size();
    auto at = [&](double p) -> uint64_t {
        size_t idx = static_cast<size_t>(p * static_cast<double>(n - 1));
        if (idx >= n) idx = n - 1;
        return samples_ns[idx];
    };
    PercentileReport r;
    r.min = samples_ns.front();
    r.p50 = at(0.50);
    r.p90 = at(0.90);
    r.p99 = at(0.99);
    r.p999 = at(0.999);
    r.max = samples_ns.back();
    return r;
}

// Consumer-callback context. Only the consumer thread ever touches
// `latencies`/`recorded`, so no atomics are needed for either -- the
// vector is pre-reserved to kMeasuredPackets before the timed run starts,
// so push_back() here never reallocates or otherwise allocates.
struct ConsumerContext {
    std::vector<uint64_t> latencies_ns;
    uint64_t recorded = 0;
    uint64_t consumed_total = 0;
};

void on_entry(const MarketDataEntry& entry, void* user_data) {
    auto* ctx = static_cast<ConsumerContext*>(user_data);
    ++ctx->consumed_total;
    if (entry.sequence >= kWarmupPackets) {
        ctx->latencies_ns.push_back(entry.recv_timestamp_ns - entry.send_timestamp_ns);
        ++ctx->recorded;
    }
}

} // namespace

int main(int argc, char** argv) {
    uint32_t producer_core = kDefaultProducerCore;
    uint32_t consumer_core = kDefaultConsumerCore;
    if (argc >= 3) {
        producer_core = static_cast<uint32_t>(std::atoi(argv[1]));
        consumer_core = static_cast<uint32_t>(std::atoi(argv[2]));
    }

    std::printf("Animus Evaluation Kit -- Replay Benchmark\n");
    std::printf("==========================================\n");
    std::printf("Warm-up packets:        %llu\n", static_cast<unsigned long long>(kWarmupPackets));
    std::printf("Measured packets:       %llu\n", static_cast<unsigned long long>(kMeasuredPackets));
    std::printf("Ring capacity:          %zu slots\n", kRingCapacity);
    std::printf("Requested affinity:     producer=core %u, consumer=core %u\n\n", producer_core, consumer_core);

    std::printf("Pre-generating %llu synthetic packets in memory...\n", static_cast<unsigned long long>(kTotalPackets));
    std::vector<WireTick> packets = generate_synthetic_packets(kTotalPackets);
    std::printf("Done. Starting timed run (no allocation, no disk I/O from here).\n\n");

    AnimusEngine engine;
    ConsumerContext ctx;
    ctx.latencies_ns.reserve(kMeasuredPackets);

    std::atomic<bool> consumer_ready{false};
    std::atomic<bool> stop{false};

    std::thread consumer([&]() {
        const bool pinned = animus::eval::affinity::pin_current_thread_to_core(consumer_core);
        if (!pinned) {
            std::fprintf(stderr, "[warn] consumer: failed to pin to core %u\n", consumer_core);
        }
        consumer_ready.store(true, std::memory_order_release);
        while (!stop.load(std::memory_order_relaxed)) {
            engine.poll(on_entry, &ctx, kTotalPackets);
        }
        engine.poll(on_entry, &ctx, kTotalPackets); // final drain after stop is observed
    });

    // initialize() pins the *calling* thread -- this is the producer, per
    // AnimusEngine's documented contract (see engine.hpp).
    if (!engine.initialize(producer_core, kRingCapacity)) {
        std::fprintf(stderr, "error: AnimusEngine::initialize failed (producer core=%u, capacity=%zu)\n",
            producer_core, kRingCapacity);
        stop.store(true, std::memory_order_relaxed);
        consumer.join();
        return 1;
    }
    while (!consumer_ready.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    const auto run_start = std::chrono::steady_clock::now();
    for (uint64_t i = 0; i < kTotalPackets; ++i) {
        packets[i].send_timestamp_ns = now_ns();
        engine.ingest_packet(reinterpret_cast<const uint8_t*>(&packets[i]), sizeof(WireTick));
    }
    const auto send_done = std::chrono::steady_clock::now();

    // Wait for the consumer to drain everything the producer published.
    while (engine.get_telemetry_metrics().ring_occupancy_approx > 0) {
        std::this_thread::yield();
    }
    stop.store(true, std::memory_order_relaxed);
    consumer.join();
    const auto run_end = std::chrono::steady_clock::now();

    const auto metrics = engine.get_telemetry_metrics();
    const double send_elapsed_s = std::chrono::duration<double>(send_done - run_start).count();
    const double drain_elapsed_s = std::chrono::duration<double>(run_end - run_start).count();
    const double throughput_msgs_per_sec = static_cast<double>(metrics.packets_ingested) / send_elapsed_s;

    if (ctx.latencies_ns.empty()) {
        std::fprintf(stderr, "error: no latency samples captured -- consumer never observed a measured-phase packet\n");
        return 1;
    }
    const PercentileReport report = summarize(ctx.latencies_ns);

    std::printf("Tick-to-ring latency (send_timestamp -> ring push), %llu samples:\n",
        static_cast<unsigned long long>(ctx.latencies_ns.size()));
    std::printf("  -------------------------------------------------\n");
    std::printf("  min      %12.1f ns\n", static_cast<double>(report.min));
    std::printf("  p50      %12.1f ns\n", static_cast<double>(report.p50));
    std::printf("  p90      %12.1f ns\n", static_cast<double>(report.p90));
    std::printf("  p99      %12.1f ns\n", static_cast<double>(report.p99));
    std::printf("  p99.9    %12.1f ns\n", static_cast<double>(report.p999));
    std::printf("  max      %12.1f ns\n", static_cast<double>(report.max));
    std::printf("  -------------------------------------------------\n\n");

    std::printf("Throughput and loss (%.3fs send window, %.3fs to full drain):\n", send_elapsed_s, drain_elapsed_s);
    std::printf("  -------------------------------------------------\n");
    std::printf("  packets ingested:     %llu\n", static_cast<unsigned long long>(metrics.packets_ingested));
    std::printf("  packets dropped:      %llu\n", static_cast<unsigned long long>(metrics.packets_dropped));
    std::printf("  packets malformed:    %llu\n", static_cast<unsigned long long>(metrics.packets_malformed));
    std::printf("  packets consumed:     %llu\n", static_cast<unsigned long long>(ctx.consumed_total));
    std::printf("  sustained throughput: %.3f M msgs/sec\n", throughput_msgs_per_sec / 1'000'000.0);
    const bool zero_loss = (metrics.packets_dropped == 0) && (metrics.packets_malformed == 0) &&
        (ctx.consumed_total == metrics.packets_ingested);
    std::printf("  zero-loss verification: %s\n", zero_loss ? "PASS" : "FAIL");
    std::printf("  -------------------------------------------------\n");

    return zero_loss ? 0 : 1;
}
