// Institutional-grade benchmark suite for animus.hpp's low-latency
// primitives: header-only, zero DLL, zero Python, zero ctypes -- same
// "#include animus.hpp is the entire dependency footprint" approach as
// execution_interop_demo.cpp. Deliberately native C++, not Python: two of
// the three measurements below (real 8-thread ring contention, and
// sub-microsecond tick-to-trade latency) cannot be honestly measured from
// Python. The GIL serializes Python "threads" onto one core, so an 8-
// Python-thread test would never exercise real cross-core cache
// contention on the ring -- it would silently measure something else and
// call it "8-thread concurrency." And the ctypes call-marshalling tax
// documented elsewhere in this repo (~1.3us/call, Phases 11/13/16) is
// already wider than the sub-microsecond budget this benchmark is asked
// to report on. Both measurements need real OS threads and in-process
// calls with no FFI boundary, which is exactly what driving animus::
// directly gets for free.
//
// benchmarks/generate_benchmark_report.py compiles and runs this binary,
// parses the BENCHMARK_RESULT_* lines below, and renders the Markdown
// report -- this file owns the measurement, that script owns formatting
// and orchestration, same division of labor as animus_engine.cpp (C-ABI
// shim) vs. animus.hpp (implementation).
//
// Build and run standalone (matches execution_interop_demo.cpp's own
// instructions):
//   g++ -std=c++17 -O2 -pthread animus_benchmark_suite.cpp -o animus_benchmark_suite.exe
//   cl /EHsc /std:c++17 /O2 animus_benchmark_suite.cpp
#include "animus.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <numeric>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

// Linear-interpolation percentile, matching benchmarks/fintech_tail_latency.py's
// percentile() exactly (same method as numpy.percentile's default 'linear'
// mode) so numbers from the Python and C++ sides of this repo are directly
// comparable, not silently using two different definitions of "p99".
double percentile(const std::vector<double>& sorted_data, double pct) {
    if (sorted_data.empty()) return 0.0;
    if (sorted_data.size() == 1) return sorted_data[0];
    double k = (pct / 100.0) * static_cast<double>(sorted_data.size() - 1);
    size_t f = static_cast<size_t>(std::floor(k));
    size_t c = static_cast<size_t>(std::ceil(k));
    if (f == c) return sorted_data[static_cast<size_t>(k)];
    double d0 = sorted_data[f] * (static_cast<double>(c) - k);
    double d1 = sorted_data[c] * (k - static_cast<double>(f));
    return d0 + d1;
}

// -----------------------------------------------------------------------
// 1. Tick-to-trade end-to-end latency: MarketDataFeed ingestion through
//    ExecutionClient submission, timed one tick at a time.
// -----------------------------------------------------------------------
// Deliberately single-threaded, sequential push-then-drain-then-submit,
// not a decoupled producer thread racing a consumer thread -- matching
// execution_interop_demo.cpp's own latency-measurement methodology (that
// demo times submit() in a plain sequential loop, not from a separate
// producer thread either). A two-thread version was tried first and
// measured mean/p50 latency in the *milliseconds*, not nanoseconds: an
// unpaced producer thread pushes 500,000 ticks in a tight loop far faster
// than the consumer thread can drain and process them, so most ticks sit
// queued for a growing backlog before ever being touched -- exactly the
// "producer-faster-than-consumer backlog" pitfall this repo already hit
// and documented once before, in the shared-memory IPC benchmark
// (BENCHMARKS.md's Phase 16 section: a burst without pacing looked like
// catastrophic latency until it was traced to a throughput mismatch, not
// the transport). That was a real methodology bug, caught by actually
// running this benchmark before reporting any number from it -- not a
// property of MarketDataFeed itself (see the separate ring-throughput
// benchmark below for its real, honest concurrent-producer numbers).
// Tick-to-trade latency, correctly measured, is a decision-loop metric:
// how long from "a tick is available" to "an order for it is submitted,"
// with nothing else queued ahead of it -- exactly what this sequential
// loop measures, and exactly how a real single-threaded low-latency
// trading loop (read tick, decide, send order) actually operates.
//
// Each iteration: stamp a wall-clock timestamp (steady_clock, not the
// TSC-based read_cycle_counter() the field would otherwise carry --
// converting raw TSC cycles to elapsed time needs a calibrated TSC
// frequency this benchmark doesn't establish, so steady_clock keeps the
// latency math exact with no calibration step), push it as one trade
// tick, immediately poll it back out (single-threaded lockstep: exactly
// one tick is ever pending, so poll_trades must return exactly one),
// route it through ExecutionClient (LoopbackBrokerGateway: an instant,
// deterministic in-process fill, so nothing here measures a real
// broker's latency, only this pipeline's own overhead), then stamp
// another timestamp. Latency = the gap between the two: genuinely
// end-to-end, ingestion through execution.
struct TickToTradeResult {
    size_t num_ticks;
    double mean_ns, min_ns, max_ns, p50_ns, p99_ns, p99_9_ns;
    double throughput_ticks_per_sec;
};

TickToTradeResult run_tick_to_trade_benchmark(size_t num_ticks) {
    auto engine = animus::Engine::Create(1 << 16);
    animus::LoopbackBrokerGateway gateway;
    animus::ExecutionClient client(*engine, gateway);
    auto feed = animus::MarketDataFeed::create(/*l2_capacity=*/1024, /*trade_capacity=*/1024);

    std::vector<double> latencies_ns;
    latencies_ns.reserve(num_ticks);

    animus::TradeTick batch[8];

    auto wall_start = std::chrono::steady_clock::now();
    for (uint64_t i = 0; i < num_ticks; ++i) {
        uint64_t now_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        bool pushed = feed->push_trade(
            /*instrument_id=*/1, /*trade_id=*/i,
            static_cast<uint8_t>(animus::TradeAggressor::Buyer),
            /*price_ticks=*/100'000, /*quantity=*/1,
            /*sequence_number=*/i, /*exchange_timestamp_ns=*/now_ns);
        if (!pushed) {
            std::fprintf(stderr, "[tick_to_trade] unexpected full trade ring at tick %llu\n",
                static_cast<unsigned long long>(i));
            std::exit(1);
        }

        size_t n = feed->poll_trades(batch, 8);
        if (n != 1) {
            std::fprintf(stderr, "[tick_to_trade] expected exactly 1 pending tick, got %zu at iteration %llu\n",
                n, static_cast<unsigned long long>(i));
            std::exit(1);
        }
        const animus::TradeTick& tick = batch[0];

        animus::OrderRequest request{
            /*client_order_id=*/tick.trade_id,
            /*instrument_id=*/tick.instrument_id,
            animus::OrderSide::Buy,
            animus::OrderType::Market,
            tick.price_ticks,
            tick.quantity,
        };
        animus::ExecutionReport report;
        client.submit(request, report);

        uint64_t after_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        latencies_ns.push_back(static_cast<double>(after_ns - tick.exchange_timestamp_ns));
    }
    auto wall_end = std::chrono::steady_clock::now();
    double wall_elapsed_s = std::chrono::duration<double>(wall_end - wall_start).count();

    std::sort(latencies_ns.begin(), latencies_ns.end());
    double sum_ns = std::accumulate(latencies_ns.begin(), latencies_ns.end(), 0.0);

    TickToTradeResult result;
    result.num_ticks = num_ticks;
    result.mean_ns = sum_ns / static_cast<double>(num_ticks);
    result.min_ns = latencies_ns.front();
    result.max_ns = latencies_ns.back();
    result.p50_ns = percentile(latencies_ns, 50);
    result.p99_ns = percentile(latencies_ns, 99);
    result.p99_9_ns = percentile(latencies_ns, 99.9);
    result.throughput_ticks_per_sec = static_cast<double>(num_ticks) / wall_elapsed_s;
    return result;
}

// -----------------------------------------------------------------------
// 2. Lock-free ring buffer throughput under real 8-thread concurrency.
// -----------------------------------------------------------------------
// Drives animus::LockFreeRingBuffer<TelemetryPayload> directly -- the
// exact ring EngineImpl's own telemetry ring uses, not a synthetic stand-
// in -- with N concurrent producer threads (real std::thread, real OS
// scheduling across real cores) all pushing at once, contending on the
// same compare-exchange retry loop. Ring is pre-sized to hold every push
// from every thread (same "no call ever blocks" methodology as the
// tick-to-trade benchmark above), so throughput reflects push() cost
// under contention, not backpressure stalls. Per-push latency is sampled
// into per-thread-local vectors (no shared results container touched
// inside the timed loop, so the measurement doesn't introduce its own
// false-sharing artifact) and merged only after every thread has joined.
struct RingThroughputResult {
    size_t num_producer_threads;
    size_t pushes_per_producer;
    size_t total_pushes;
    double elapsed_s;
    double throughput_pushes_per_sec;
    double per_push_mean_ns, per_push_p50_ns, per_push_p99_ns;
};

RingThroughputResult run_ring_throughput_benchmark(size_t num_producer_threads, size_t pushes_per_producer) {
    size_t total_pushes = num_producer_threads * pushes_per_producer;
    animus::LockFreeRingBuffer<animus::TelemetryPayload> ring(total_pushes);

    std::vector<std::vector<double>> per_thread_latencies(num_producer_threads);
    for (auto& v : per_thread_latencies) v.reserve(pushes_per_producer);

    std::vector<std::thread> producers;
    producers.reserve(num_producer_threads);

    auto t0 = std::chrono::steady_clock::now();
    for (size_t t = 0; t < num_producer_threads; ++t) {
        producers.emplace_back([&, t]() {
            std::vector<double>& latencies = per_thread_latencies[t];
            for (size_t i = 0; i < pushes_per_producer; ++i) {
                animus::TelemetryPayload payload{
                    animus::read_cycle_counter(),
                    static_cast<uint32_t>(t),
                    static_cast<uint32_t>(i),
                    static_cast<uint64_t>(i),
                };
                auto s0 = std::chrono::steady_clock::now();
                bool ok = ring.push(payload);
                auto s1 = std::chrono::steady_clock::now();
                if (!ok) {
                    std::fprintf(stderr, "[ring_throughput] unexpected full ring (thread %zu, push %zu)\n", t, i);
                    std::exit(1);
                }
                latencies.push_back(std::chrono::duration<double, std::nano>(s1 - s0).count());
            }
        });
    }
    for (auto& th : producers) th.join();
    auto t1 = std::chrono::steady_clock::now();
    double elapsed_s = std::chrono::duration<double>(t1 - t0).count();

    std::vector<double> all_latencies;
    all_latencies.reserve(total_pushes);
    for (auto& v : per_thread_latencies) {
        all_latencies.insert(all_latencies.end(), v.begin(), v.end());
    }
    std::sort(all_latencies.begin(), all_latencies.end());
    double sum_ns = std::accumulate(all_latencies.begin(), all_latencies.end(), 0.0);

    // Untimed correctness check: every push must be drainable exactly
    // once. Not a benchmarked figure -- a sanity check that the reported
    // throughput isn't hiding lost or corrupted pushes under contention.
    size_t drained = 0;
    animus::TelemetryPayload discard;
    while (ring.pop(discard)) ++drained;
    if (drained != total_pushes) {
        std::fprintf(stderr, "[ring_throughput] correctness check failed: pushed %zu, drained %zu\n",
            total_pushes, drained);
        std::exit(1);
    }

    RingThroughputResult result;
    result.num_producer_threads = num_producer_threads;
    result.pushes_per_producer = pushes_per_producer;
    result.total_pushes = total_pushes;
    result.elapsed_s = elapsed_s;
    result.throughput_pushes_per_sec = static_cast<double>(total_pushes) / elapsed_s;
    result.per_push_mean_ns = sum_ns / static_cast<double>(total_pushes);
    result.per_push_p50_ns = percentile(all_latencies, 50);
    result.per_push_p99_ns = percentile(all_latencies, 99);
    return result;
}

// -----------------------------------------------------------------------
// 3. CPU cache locality: pointer-chase latency vs. working-set size, plus
//    a false-sharing A/B test tied directly to this codebase's own
//    alignas(64) design choice (LockFreeRingBuffer's enqueue_pos_/
//    dequeue_pos_, SpscRingBuffer's head_/tail_, TelemetryPayload itself).
// -----------------------------------------------------------------------
// One 64-byte node per cache line, so a working-set size in bytes maps
// directly to a cache-line count -- the classic "membench" pointer-chase
// design (lmbench, Google's "cache killer" microbenchmarks, ...): each
// node holds only a `next` pointer, and the chain is built from a
// Sattolo-shuffled permutation (a single N-cycle covering every node, no
// short sub-cycles) so each jump is data-dependent on the *previous*
// jump's result and effectively unpredictable to the hardware prefetcher
// -- a plain sequential or Fisher-Yates-shuffled walk (which can produce
// short sub-cycles) would let the prefetcher or branch predictor partly
// hide the latency this benchmark exists to expose.
struct alignas(64) ChaseNode {
    ChaseNode* next;
    char _pad[64 - sizeof(void*)];
};
static_assert(sizeof(ChaseNode) == 64, "ChaseNode must be exactly one cache line for size<->line-count correspondence");

std::vector<ChaseNode> build_chase_buffer(size_t num_nodes, std::mt19937_64& rng) {
    std::vector<size_t> perm(num_nodes);
    for (size_t i = 0; i < num_nodes; ++i) perm[i] = i;
    // Sattolo's algorithm (note: j is drawn from [0, i-1], strictly less
    // than i -- that's what makes this Sattolo's variant, guaranteeing a
    // single N-cycle, rather than plain Fisher-Yates, which can produce
    // multiple shorter cycles).
    for (size_t i = num_nodes - 1; i > 0; --i) {
        std::uniform_int_distribution<size_t> dist(0, i - 1);
        size_t j = dist(rng);
        std::swap(perm[i], perm[j]);
    }
    std::vector<ChaseNode> nodes(num_nodes);
    for (size_t i = 0; i < num_nodes; ++i) {
        nodes[i].next = &nodes[perm[i]];
    }
    return nodes;
}

double measure_chase_latency_ns(std::vector<ChaseNode>& nodes, size_t steps) {
    ChaseNode* p = &nodes[0];
    auto t0 = std::chrono::steady_clock::now();
    for (size_t i = 0; i < steps; ++i) {
        p = p->next;
    }
    auto t1 = std::chrono::steady_clock::now();
    // Publish the final pointer through a volatile sink so the compiler
    // cannot prove the loop's result is unused and eliminate it.
    static void* volatile sink;
    sink = p;
    (void)sink;
    return std::chrono::duration<double, std::nano>(t1 - t0).count() / static_cast<double>(steps);
}

struct CachePoint { size_t size_bytes; double avg_ns_per_access; };

std::vector<CachePoint> run_cache_locality_sweep(std::mt19937_64& rng) {
    static const size_t kSizesBytes[] = {
        4 * 1024, 8 * 1024, 16 * 1024, 32 * 1024, 64 * 1024, 128 * 1024, 256 * 1024,
        512 * 1024, 1024 * 1024, 2 * 1024 * 1024, 4 * 1024 * 1024, 8 * 1024 * 1024,
        16 * 1024 * 1024, 32 * 1024 * 1024, 64 * 1024 * 1024, 128 * 1024 * 1024,
    };
    constexpr size_t STEPS = 3'000'000;

    std::vector<CachePoint> points;
    for (size_t size_bytes : kSizesBytes) {
        size_t num_nodes = std::max<size_t>(2, size_bytes / sizeof(ChaseNode));
        std::vector<ChaseNode> nodes = build_chase_buffer(num_nodes, rng);
        double avg_ns = measure_chase_latency_ns(nodes, STEPS);
        points.push_back(CachePoint{ num_nodes * sizeof(ChaseNode), avg_ns });
    }
    return points;
}

// Two atomic counters incremented by two concurrent threads, one
// iteration count apiece: Unpadded places both on the same cache line
// (the classic false-sharing setup -- every increment on either thread
// invalidates the other core's cached copy of the line); Padded places
// each on its own cache line via alignas(64), exactly the layout this
// codebase's own LockFreeRingBuffer/SpscRingBuffer already use for their
// producer/consumer index atomics. This ties "cache locality" back to a
// real, already-shipped design decision instead of a purely abstract
// microbenchmark.
struct UnpaddedCounters {
    std::atomic<uint64_t> a{ 0 };
    std::atomic<uint64_t> b{ 0 };
};
struct PaddedCounters {
    alignas(64) std::atomic<uint64_t> a{ 0 };
    alignas(64) std::atomic<uint64_t> b{ 0 };
};

template <typename Counters>
double measure_increment_throughput(uint64_t iterations_per_thread) {
    Counters counters;
    auto t0 = std::chrono::steady_clock::now();
    std::thread ta([&]() { for (uint64_t i = 0; i < iterations_per_thread; ++i) counters.a.fetch_add(1, std::memory_order_relaxed); });
    std::thread tb([&]() { for (uint64_t i = 0; i < iterations_per_thread; ++i) counters.b.fetch_add(1, std::memory_order_relaxed); });
    ta.join();
    tb.join();
    auto t1 = std::chrono::steady_clock::now();
    double elapsed_s = std::chrono::duration<double>(t1 - t0).count();
    return (2.0 * static_cast<double>(iterations_per_thread)) / elapsed_s; // combined ops/sec, both threads
}

struct FalseSharingResult { double unpadded_ops_per_sec, padded_ops_per_sec, speedup_factor; };

FalseSharingResult run_false_sharing_benchmark(uint64_t iterations_per_thread) {
    FalseSharingResult result;
    result.unpadded_ops_per_sec = measure_increment_throughput<UnpaddedCounters>(iterations_per_thread);
    result.padded_ops_per_sec = measure_increment_throughput<PaddedCounters>(iterations_per_thread);
    result.speedup_factor = result.padded_ops_per_sec / result.unpadded_ops_per_sec;
    return result;
}

// -----------------------------------------------------------------------
// JSON emission -- hand-rolled, not a library dependency (matches this
// project's zero-external-dependency posture; see CLAUDE.md). Every field
// here is a number or a compile-time-known string, so no escaping is
// needed for correctness.
// -----------------------------------------------------------------------
std::string json_tick_to_trade(const TickToTradeResult& r) {
    std::ostringstream os;
    os << std::fixed;
    os << "{\"num_ticks\":" << r.num_ticks
       << ",\"mean_ns\":" << std::setprecision(2) << r.mean_ns
       << ",\"min_ns\":" << std::setprecision(2) << r.min_ns
       << ",\"max_ns\":" << std::setprecision(2) << r.max_ns
       << ",\"p50_ns\":" << std::setprecision(2) << r.p50_ns
       << ",\"p99_ns\":" << std::setprecision(2) << r.p99_ns
       << ",\"p99_9_ns\":" << std::setprecision(2) << r.p99_9_ns
       << ",\"throughput_ticks_per_sec\":" << std::setprecision(1) << r.throughput_ticks_per_sec
       << "}";
    return os.str();
}

std::string json_ring_throughput(const RingThroughputResult& r) {
    std::ostringstream os;
    os << std::fixed;
    os << "{\"num_producer_threads\":" << r.num_producer_threads
       << ",\"pushes_per_producer\":" << r.pushes_per_producer
       << ",\"total_pushes\":" << r.total_pushes
       << ",\"elapsed_s\":" << std::setprecision(4) << r.elapsed_s
       << ",\"throughput_pushes_per_sec\":" << std::setprecision(1) << r.throughput_pushes_per_sec
       << ",\"per_push_mean_ns\":" << std::setprecision(2) << r.per_push_mean_ns
       << ",\"per_push_p50_ns\":" << std::setprecision(2) << r.per_push_p50_ns
       << ",\"per_push_p99_ns\":" << std::setprecision(2) << r.per_push_p99_ns
       << "}";
    return os.str();
}

std::string json_cache_locality(const std::vector<CachePoint>& points, const FalseSharingResult& fs) {
    std::ostringstream os;
    os << std::fixed;
    os << "{\"points\":[";
    for (size_t i = 0; i < points.size(); ++i) {
        if (i) os << ",";
        os << "{\"size_bytes\":" << points[i].size_bytes
           << ",\"avg_ns_per_access\":" << std::setprecision(3) << points[i].avg_ns_per_access << "}";
    }
    os << "],\"false_sharing\":{"
       << "\"unpadded_ops_per_sec\":" << std::setprecision(1) << fs.unpadded_ops_per_sec
       << ",\"padded_ops_per_sec\":" << std::setprecision(1) << fs.padded_ops_per_sec
       << ",\"speedup_factor\":" << std::setprecision(3) << fs.speedup_factor
       << "}}";
    return os.str();
}

std::string json_system_info() {
    unsigned cpu_count = std::thread::hardware_concurrency();
    std::string platform =
#if defined(_WIN32)
        "Windows";
#elif defined(__linux__)
        "Linux";
#elif defined(__APPLE__)
        "macOS";
#else
        "Unknown";
#endif
    std::ostringstream compiler;
#if defined(_MSC_VER)
    compiler << "MSVC " << _MSC_VER;
#elif defined(__clang__)
    compiler << "Clang " << __clang_major__ << "." << __clang_minor__;
#elif defined(__GNUC__)
    compiler << "GCC " << __GNUC__ << "." << __GNUC_MINOR__;
#else
    compiler << "Unknown";
#endif
    std::ostringstream os;
    os << "{\"cpu_count\":" << (cpu_count ? cpu_count : 1)
       << ",\"platform\":\"" << platform << "\""
       << ",\"compiler\":\"" << compiler.str() << "\""
       << "}";
    return os.str();
}

} // namespace

int main() {
    std::fprintf(stderr, "[animus_benchmark_suite] running tick-to-trade benchmark (500,000 ticks)...\n");
    TickToTradeResult ttt = run_tick_to_trade_benchmark(500'000);
    std::printf("BENCHMARK_RESULT_TICK_TO_TRADE %s\n", json_tick_to_trade(ttt).c_str());
    std::fflush(stdout);

    std::fprintf(stderr, "[animus_benchmark_suite] running ring buffer throughput benchmark (8 producer threads)...\n");
    RingThroughputResult ring = run_ring_throughput_benchmark(/*num_producer_threads=*/8, /*pushes_per_producer=*/200'000);
    std::printf("BENCHMARK_RESULT_RING_THROUGHPUT %s\n", json_ring_throughput(ring).c_str());
    std::fflush(stdout);

    std::fprintf(stderr, "[animus_benchmark_suite] running cache locality sweep + false-sharing A/B test...\n");
    std::mt19937_64 rng(0xA173C0DEu); // fixed seed: reproducible permutations run-to-run
    std::vector<CachePoint> points = run_cache_locality_sweep(rng);
    FalseSharingResult fs = run_false_sharing_benchmark(/*iterations_per_thread=*/20'000'000);
    std::printf("BENCHMARK_RESULT_CACHE_LOCALITY %s\n", json_cache_locality(points, fs).c_str());
    std::fflush(stdout);

    std::printf("BENCHMARK_RESULT_SYSTEM_INFO %s\n", json_system_info().c_str());
    std::fflush(stdout);

    std::fprintf(stderr, "[animus_benchmark_suite] done.\n");
    return 0;
}
