// Animus Engine -- Cross-Process Shared-Memory Producer Benchmark (C++17)
//
// Producer half of the Milestone 4 evaluation harness. Unlike
// telemetry_benchmark.cpp (an in-process, single-binary proof point over
// animus::SpscRingBuffer), this binary is one of *two independent OS
// processes*: it creates a named shared-memory ring (Windows:
// CreateFileMapping; POSIX: shm_open/mmap under /dev/shm) and injects
// synthetic execution events into it, hardware-timestamping each one. A
// consumer attaches to the identical segment from a separate Python process
// and drains it, with no serialization step and no IPC mechanism between
// the two beyond the shared pages themselves.
//
// Default mode is decoupled/overwrite, on animus::sys::ipc::BroadcastRing<
// ExecutionEvent> (include/animus/broadcast_ring.hpp): publish() never waits
// on any reader, so this binary is fully self-contained and meaningful to run
// with no consumer attached at all. Readers (eval_kit/scripts/verify_stream.py,
// bindings/animus_shm_py.cpp's BroadcastRing) keep their own cursors and count
// their own drops; the writer cannot know what any reader missed, so the
// "dropped" figure reported here is the writer-side bound -- how many of the
// events are no longer readable once the run ends (everything beyond the
// newest capacity - 1).
//
// --backpressure switches to the legacy animus::sys::ipc::ShmRing<
// ExecutionEvent> (include/animus/shm_ipc.hpp) and its bounded-retry
// push_spin(), for measuring true end-to-end throughput with consumer.py
// running concurrently and nothing lost. A broadcast ring has no
// backpressure by design, so that mode cannot move to it; consumer.py and the
// CI zero-loss check read the legacy segment layout, and stay on it until they
// are migrated to the lossless SpscQueue (include/animus/spsc_queue.hpp).
//
// Timestamping follows telemetry_benchmark.cpp's own methodology: a
// serialized RDTSC read (x86, calibrated against std::chrono::steady_clock
// over a dedicated window rather than an assumed base clock) or
// clock_gettime(CLOCK_MONOTONIC_RAW) on non-x86, taken immediately before
// and after each push call, so the recorded latency is exactly the cost of
// that one push -- ring-transport cost in backpressure mode, or the
// (deliberately near-constant, near-zero) cost of a write-plus-branch in
// overwrite mode. Both are real, reportable numbers, not a benchmark
// artifact to explain away.

#include "animus/broadcast_ring.hpp"
#include "animus/shm_ipc.hpp"
#include "animus/shm_lifecycle.hpp"
#include "animus/execution_event.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
    #define ANIMUS_ARCH_X86 1
#else
    #define ANIMUS_ARCH_X86 0
#endif

#if ANIMUS_ARCH_X86
    #if defined(_MSC_VER)
        #include <intrin.h>
    #else
        #include <x86intrin.h>
    #endif
#endif

#if !defined(_WIN32)
    #include <time.h>
#endif

namespace animus_harness {

// Wire record pushed through the shared-memory ring -- animus::ExecutionEvent
// (include/animus/execution_event.hpp), the single shared definition also
// used by bindings/animus_shm_py.cpp's nanobind consumer, so the producer
// here and any consumer attaching to the same segment agree on the exact
// byte layout by construction rather than by two hand-copied definitions
// staying in sync. benchmarks/consumer.py is the one exception (no C++
// toolchain at runtime) and hardcodes the matching struct format instead.
using animus::ExecutionEvent;
using animus::kExecutionEventWireFormat;
constexpr const char* kWireFormat = kExecutionEventWireFormat;

using Broadcast = animus::sys::ipc::BroadcastRing<ExecutionEvent>; // overwrite mode
using LegacyRing = animus::sys::ipc::ShmRing<ExecutionEvent>;      // backpressure mode only
using Region = animus::sys::ipc::SharedMemoryRegion;

// Allocates the named segment for at least `requested_capacity` slots
// (rounded up to a power of two, minimum 2, as ShmRing::create did) and
// initialises the writer's view of it. False on a name collision, an OS
// failure, or a capacity whose byte size would overflow size_t.
bool create_broadcast(const char* name, size_t requested_capacity, Region& region, Broadcast& writer) {
    const size_t max_capacity = (SIZE_MAX - Broadcast::required_bytes(0)) / sizeof(ExecutionEvent);
    size_t capacity = 2;
    while (capacity < requested_capacity) {
        if (capacity > max_capacity / 2) return false;
        capacity <<= 1;
    }
    if (!Region::create(name, Broadcast::required_bytes(capacity), region)) return false;
    if (!Broadcast::init(region.data(), region.size(), capacity, writer)) {
        region.close();
        Region::unlink(name); // don't leave a half-built segment behind
        return false;
    }
    return true;
}

#if ANIMUS_ARCH_X86
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
        // busy-wait deliberately -- see telemetry_benchmark.cpp's identical
        // comment: a sleep here risks a core migration corrupting the delta.
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
double calibrate_units_per_ns() noexcept { return 1.0; } // sample_clock() already returns ns
const char* clock_source_name() noexcept { return "clock_gettime(CLOCK_MONOTONIC_RAW)"; }
#endif

struct PercentileReportNs {
    double min = 0, p50 = 0, p90 = 0, p99 = 0, p999 = 0, max = 0;
};

PercentileReportNs summarize_ns(std::vector<uint64_t>& samples_raw, double units_per_ns) {
    std::sort(samples_raw.begin(), samples_raw.end());
    const size_t n = samples_raw.size();
    auto at = [&](double p) -> uint64_t {
        size_t idx = static_cast<size_t>(p * static_cast<double>(n - 1));
        if (idx >= n) idx = n - 1;
        return samples_raw[idx];
    };
    auto to_ns = [&](uint64_t raw) { return static_cast<double>(raw) / units_per_ns; };
    PercentileReportNs r;
    r.min = to_ns(samples_raw.front());
    r.p50 = to_ns(at(0.50));
    r.p90 = to_ns(at(0.90));
    r.p99 = to_ns(at(0.99));
    r.p999 = to_ns(at(0.999));
    r.max = to_ns(samples_raw.back());
    return r;
}

ExecutionEvent make_synthetic_event(uint64_t sequence) noexcept {
    ExecutionEvent ev{};
    ev.sequence = sequence;
    ev.price_ticks = static_cast<int64_t>(100000 + (sequence % 4096));
    ev.quantity = static_cast<int64_t>(1 + (sequence % 500));
    ev.instrument_id = static_cast<uint32_t>(sequence % 256);
    ev.flags = 0;
    return ev;
}

struct Options {
    std::string name = "animus_harness_shm";
    uint64_t event_count = 10'000'000ull;
    size_t ring_capacity = 1 << 20; // 1,048,576 slots * 40 bytes = 40 MiB segment
    int producer_core = -1;
    bool backpressure = false;
    bool unlink_when_done = false;
    std::string json_path = "harness_benchmark.json";
};

Options parse_args(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&](const char* flag) -> std::string {
            if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", flag); std::exit(2); }
            return argv[++i];
        };
        if (arg == "--name") o.name = next("--name");
        else if (arg == "--events") o.event_count = std::strtoull(next("--events").c_str(), nullptr, 10);
        else if (arg == "--capacity") o.ring_capacity = static_cast<size_t>(std::strtoull(next("--capacity").c_str(), nullptr, 10));
        else if (arg == "--core") o.producer_core = std::atoi(next("--core").c_str());
        else if (arg == "--backpressure") o.backpressure = true;
        else if (arg == "--mode") {
            // Preferred, explicit spelling of the same choice --backpressure
            // toggles; kept as a separate branch (not just an alias table)
            // so an unrecognized mode value fails loudly instead of
            // silently doing nothing, which --unknown-flag already does not.
            const std::string mode = next("--mode");
            if (mode == "overwrite") o.backpressure = false;
            else if (mode == "backpressure") o.backpressure = true;
            else {
                std::fprintf(stderr, "unknown --mode '%s' (expected 'overwrite' or 'backpressure')\n", mode.c_str());
                std::exit(2);
            }
        }
        else if (arg == "--unlink-when-done") o.unlink_when_done = true;
        else if (arg == "--json") o.json_path = next("--json");
        else if (arg == "--help") {
            std::printf(
                "usage: harness_benchmark [--name NAME] [--events N] [--capacity SLOTS]\n"
                "                          [--core CPU] [--mode overwrite|backpressure] [--unlink-when-done]\n"
                "                          [--json PATH]\n"
                "  --name             shared-memory segment name the consumer must match (default: animus_harness_shm)\n"
                "  --events           synthetic events to inject (default: 10000000)\n"
                "  --capacity         ring capacity in slots, rounded up to a power of two (default: 1048576)\n"
                "  --mode             'overwrite' (default): lossy BroadcastRing, self-contained, read it with\n"
                "                     eval_kit/scripts/verify_stream.py; or 'backpressure': legacy ShmRing with\n"
                "                     bounded-retry push_spin(), needs a live benchmarks/consumer.py\n"
                "  --backpressure     shorthand for --mode backpressure\n"
                "  --unlink-when-done destroy the segment after the run (default: leave it for consumer.py)\n");
            std::exit(0);
        } else {
            std::fprintf(stderr, "unknown argument: %s (--help for usage)\n", arg.c_str());
            std::exit(2);
        }
    }
    return o;
}

} // namespace animus_harness

int main(int argc, char** argv) {
    using namespace animus_harness;
    using namespace std::chrono;

    const Options opt = parse_args(argc, argv);

    std::printf("Animus Engine -- Cross-Process SHM Producer Harness\n");
    std::printf("=====================================================\n");
    std::printf("Segment name:      %s\n", opt.name.c_str());
    std::printf("Events:            %llu\n", static_cast<unsigned long long>(opt.event_count));
    std::printf("Ring capacity:     %zu slots (%zu bytes/slot)\n", opt.ring_capacity, sizeof(ExecutionEvent));
    std::printf("Mode:              %s\n", opt.backpressure ? "backpressure (legacy ShmRing, push_spin)"
                                                           : "decoupled overwrite (BroadcastRing, publish)");
    std::printf("Clock source:      %s\n", clock_source_name());
    if (opt.producer_core >= 0) {
        animus::sys::pin_current_thread_to_core(static_cast<size_t>(opt.producer_core));
        std::printf("Pinned to core:    %d\n", opt.producer_core);
    }
    std::printf("\n");

    // Exactly one of these is live: `legacy` in backpressure mode, otherwise
    // `region` (owns the mapping) + `broadcast` (the writer's view over it).
    std::unique_ptr<LegacyRing> legacy;
    Region region;
    Broadcast broadcast;
    const bool created = opt.backpressure
        ? static_cast<bool>(legacy = LegacyRing::create(opt.name.c_str(), opt.ring_capacity))
        : create_broadcast(opt.name.c_str(), opt.ring_capacity, region, broadcast);
    if (!created) {
        std::fprintf(stderr,
            "error: creating the '%s' segment failed -- a segment with this name may already "
            "exist (clean it up, or pass a different --name) or the OS refused the "
            "shared-memory allocation.\n", opt.name.c_str());
        return 1;
    }
    if (legacy) legacy->mark_producer_attached();
    const size_t ring_capacity = legacy ? legacy->capacity() : broadcast.capacity();

    // Milestone 2: install signal handling before the hot loop so Ctrl+C
    // (SIGINT) or a supervisor's SIGTERM detaches this process's view
    // cleanly (breaks the loop below, unmaps via legacy.reset()/region.close()) rather than
    // being caught mid-push by the default handler and killing the process
    // with the segment's producer_pid still pointing at a corpse. Note
    // this process's own destructor path does NOT unlink by default (see
    // --unlink-when-done) -- the segment survives so a consumer already
    // attached, or about to attach, is never left holding a mapping to a
    // node that vanished out from under it.
    animus::sys::lifecycle::SignalGuard signal_guard;

    const double units_per_ns = calibrate_units_per_ns();
    std::printf("Calibration:       %.4f raw-units/ns\n\n", units_per_ns);

    std::vector<uint64_t> latency_samples_raw;
    latency_samples_raw.reserve(static_cast<size_t>(opt.event_count));

    const uint64_t dropped_before = legacy ? legacy->dropped_count() : 0;
    uint64_t pushed = 0;
    const auto wall_start = steady_clock::now();

    for (uint64_t i = 0; i < opt.event_count; ++i) {
        if (animus::sys::lifecycle::SignalGuard::shutdown_requested()) {
            std::printf("\nShutdown requested (SIGINT/SIGTERM) -- detaching cleanly after %llu/%llu events.\n",
                        static_cast<unsigned long long>(i), static_cast<unsigned long long>(opt.event_count));
            break;
        }
        const ExecutionEvent ev = make_synthetic_event(i);
        const uint64_t t0 = sample_clock();
        if (legacy) {
            // Bounded retry, not an unbounded blocking wait -- see
            // ShmRing::push_spin's own doc comment (shm_ipc.hpp). A
            // consumer that has died is detectable via
            // legacy->is_producer_alive()/is_consumer_alive() rather than
            // this call hanging forever.
            legacy->push_spin(ev);
        } else {
            broadcast.publish(ev);
        }
        const uint64_t t1 = sample_clock();
        latency_samples_raw.push_back(t1 - t0);
        ++pushed;
        if (legacy) legacy->producer_heartbeat();
    }

    const auto wall_end = steady_clock::now();
    // Legacy ring: the writer itself counts every overwrite. Broadcast ring:
    // readers own their drop counts, so report the writer-side bound instead --
    // everything beyond the newest capacity - 1 records is no longer readable.
    const uint64_t readable = ring_capacity - 1;
    const uint64_t dropped = legacy ? legacy->dropped_count() - dropped_before
                                    : (pushed > readable ? pushed - readable : 0);

    const PercentileReportNs report = summarize_ns(latency_samples_raw, units_per_ns);
    const double wall_seconds = duration<double>(wall_end - wall_start).count();
    const double events_per_sec = wall_seconds > 0.0 ? static_cast<double>(pushed) / wall_seconds : 0.0;

    std::printf("Per-event enqueue latency:\n");
    std::printf("  min    %12.1f ns\n", report.min);
    std::printf("  p50    %12.1f ns\n", report.p50);
    std::printf("  p90    %12.1f ns\n", report.p90);
    std::printf("  p99    %12.1f ns\n", report.p99);
    std::printf("  p99.9  %12.1f ns\n", report.p999);
    std::printf("  max    %12.1f ns\n", report.max);
    std::printf("\nThroughput:        %.3f M events/sec (%llu events, %.3f s wall)\n",
                events_per_sec / 1'000'000.0, static_cast<unsigned long long>(pushed), wall_seconds);
    std::printf("%s: %llu / %llu (%.4f%%)\n",
                legacy ? "Dropped (overwritten before consumption)"
                       : "Overwritten (no longer readable; each reader counts its own drops)",
                static_cast<unsigned long long>(dropped), static_cast<unsigned long long>(pushed),
                pushed ? 100.0 * static_cast<double>(dropped) / static_cast<double>(pushed) : 0.0);

    std::ofstream json_out(opt.json_path, std::ios::trunc);
    if (json_out.is_open()) {
        json_out << "{\n"
                 << "  \"benchmark\": \"animus_shm_harness_producer\",\n"
                 << "  \"segment_name\": \"" << opt.name << "\",\n"
                 << "  \"mode\": \"" << (opt.backpressure ? "backpressure" : "overwrite") << "\",\n"
                 << "  \"ring_type\": \"" << (legacy ? "ShmRing" : "BroadcastRing") << "\",\n"
                 << "  \"events_requested\": " << opt.event_count << ",\n"
                 << "  \"events_pushed\": " << pushed << ",\n"
                 << "  \"events_dropped\": " << dropped << ",\n"
                 << "  \"ring_capacity_slots\": " << ring_capacity << ",\n"
                 << "  \"record_size_bytes\": " << sizeof(ExecutionEvent) << ",\n"
                 << "  \"wire_format\": \"" << kWireFormat << "\",\n"
                 << "  \"clock_source\": \"" << clock_source_name() << "\",\n"
                 << "  \"latency_ns\": {\n"
                 << "    \"min\": " << report.min << ",\n"
                 << "    \"p50\": " << report.p50 << ",\n"
                 << "    \"p90\": " << report.p90 << ",\n"
                 << "    \"p99\": " << report.p99 << ",\n"
                 << "    \"p99_9\": " << report.p999 << ",\n"
                 << "    \"max\": " << report.max << "\n"
                 << "  },\n"
                 << "  \"throughput\": {\n"
                 << "    \"events_per_sec\": " << static_cast<uint64_t>(events_per_sec) << ",\n"
                 << "    \"wall_time_seconds\": " << wall_seconds << "\n"
                 << "  }\n"
                 << "}\n";
        json_out.close();
        std::printf("\nJSON report written to: %s\n", opt.json_path.c_str());
    }

    if (opt.unlink_when_done) {
        legacy.reset(); // detach this process's own view first
        region.close();
        Region::unlink(opt.name.c_str());
        std::printf("Segment '%s' unlinked.\n", opt.name.c_str());
    } else {
        std::printf("Segment '%s' left intact for %s -- run:\n"
                    "  python %s --name %s --events %llu\n",
                    opt.name.c_str(),
                    legacy ? "consumer.py" : "verify_stream.py",
                    legacy ? "consumer.py" : "scripts/verify_stream.py",
                    opt.name.c_str(), static_cast<unsigned long long>(pushed));
    }
    return 0;
}
