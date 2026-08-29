// Phase 6 benchmark: real cross-process latency measurement for
// include/animus/shm_ipc.hpp's ShmRing<T>, run specifically to check the
// "sub-50ns IPC latency" target -- not estimated, not measured in-process
// (a two-thread-in-one-process version would still cross real cores, but
// would not exercise SharedMemoryRegion's actual create()/open() mapping
// path the way two independent OS processes do).
//
// This binary IS both sides: which one it plays is selected by argv, and
// the two invocations must be launched as two separate OS processes (not
// two threads of one process) for this to be a genuine cross-process
// measurement:
//
//   shm_ipc_bench.exe --consumer <ring_name> <iterations> [results_json_path]
//   shm_ipc_bench.exe --producer <ring_name> <iterations>
//
// Start the consumer FIRST -- it owns ring creation (ShmRing::create) and
// unlink, mirroring cluster_latency_bench.cpp's own "the process that
// should be up first, is" convention. The producer retries ShmRing::open
// for up to 5 seconds to absorb the startup race if it's launched an
// instant before the consumer has finished creating the segment.
//
// Latency methodology: the producer stamps a raw TSC read (rdtsc(), same
// intrinsic as animus.hpp's read_cycle_counter(), reimplemented locally
// here rather than pulling in all of animus.hpp for one intrinsic) into
// each message immediately before push_spin(); the consumer takes its own
// rdtsc() reading immediately after pop_spin() returns and reports the
// delta, converted to nanoseconds via a wall-clock TSC calibration each
// process performs independently at startup (turbo/power-state changes
// mean the real cycles/ns ratio during a run can differ from any nominal
// "rated" frequency, so this is measured, not assumed). This relies on the
// TSC being synchronized and running at the same rate across cores/
// sockets on this machine (the "invariant TSC" feature every x86_64 CPU
// since ~2008 has) -- true of essentially all modern x86_64 hardware, but
// worth stating as the assumption it is rather than silently relying on
// it. The reported number therefore includes the producer's own
// rdtsc()-to-store gap and the consumer's own load-to-rdtsc() gap (a few
// instructions each) as part of the "latency" -- this is intentionally an
// honest end-to-end number, not an idealized zero-overhead one.
#include "../include/animus/shm_ipc.hpp"
#include "../include/animus/thread_affinity.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#pragma intrinsic(__rdtsc)
#endif

namespace {

    struct Msg {
        uint64_t sequence;
        uint64_t timestamp_cycles;
    };
    static_assert(std::is_trivially_copyable<Msg>::value, "Msg must be trivially copyable to live in ShmRing<Msg>");

    // Mirrors animus.hpp's read_cycle_counter() -- kept local to this
    // benchmark rather than included from animus.hpp, since shm_ipc.hpp
    // itself has no dependency on animus.hpp and this file shouldn't
    // introduce one just for a single intrinsic.
    inline uint64_t rdtsc() noexcept {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
        return __rdtsc();
#elif defined(__x86_64__) || defined(__i386__)
        return __builtin_ia32_rdtsc();
#else
        return static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
    }

    // Measures this process's own real (turbo/power-state-affected)
    // cycles-per-nanosecond ratio over a short busy-wait window, rather
    // than trusting a nominal rated CPU frequency.
    double calibrate_ns_per_cycle() noexcept {
        using clock = std::chrono::steady_clock;
        constexpr auto kWindow = std::chrono::milliseconds(200);
        const auto t0 = clock::now();
        const uint64_t c0 = rdtsc();
        const auto deadline = t0 + kWindow;
        while (clock::now() < deadline) {
            // busy-wait: sleeping here would let the OS put this core into
            // a lower power/frequency state, defeating the point of
            // calibrating against the frequency this benchmark will
            // actually run at.
        }
        const uint64_t c1 = rdtsc();
        const auto t1 = clock::now();
        const double elapsed_ns = std::chrono::duration<double, std::nano>(t1 - t0).count();
        const double elapsed_cycles = static_cast<double>(c1 - c0);
        return elapsed_ns / elapsed_cycles;
    }

    // Same linear-interpolation percentile as animus_benchmark_suite.cpp's
    // percentile() (and benchmarks/fintech_tail_latency.py's), so numbers
    // from either benchmark are directly comparable.
    double percentile(const std::vector<double>& sorted_data, double pct) {
        if (sorted_data.empty()) return 0.0;
        if (sorted_data.size() == 1) return sorted_data[0];
        const double k = (pct / 100.0) * static_cast<double>(sorted_data.size() - 1);
        const size_t f = static_cast<size_t>(std::floor(k));
        const size_t c = static_cast<size_t>(std::ceil(k));
        if (f == c) return sorted_data[static_cast<size_t>(k)];
        const double d0 = sorted_data[f] * (static_cast<double>(c) - k);
        const double d1 = sorted_data[c] * (k - static_cast<double>(f));
        return d0 + d1;
    }

    struct LatencyStats {
        size_t num_samples = 0;
        double mean_ns = 0, min_ns = 0, max_ns = 0, p50_ns = 0, p99_ns = 0, p99_9_ns = 0;
    };

    LatencyStats summarize(std::vector<double> latencies_ns) {
        LatencyStats s;
        if (latencies_ns.empty()) return s;
        std::sort(latencies_ns.begin(), latencies_ns.end());
        s.num_samples = latencies_ns.size();
        s.min_ns = latencies_ns.front();
        s.max_ns = latencies_ns.back();
        s.p50_ns = percentile(latencies_ns, 50);
        s.p99_ns = percentile(latencies_ns, 99);
        s.p99_9_ns = percentile(latencies_ns, 99.9);
        s.mean_ns = std::accumulate(latencies_ns.begin(), latencies_ns.end(), 0.0) / static_cast<double>(latencies_ns.size());
        return s;
    }

    void write_results_json(const std::string& path, const LatencyStats& s) {
        std::ofstream out(path, std::ios::trunc);
        out << std::fixed;
        out << "{\"num_samples\":" << s.num_samples
            << ",\"mean_ns\":" << s.mean_ns
            << ",\"min_ns\":" << s.min_ns
            << ",\"max_ns\":" << s.max_ns
            << ",\"p50_ns\":" << s.p50_ns
            << ",\"p99_ns\":" << s.p99_ns
            << ",\"p99_9_ns\":" << s.p99_9_ns
            << ",\"sub_50ns_p50\":" << (s.p50_ns < 50.0 ? "true" : "false")
            << "}";
    }

    int run_consumer(const std::string& ring_name, uint64_t iterations, const std::string& results_path) {
        // Best-effort: this benchmark exists specifically to exercise
        // thread_affinity.hpp alongside shm_ipc.hpp, and a consumer that
        // migrates cores mid-run would show up directly as latency-tail
        // noise in the very numbers this benchmark reports.
        animus::sys::pin_current_thread_to_core(0);
        animus::sys::set_thread_high_priority();
        const double ns_per_cycle = calibrate_ns_per_cycle();
        std::fprintf(stderr, "[consumer] calibrated %.4f ns/cycle (~%.2f GHz)\n", ns_per_cycle, 1.0 / ns_per_cycle);

        // Two small rings, not one: fwd_ring carries producer->consumer
        // messages, ack_ring carries a same-sequence reply back. The
        // producer blocks on the ack before sending the next message (see
        // run_producer below), so at most ONE message is ever in flight --
        // a deliberate lockstep, same methodology as
        // animus_benchmark_suite.cpp's tick-to-trade benchmark. An earlier
        // version of this file used a single, large-capacity ring with an
        // unpaced producer and measured 50-100+ MICROSECONDS here, not
        // nanoseconds: with nothing to slow it down, the producer burst
        // ahead of the consumer and built a standing backlog inside the
        // ring, so the "latency" being measured was queueing delay behind
        // that backlog, not transit time -- the exact "unpaced burst looks
        // like catastrophic latency" pitfall this codebase already hit
        // once before and documented (BENCHMARKS.md's Phase 16 section).
        // Capacity is intentionally small (64): the lockstep invariant
        // means depth never exceeds 1 in either direction, so there is
        // nothing to gain from a larger ring here.
        const std::string fwd_name = ring_name + "_fwd";
        const std::string ack_name = ring_name + "_ack";
        auto fwd_ring = animus::sys::ipc::ShmRing<Msg>::create(fwd_name.c_str(), /*requested_capacity=*/64);
        auto ack_ring = animus::sys::ipc::ShmRing<Msg>::create(ack_name.c_str(), /*requested_capacity=*/64);
        if (!fwd_ring || !ack_ring) {
            std::fprintf(stderr, "[consumer] failed to create rings '%s'/'%s'\n", fwd_name.c_str(), ack_name.c_str());
            return 1;
        }
        std::fprintf(stderr, "[consumer] rings ready, waiting for producer...\n");

        std::vector<double> latencies_ns;
        latencies_ns.reserve(iterations);
        Msg msg{};
        for (uint64_t i = 0; i < iterations; ++i) {
            if (!fwd_ring->pop_spin(msg)) {
                std::fprintf(stderr, "[consumer] timed out waiting for message %llu\n",
                    static_cast<unsigned long long>(i));
                return 1;
            }
            const uint64_t now_cycles = rdtsc(); // read as early as possible after pop_spin returns
            if (msg.sequence != i) {
                std::fprintf(stderr, "[consumer] sequence mismatch: expected %llu, got %llu\n",
                    static_cast<unsigned long long>(i), static_cast<unsigned long long>(msg.sequence));
                return 1;
            }
            latencies_ns.push_back(static_cast<double>(now_cycles - msg.timestamp_cycles) * ns_per_cycle);

            const Msg ack{ i, 0 };
            if (!ack_ring->push_spin(ack)) {
                std::fprintf(stderr, "[consumer] failed to send ack for message %llu\n",
                    static_cast<unsigned long long>(i));
                return 1;
            }
        }

        const LatencyStats stats = summarize(std::move(latencies_ns));
        write_results_json(results_path, stats);
        std::fprintf(stderr,
            "[consumer] done: n=%zu mean=%.2fns min=%.2fns p50=%.2fns p99=%.2fns p99.9=%.2fns max=%.2fns\n",
            stats.num_samples, stats.mean_ns, stats.min_ns, stats.p50_ns, stats.p99_ns, stats.p99_9_ns, stats.max_ns);
        std::fprintf(stderr, "[consumer] p50 %s sub-50ns\n", stats.p50_ns < 50.0 ? "IS" : "is NOT");

        animus::sys::ipc::ShmRing<Msg>::unlink(fwd_name.c_str());
        animus::sys::ipc::ShmRing<Msg>::unlink(ack_name.c_str());
        return 0;
    }

    int run_producer(const std::string& ring_name, uint64_t iterations) {
        animus::sys::pin_current_thread_to_core(1);
        animus::sys::set_thread_high_priority();

        const std::string fwd_name = ring_name + "_fwd";
        const std::string ack_name = ring_name + "_ack";
        std::unique_ptr<animus::sys::ipc::ShmRing<Msg>> fwd_ring, ack_ring;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            fwd_ring = animus::sys::ipc::ShmRing<Msg>::open(fwd_name.c_str());
            ack_ring = animus::sys::ipc::ShmRing<Msg>::open(ack_name.c_str());
            if (fwd_ring && ack_ring) break;
            fwd_ring.reset();
            ack_ring.reset();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (!fwd_ring || !ack_ring) {
            std::fprintf(stderr, "[producer] could not open rings '%s'/'%s' within 5s -- start the consumer first\n",
                fwd_name.c_str(), ack_name.c_str());
            return 1;
        }
        std::fprintf(stderr, "[producer] rings opened, sending %llu messages (lockstep, 1 in flight)...\n",
            static_cast<unsigned long long>(iterations));

        for (uint64_t i = 0; i < iterations; ++i) {
            // Timestamp taken as late as possible -- immediately before
            // the value is handed to push_spin -- so the measured latency
            // is as close as this benchmark can get to "time from the
            // producer's store becoming visible" rather than including
            // extra producer-side bookkeeping.
            const Msg msg{ i, rdtsc() };
            if (!fwd_ring->push_spin(msg)) {
                std::fprintf(stderr, "[producer] push_spin failed at message %llu (consumer gone?)\n",
                    static_cast<unsigned long long>(i));
                return 1;
            }
            // Block for the ack before sending the next message -- this is
            // what enforces the lockstep (at most 1 message ever pending)
            // that makes the consumer's timestamp delta a genuine transit
            // latency instead of a queueing delay. See the comment in
            // run_consumer above for what happens without it.
            Msg ack{};
            if (!ack_ring->pop_spin(ack)) {
                std::fprintf(stderr, "[producer] timed out waiting for ack %llu\n",
                    static_cast<unsigned long long>(i));
                return 1;
            }
        }
        std::fprintf(stderr, "[producer] done: sent %llu messages\n", static_cast<unsigned long long>(iterations));
        return 0;
    }

} // namespace

int main(int argc, char** argv) {
    if (argc >= 4 && std::strcmp(argv[1], "--consumer") == 0) {
        const std::string ring_name = argv[2];
        const uint64_t iterations = std::strtoull(argv[3], nullptr, 10);
        const std::string results_path = argc >= 5 ? argv[4] : "shm_ipc_bench_results.json";
        return run_consumer(ring_name, iterations, results_path);
    }
    if (argc >= 4 && std::strcmp(argv[1], "--producer") == 0) {
        const std::string ring_name = argv[2];
        const uint64_t iterations = std::strtoull(argv[3], nullptr, 10);
        return run_producer(ring_name, iterations);
    }
    std::fprintf(stderr,
        "Usage:\n"
        "  %s --consumer <ring_name> <iterations> [results_json_path]\n"
        "  %s --producer <ring_name> <iterations>\n"
        "Launch these as two SEPARATE OS processes, consumer first (it owns\n"
        "ring creation/unlink) -- see this file's own header comment.\n",
        argv[0], argv[0]);
    return 2;
}
