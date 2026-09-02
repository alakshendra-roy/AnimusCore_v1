// Animus Engine -- Telemetry Dispatch Latency & Throughput Benchmark (C++23)
//
// Institutional-grade proof-point harness for the telemetry transport used
// on the Animus hot path. Measures one-way, end-to-end dispatch latency --
// producer enqueue to consumer receipt -- across a single-producer/
// single-consumer, lock-free, zero-copy ring buffer, plus the sustained
// throughput the same pipeline can carry under continuous load.
//
// Methodology: a single producer thread stamps each TelemetryEvent with a
// serialized RDTSC reading (x86) or a nanosecond steady_clock sample
// (non-x86) at the moment it is handed to the ring, then pushes it. A
// single consumer thread busy-spins on pop() and, for every event tagged
// "measured", timestamps the instant of receipt and records the delta.
// This isolates ring-transport + cross-core cache-coherency latency from
// application logic -- the consumer does no work beyond the timestamp.
//
// 1,000,000 warm-up events run the identical push/pop path, unmeasured,
// before the loop so instruction/data caches and branch predictors are hot
// when timing starts. 10,000,000 measured events follow. Both counts are
// pushed through the very same ring instance and consumer loop; only the
// "measured" flag differs, so the timed region sees the exact steady-state
// path a real deployment would run under sustained load.
//
// Zero heap allocation in the hot loop: the ring's backing store is a
// compile-time-sized std::array embedded directly in a static-storage-
// duration SpscRingBuffer instance (BSS, not the heap, not the stack), and
// the one std::vector used to collect latency samples reserve()s its full
// 10,000,000-entry capacity before the timed region begins, so no
// reallocation is possible once push()/pop() start running.
//
// RDTSC serialization: __rdtsc() alone is not a serializing instruction and
// can be reordered by the CPU relative to surrounding code, letting work
// outside the measured region leak into a sample. _mm_lfence() is a
// lightweight serializing fence (unlike CPUID, it does not flush the
// pipeline) -- one before the read so nothing prior is still in flight,
// one after so the read itself cannot be reordered past what follows it.
// The TSC's cycle rate is *measured* against std::chrono::steady_clock over
// a dedicated calibration window rather than assumed from the advertised
// base clock, which avoids the class of error where turbo, power states,
// or virtualization make an assumed cycles/ns figure silently wrong.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

#if __has_include(<print>)
    #include <print>
    #define ANIMUS_HAS_STD_PRINT 1
#else
    #define ANIMUS_HAS_STD_PRINT 0
#endif

#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
    #define ANIMUS_ARCH_X86 1
#else
    #define ANIMUS_ARCH_X86 0
#endif

#if ANIMUS_ARCH_X86
    #if defined(_MSC_VER)
        #include <intrin.h>
    #else
        #include <cpuid.h>
        #include <x86intrin.h>
    #endif
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

namespace animus_bench {

// Thin portable wrapper so the rest of the file can call one function
// regardless of whether <print> is available; falls back to std::printf
// on toolchains where the standard library hasn't shipped it yet even
// though the compiler itself is C++23-capable (e.g. some Clang+libstdc++
// pairings older than GCC 14).
template <typename... Args>
void out(std::format_string<Args...> fmt, Args&&... args) {
#if ANIMUS_HAS_STD_PRINT
    std::print(fmt, std::forward<Args>(args)...);
#else
    std::fputs(std::format(fmt, std::forward<Args>(args)...).c_str(), stdout);
#endif
}

inline constexpr std::size_t kCacheLineSize = 64;
inline constexpr std::uint32_t kFlagMeasured = 0x1;   // counts toward the latency sample set
inline constexpr std::uint32_t kFlagThroughput = 0x2; // counts toward the throughput window

// One event = one cache line. Sized and aligned so adjacent ring slots
// never share a line -- the same false-sharing elimination the ring's own
// head_/tail_ cursors rely on, applied to the payload itself.
struct alignas(kCacheLineSize) TelemetryEvent {
    std::uint64_t sequence;      // monotonically increasing dispatch sequence number
    std::uint64_t dispatch_tsc;  // clock sample taken at enqueue time
    std::uint64_t venue_ts_ns;   // synthetic upstream venue timestamp, ns since epoch
    std::int64_t  price_ticks;   // synthetic instrument price, in ticks
    std::int64_t  quantity;      // synthetic order/quote quantity
    std::uint32_t instrument_id; // synthetic instrument identifier
    std::uint32_t flags;         // bit 0 (kFlagMeasured): counts toward the timed sample set
    std::array<std::uint64_t, 2> reserved{}; // pads struct to exactly one cache line
};
static_assert(sizeof(TelemetryEvent) == kCacheLineSize,
    "TelemetryEvent must occupy exactly one cache line");
static_assert(std::is_trivially_copyable_v<TelemetryEvent>,
    "ring slots are plain-copied, not serialized -- T must be trivially copyable");

// Single-producer/single-consumer lock-free ring, capacity fixed at compile
// time so the backing store is a plain std::array -- no heap allocation,
// ever, not even at construction. head_/tail_ are each pinned to their own
// cache line so a producer publishing head_ never invalidates the line the
// consumer is polling tail_ from, and vice versa.
template <typename T, std::size_t Capacity>
class SpscRingBuffer {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");
    static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable");

public:
    SpscRingBuffer() = default;
    SpscRingBuffer(const SpscRingBuffer&) = delete;
    SpscRingBuffer& operator=(const SpscRingBuffer&) = delete;

    // Producer-thread-only. Never blocks; returns false if the ring is full.
    bool push(const T& value) noexcept {
        const std::uint64_t head = head_.load(std::memory_order_relaxed);
        const std::uint64_t tail = tail_.load(std::memory_order_acquire);
        if (head - tail >= Capacity) return false;
        slots_[head & kMask] = value;
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    // Consumer-thread-only. Never blocks; returns false if the ring is empty.
    bool pop(T& out_value) noexcept {
        const std::uint64_t tail = tail_.load(std::memory_order_relaxed);
        const std::uint64_t head = head_.load(std::memory_order_acquire);
        if (tail == head) return false;
        out_value = slots_[tail & kMask];
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

private:
    static constexpr std::uint64_t kMask = Capacity - 1;
    std::array<T, Capacity> slots_{};

    alignas(kCacheLineSize) std::atomic<std::uint64_t> head_{0}; // producer-owned cursor
    alignas(kCacheLineSize) std::atomic<std::uint64_t> tail_{0}; // consumer-owned cursor

    static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
        "head_/tail_ must be lock-free, or a mutex/futex fallback would defeat "
        "the point of a lock-free ring");
};

#if ANIMUS_ARCH_X86
// Serialized RDTSC: see file header for why both fences are needed.
inline std::uint64_t sample_clock() noexcept {
    _mm_lfence();
    const std::uint64_t t = __rdtsc();
    _mm_lfence();
    return t;
}

// Measures TSC frequency by racing the TSC against std::chrono::steady_clock
// over a fixed wall-clock window, rather than assuming the advertised base
// clock -- turbo, power-saving states, and virtualized/emulated TSCs are
// all common sources of a silently wrong "assumed" ns conversion.
double calibrate_cycles_per_ns() noexcept {
    constexpr auto kCalibrationWindow = std::chrono::milliseconds(200);

    const auto wall_start = std::chrono::steady_clock::now();
    const std::uint64_t tsc_start = sample_clock();

    while (std::chrono::steady_clock::now() - wall_start < kCalibrationWindow) {
        // busy-wait -- a sleep-based window risks an OS-scheduled core
        // migration mid-measurement, which would corrupt the TSC delta on
        // systems without a synchronized TSC across cores.
    }

    const std::uint64_t tsc_end = sample_clock();
    const auto wall_end = std::chrono::steady_clock::now();

    const double elapsed_ns = std::chrono::duration<double, std::nano>(wall_end - wall_start).count();
    const double elapsed_cycles = static_cast<double>(tsc_end - tsc_start);
    return elapsed_cycles / elapsed_ns;
}

std::string clock_source_name() { return "RDTSC (lfence-serialized, calibrated)"; }

std::string get_cpu_brand() {
    std::array<int, 4> regs{};
#if defined(_MSC_VER)
    __cpuid(regs.data(), static_cast<int>(0x80000000));
#else
    __cpuid(0x80000000, regs[0], regs[1], regs[2], regs[3]);
#endif
    const auto max_ext = static_cast<unsigned>(regs[0]);
    if (max_ext < 0x80000004u) return "unknown";

    char brand[49] = {};
    for (unsigned leaf = 0; leaf < 3; ++leaf) {
#if defined(_MSC_VER)
        __cpuid(regs.data(), static_cast<int>(0x80000002 + leaf));
#else
        __cpuid(0x80000002 + leaf, regs[0], regs[1], regs[2], regs[3]);
#endif
        std::memcpy(brand + leaf * 16, regs.data(), sizeof(regs));
    }
    std::string result(brand);
    while (!result.empty() && result.front() == ' ') result.erase(result.begin());
    while (!result.empty() && result.back() == ' ') result.pop_back();
    return result.empty() ? "unknown" : result;
}
#else
// Non-x86 fallback: sample_clock() returns nanoseconds directly, so the
// "cycles/ns" ratio is exactly 1.0 by construction -- no TSC to calibrate.
inline std::uint64_t sample_clock() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}
double calibrate_cycles_per_ns() noexcept { return 1.0; }
std::string clock_source_name() { return "std::chrono::steady_clock"; }
std::string get_cpu_brand() { return "unknown (non-x86 target)"; }
#endif

// Best-effort thread pinning. Returns false (silently) on any platform or
// call failure -- an unpinned run is still meaningful, just noisier, so
// this never aborts the benchmark.
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

std::string compiler_string() {
#if defined(__clang__)
    return std::format("Clang {}.{}.{}", __clang_major__, __clang_minor__, __clang_patchlevel__);
#elif defined(_MSC_VER)
    return std::format("MSVC {}", _MSC_VER);
#elif defined(__GNUC__)
    return std::format("GCC {}.{}.{}", __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__);
#else
    return "unknown";
#endif
}

constexpr std::string_view os_string() {
#if defined(_WIN32)
    return "Windows";
#elif defined(__linux__)
    return "Linux";
#elif defined(__APPLE__)
    return "macOS";
#else
    return "unknown";
#endif
}

std::string iso8601_timestamp_utc() {
    const auto now = std::chrono::time_point_cast<std::chrono::seconds>(std::chrono::system_clock::now());
    return std::format("{:%Y-%m-%dT%H:%M:%SZ}", now);
}

// Minimal, dependency-free JSON string escaping -- covers the printable
// ASCII this file ever feeds it (CPU brand strings, compiler names) plus
// control characters defensively, without pulling in a JSON library.
std::string json_escape(std::string_view s) {
    std::string out_str;
    out_str.reserve(s.size() + 8);
    for (const char c : s) {
        switch (c) {
            case '"':  out_str += "\\\""; break;
            case '\\': out_str += "\\\\"; break;
            case '\n': out_str += "\\n";  break;
            case '\t': out_str += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    out_str += std::format("\\u{:04x}", static_cast<unsigned char>(c));
                } else {
                    out_str += c;
                }
        }
    }
    return out_str;
}

struct PercentileReportNs {
    double min = 0, p50 = 0, p90 = 0, p99 = 0, p999 = 0, max = 0;
};

PercentileReportNs summarize_ns(std::vector<std::uint64_t>& samples_raw, double cycles_per_ns) {
    std::sort(samples_raw.begin(), samples_raw.end());
    const std::size_t n = samples_raw.size();

    auto at_percentile = [&](double p) -> std::uint64_t {
        std::size_t idx = static_cast<std::size_t>(p * static_cast<double>(n - 1));
        if (idx >= n) idx = n - 1;
        return samples_raw[idx];
    };

    auto to_ns = [&](std::uint64_t raw) { return static_cast<double>(raw) / cycles_per_ns; };

    PercentileReportNs r;
    r.min  = to_ns(samples_raw.front());
    r.p50  = to_ns(at_percentile(0.50));
    r.p90  = to_ns(at_percentile(0.90));
    r.p99  = to_ns(at_percentile(0.99));
    r.p999 = to_ns(at_percentile(0.999));
    r.max  = to_ns(samples_raw.back());
    return r;
}

TelemetryEvent make_synthetic_event(std::uint64_t sequence, std::uint32_t flags) noexcept {
    TelemetryEvent ev{};
    ev.sequence = sequence;
    ev.venue_ts_ns = sequence * 37; // deterministic synthetic payload, no clock call in hot loop
    ev.price_ticks = static_cast<std::int64_t>(100000 + (sequence % 4096));
    ev.quantity = static_cast<std::int64_t>(1 + (sequence % 500));
    ev.instrument_id = static_cast<std::uint32_t>(sequence % 256);
    ev.flags = flags;
    return ev;
}

} // namespace animus_bench

int main(int argc, char** argv) {
    using namespace animus_bench;
    using namespace std::chrono;

    constexpr std::uint64_t kWarmupEvents = 1'000'000;
    // 10,000,000 total measured events, split across two purpose-built phases
    // (see file header): a depth-1 latency phase, where the producer waits
    // for each event to be received before dispatching the next, so a
    // sample reflects actual transport cost rather than queueing backlog;
    // and an unthrottled throughput phase, which floods the ring at maximum
    // rate to find sustained capacity. Conflating the two into one flooded
    // run would report queueing delay under saturation as "dispatch
    // latency" -- technically a real number, but not the one an HFT client
    // asking "how fast is one event" wants.
    constexpr std::uint64_t kLatencyEvents = 1'000'000;
    constexpr std::uint64_t kThroughputEvents = 9'000'000;
    constexpr std::uint64_t kMeasuredEvents = kLatencyEvents + kThroughputEvents;
    constexpr std::size_t kRingCapacity = 16384; // power of two

    int producer_core = 0;
    int consumer_core = 1;
    std::string json_path = "telemetry_benchmarks.json";
    if (argc >= 3) {
        producer_core = std::atoi(argv[1]);
        consumer_core = std::atoi(argv[2]);
    }
    if (argc >= 4) {
        json_path = argv[3];
    }

    out("Animus Engine -- Telemetry Dispatch Benchmark (C++23)\n");
    out("=========================================================\n");
    out("Warm-up events:        {}\n", kWarmupEvents);
    out("Latency-phase events:  {} (depth-1, waits for receipt before next dispatch)\n", kLatencyEvents);
    out("Throughput-phase events: {} (unthrottled flood)\n", kThroughputEvents);
    out("Ring capacity:         {} slots ({} bytes/slot, {}-byte aligned)\n",
        kRingCapacity, sizeof(TelemetryEvent), kCacheLineSize);
    out("Requested affinity:    producer=core {}, consumer=core {}\n", producer_core, consumer_core);
    out("Clock source:          {}\n\n", clock_source_name());

    // Static storage duration -- BSS, not the heap, not the stack. This is
    // the entire backing store for the ring; nothing else allocates for the
    // life of the benchmark except the one reserve() below.
    static SpscRingBuffer<TelemetryEvent, kRingCapacity> dispatch_ring;

    std::vector<std::uint64_t> latency_samples_raw;
    latency_samples_raw.reserve(kLatencyEvents); // sole heap allocation; happens before timing starts

    std::atomic<bool> consumer_ready{false};
    std::atomic<std::uint64_t> latency_ack{0}; // count of latency-phase events the consumer has received
    steady_clock::time_point wall_start{};
    steady_clock::time_point wall_end{};

    std::thread consumer([&]() {
        pin_current_thread_to_core(consumer_core);
        consumer_ready.store(true, std::memory_order_release);

        TelemetryEvent event;
        std::uint64_t throughput_received = 0;
        while (throughput_received < kThroughputEvents) {
            if (!dispatch_ring.pop(event)) continue; // busy-spin: ring momentarily empty
            const std::uint64_t recv = sample_clock();
            if (event.flags & kFlagMeasured) {
                latency_samples_raw.push_back(recv - event.dispatch_tsc); // reserved -- no reallocation
                latency_ack.fetch_add(1, std::memory_order_release); // unblocks producer for the next dispatch
            } else if (event.flags & kFlagThroughput) {
                ++throughput_received;
                if (throughput_received == kThroughputEvents) {
                    wall_end = steady_clock::now();
                }
            }
            // warm-up events carry flags == 0 and are just drained.
        }
    });

    pin_current_thread_to_core(producer_core);
    while (!consumer_ready.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    const double cycles_per_ns = calibrate_cycles_per_ns();
    out("Calibration:            {:.4f} raw-units/ns (~{:.3f} GHz-equivalent)\n\n", cycles_per_ns, cycles_per_ns);

    // Warm-up: identical push/pop path, unmeasured, so caches and branch
    // predictors are hot before the timed region begins.
    for (std::uint64_t i = 0; i < kWarmupEvents; ++i) {
        TelemetryEvent ev = make_synthetic_event(i, /*flags=*/0);
        ev.dispatch_tsc = sample_clock();
        while (!dispatch_ring.push(ev)) {
            // ring momentarily full -- retry; consumer is draining continuously.
        }
    }

    // Latency phase: depth bounded to 1 in-flight event -- the producer
    // waits for the consumer's ack before dispatching the next one, so each
    // sample is the actual enqueue-to-receipt transport cost, not queueing
    // delay behind a backlog of not-yet-drained events.
    for (std::uint64_t i = 0; i < kLatencyEvents; ++i) {
        TelemetryEvent ev = make_synthetic_event(i, kFlagMeasured);
        ev.dispatch_tsc = sample_clock();
        while (!dispatch_ring.push(ev)) {
            // ring should never be full here (depth 1) -- retry defensively anyway.
        }
        while (latency_ack.load(std::memory_order_acquire) <= i) {
            // busy-spin for this event's receipt before dispatching the next.
        }
    }

    // Throughput phase: unthrottled flood at maximum sustained rate.
    const steady_clock::time_point throughput_wall_start = steady_clock::now();
    wall_start = throughput_wall_start;
    for (std::uint64_t i = 0; i < kThroughputEvents; ++i) {
        TelemetryEvent ev = make_synthetic_event(i, kFlagThroughput);
        while (!dispatch_ring.push(ev)) {
            // backpressure retry -- included in the measured wall-clock
            // throughput window, since a real consumer stall would be too.
        }
    }

    consumer.join(); // establishes happens-before for wall_end and latency_samples_raw

    const PercentileReportNs report = summarize_ns(latency_samples_raw, cycles_per_ns);
    const double wall_seconds = duration<double>(wall_end - wall_start).count();
    const double events_per_sec = static_cast<double>(kThroughputEvents) / wall_seconds;
    const double million_events_per_sec = events_per_sec / 1'000'000.0;

    out("\nEnd-to-end dispatch latency (producer enqueue -> consumer receipt):\n");
    out("  ---------------------------------------------\n");
    out("  {:<8} {:>12.1f} ns\n", "min", report.min);
    out("  {:<8} {:>12.1f} ns\n", "p50", report.p50);
    out("  {:<8} {:>12.1f} ns\n", "p90", report.p90);
    out("  {:<8} {:>12.1f} ns\n", "p99", report.p99);
    out("  {:<8} {:>12.1f} ns\n", "p99.9", report.p999);
    out("  {:<8} {:>12.1f} ns\n", "max", report.max);
    out("  ---------------------------------------------\n");
    out("Note: max/p99.9 reflect OS scheduling noise (context switches, timer\n");
    out("interrupts) on the pinned cores as much as the transport itself -- p50/p90/p99\n");
    out("are the steady-state figures; run on an isolated/tickless core set for a\n");
    out("tighter tail.\n\n");
    out("Sustained throughput:\n");
    out("  ---------------------------------------------\n");
    out("  {:<20} {:>12.3f} M msgs/sec\n", "throughput", million_events_per_sec);
    out("  {:<20} {:>12.3f} s\n", "wall time", wall_seconds);
    out("  ---------------------------------------------\n");

    const std::string cpu_brand = get_cpu_brand();
    const unsigned logical_cores = std::thread::hardware_concurrency();
    const std::string timestamp = iso8601_timestamp_utc();
    const std::string compiler = compiler_string();

    std::ofstream json_out(json_path, std::ios::trunc);
    if (!json_out.is_open()) {
        std::fprintf(stderr, "warning: could not open '%s' for writing -- skipping JSON export\n", json_path.c_str());
        return 0;
    }

    json_out << "{\n"
             << "  \"schema_version\": \"1.0\",\n"
             << "  \"benchmark\": \"animus_telemetry_dispatch\",\n"
             << "  \"generated_at_utc\": \"" << json_escape(timestamp) << "\",\n"
             << "  \"methodology\": \"spsc_lock_free_ring_one_way_dispatch_latency\",\n"
             << "  \"parameters\": {\n"
             << "    \"warmup_events\": " << kWarmupEvents << ",\n"
             << "    \"latency_phase_events\": " << kLatencyEvents << ",\n"
             << "    \"throughput_phase_events\": " << kThroughputEvents << ",\n"
             << "    \"total_measured_events\": " << kMeasuredEvents << ",\n"
             << "    \"latency_phase_in_flight_depth\": 1,\n"
             << "    \"ring_capacity_slots\": " << kRingCapacity << ",\n"
             << "    \"event_size_bytes\": " << sizeof(TelemetryEvent) << ",\n"
             << "    \"cache_line_bytes\": " << kCacheLineSize << ",\n"
             << "    \"producer_core\": " << producer_core << ",\n"
             << "    \"consumer_core\": " << consumer_core << ",\n"
             << "    \"hot_loop_heap_allocations\": 0\n"
             << "  },\n"
             << "  \"latency_ns\": {\n"
             << "    \"min\": " << report.min << ",\n"
             << "    \"p50\": " << report.p50 << ",\n"
             << "    \"p90\": " << report.p90 << ",\n"
             << "    \"p99\": " << report.p99 << ",\n"
             << "    \"p99_9\": " << report.p999 << ",\n"
             << "    \"max\": " << report.max << "\n"
             << "  },\n"
             << "  \"throughput\": {\n"
             << "    \"events_per_sec\": " << static_cast<std::uint64_t>(events_per_sec) << ",\n"
             << "    \"million_events_per_sec\": " << million_events_per_sec << ",\n"
             << "    \"wall_time_seconds\": " << wall_seconds << "\n"
             << "  },\n"
             << "  \"system\": {\n"
             << "    \"os\": \"" << json_escape(os_string()) << "\",\n"
             << "    \"compiler\": \"" << json_escape(compiler) << "\",\n"
             << "    \"cpp_standard\": \"C++23\",\n"
             << "    \"cpu_brand\": \"" << json_escape(cpu_brand) << "\",\n"
             << "    \"logical_cores\": " << logical_cores << ",\n"
             << "    \"clock_source\": \"" << json_escape(clock_source_name()) << "\",\n"
             << "    \"calibration_units_per_ns\": " << cycles_per_ns << "\n"
             << "  }\n"
             << "}\n";
    json_out.close();

    out("\nJSON report written to: {}\n", json_path);
    return 0;
}
