// Animus Engine -- External Evaluation Harness: fast-path latency of every
// public shared-memory ring primitive.
//
// What is timed. For each primitive, ONE call to the operation a producer makes
// on its hot path -- try_push() / publish() / broadcast() -- bracketed by two
// serialized counter reads (include/animus/cycle_timer.hpp: lfence;rdtsc;lfence
// on x86, isb;mrs cntvct_el0;isb on AArch64). The ring is created and mapped
// once, outside every timed loop; the code under measurement is only the
// produce-side atomic bookkeeping plus the fixed-size slot copy.
//
//   shm_ring_spsc    ShmRing<ExecutionEvent>::try_push()        legacy SPSC, wire-schema ring
//   spsc_queue       SpscQueue<ExecutionEvent>::try_push()      lossless 1 -> 1 (execution path)
//   broadcast_ring   BroadcastRing<ExecutionEvent>::publish()   lossy 1 -> N (telemetry path)
//   spmc_ring        SpmcRing<ExecutionEvent>::broadcast()      legacy broadcast, seqlock-guarded
//   shm_ring_mpmc    ShmRingMpmc<ExecutionEvent>::try_push()    lossless N -> M (one CAS per claim)
//
// Methodology (unchanged in spirit from the 1M-iteration revision, scaled up):
//   - 500,000 warm-up calls through the identical timed code path, then
//     10,000,000 timed calls, per primitive and mode. Defaults; see --iters.
//   - Percentiles, never means: min / P50 / P90 / P99 / P99.9 / max, over the
//     sorted raw samples (index = floor(p * (n-1))). Raw counter ticks are the
//     primary unit; nanoseconds are derived with a ticks-per-ns ratio measured
//     at start-up against std::chrono::steady_clock, never assumed.
//   - The timer's own cost is measured too (back-to-back reads of an empty
//     region) and reported as the FLOOR. Every figure above it includes that
//     floor -- on x86 two lfences and an rdtsc are most of a 36-40 cycle
//     minimum -- so read the floor before reading the percentiles.
//   - Two modes. `stream` (default; the original methodology): no consumer, a
//     lossless ring sized so it can never fill, so every push writes a cache
//     line the core has not touched -- pessimistic by construction. `drained`:
//     a small, cache-resident ring that the harness empties between timed
//     batches (the pops are untimed), i.e. a consumer that keeps up. Lossy
//     rings never fill, so they are reported in `stream` only.
//
// Zero-allocation proof (tests/alloc_guard.hpp), three independent layers:
//   1. Counting global operator new/delete (every overload) and, when the build
//      wraps it (GNU ld --wrap), malloc/calloc/realloc/free. Both counters are
//      snapshotted before the warm-up and read back after the timed loop.
//   2. Armed assertion: for the whole warm-up + timed window the allocator is
//      ARMED -- any allocation or deallocation from any thread aborts the
//      process at the offending call. A nonzero delta cannot be reported
//      because the run would not have survived to print it.
//   3. Negative controls, run first: a deliberate new / malloc must move the
//      counters, proving the instrumentation is live rather than silently
//      reading zero. (scripts/audit_hotpath_symbols.py adds a fourth, static
//      layer: no allocator call appears in the disassembled hot path.)
// The sample buffer and ring are allocated strictly before the armed window.
//
// Output: a human-readable report on stdout; --json PATH writes the same
// numbers machine-readably (schema "animus.eval_harness.v2"), which is what
// scripts/generate_verification_report.py consumes.
//
// Build (Linux):    g++ -std=c++17 -O3 -march=native -DNDEBUG -pthread -I include
//                       tests/eval_harness.cpp -o eval_harness -lrt
// Build (MinGW):    g++ -std=c++17 -O3 -march=native -DNDEBUG -static -I include
//                       tests/eval_harness.cpp -o eval_harness.exe
// Build (MSVC):     cl /std:c++17 /O2 /EHsc /wd4324 /Iinclude tests\eval_harness.cpp
//         The old -lws2_32 -lbcrypt -liphlpapi flags were never needed here:
//         nothing this file includes touches Winsock, CNG or the IP helper.
//         MinGW needs -static (or -static-libstdc++ -static-libgcc) for THIS file: it
//         replaces the global operator new/delete, and a dynamically loaded
//         libstdc++-6.dll allocates from its own copy of the allocator. If a
//         different MinGW flavour's libstdc++-6.dll wins the DLL search (an msvcrt
//         build ahead of a UCRT one on PATH), the two disagree about which CRT heap
//         owns a block and the process dies with STATUS_HEAP_CORRUPTION. Linking
//         the runtime statically removes the ambiguity and makes the replacement
//         cover the library's own allocations too.
//         For the malloc-level counter on GNU ld, also pass -DANIMUS_WRAP_MALLOC=1 and
//         -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free (plus
//         --wrap=posix_memalign,--wrap=aligned_alloc on POSIX, or
//         --wrap=_aligned_malloc,--wrap=_aligned_free on Windows). The Makefile
//         and CMake do this.
// Usage:  eval_harness [--iters N] [--warmup N] [--target all|name[,name...]]
//                      [--mode stream|drained|both] [--pin CORE] [--json PATH]
//                      [--floor-samples N]
#include "../include/animus/telemetry.hpp"
#include "../include/animus/shm_ipc.hpp"
#include "../include/animus/broadcast_ring.hpp"
#include "../include/animus/spsc_queue.hpp"
#include "../include/animus/execution_event.hpp"
#include "../include/animus/thread_affinity.hpp"
#include "../include/animus/cycle_timer.hpp"
#include "alloc_guard.hpp"
#include "report_util.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

// ===== Compile-time layout checks (the hot-path structures, not proxies) =====
static_assert(animus::cache_line_size >= 64,
    "ANIMUS_CACHE_LINE_SIZE must be at least 64 bytes on any supported target");
static_assert(alignof(animus::sys::ipc::RingHeader) >= 64, "RingHeader must be cache-line aligned");
static_assert(alignof(animus::sys::ipc::SpmcRingHeader) >= 64, "SpmcRingHeader must be cache-line aligned");
static_assert(alignof(animus::sys::ipc::MpmcRingHeader) >= 64, "MpmcRingHeader must be cache-line aligned");
static_assert(alignof(animus::sys::ipc::BroadcastHeader) >= 64, "BroadcastHeader must be cache-line aligned");
static_assert(alignof(animus::sys::ipc::SpscQueueHeader) >= 64, "SpscQueueHeader must be cache-line aligned");
static_assert(offsetof(animus::sys::ipc::MpmcRingHeader, enqueue_pos) % 64 == 0 &&
              offsetof(animus::sys::ipc::MpmcRingHeader, dequeue_pos) % 64 == 0,
    "the MPMC read and write indices must each start on their own 64-byte boundary");
static_assert(offsetof(animus::sys::ipc::MpmcRingHeader, dequeue_pos) -
              offsetof(animus::sys::ipc::MpmcRingHeader, enqueue_pos) >= 64,
    "the MPMC read and write indices must not share a cache line");
static_assert(offsetof(animus::sys::ipc::SpscQueueHeader, tail) - offsetof(animus::sys::ipc::SpscQueueHeader, head) >= 64,
    "SpscQueue head and tail must not share a cache line");
static_assert(alignof(animus::CacheAligned<animus::TelemetrySnapshot>) >= 64, "CacheAligned<T> must be cache-line aligned");
static_assert(alignof(animus::CacheLinePad) >= 64, "CacheLinePad must be cache-line aligned");

// ===== Compile-time hot-path contract: noexcept + trivially copyable payload =====
using animus::ExecutionEvent;
using animus::sys::ipc::BroadcastRing;
using animus::sys::ipc::SharedMemoryRegion;
using animus::sys::ipc::ShmRing;
using animus::sys::ipc::ShmRingMpmc;
using animus::sys::ipc::SpmcRing;
using animus::sys::ipc::SpscQueue;
namespace timing = animus::timing;

static_assert(std::is_trivially_copyable<ExecutionEvent>::value, "the payload must be a plain memcpy-able value");
#define ANIMUS_ASSERT_NOEXCEPT_OP(expr) static_assert(noexcept(expr), "hot-path operation must be noexcept: " #expr)
ANIMUS_ASSERT_NOEXCEPT_OP(std::declval<ShmRing<ExecutionEvent>&>().try_push(std::declval<const ExecutionEvent&>()));
ANIMUS_ASSERT_NOEXCEPT_OP(std::declval<SpscQueue<ExecutionEvent>&>().try_push(std::declval<const ExecutionEvent&>()));
ANIMUS_ASSERT_NOEXCEPT_OP(std::declval<BroadcastRing<ExecutionEvent>&>().publish(std::declval<const ExecutionEvent&>()));
ANIMUS_ASSERT_NOEXCEPT_OP(std::declval<SpmcRing<ExecutionEvent>&>().broadcast(std::declval<const ExecutionEvent&>()));
ANIMUS_ASSERT_NOEXCEPT_OP(std::declval<ShmRingMpmc<ExecutionEvent>&>().try_push(std::declval<const ExecutionEvent&>()));
ANIMUS_ASSERT_NOEXCEPT_OP(std::declval<ShmRingMpmc<ExecutionEvent>&>().try_pop(std::declval<ExecutionEvent&>()));

namespace {

// Forces the compiler to materialise `v` in memory before the first timestamp
// instead of sinking its construction into the timed region: once its address
// has escaped, the stores that build it must happen before the next barrier.
// (Defined in alloc_guard.hpp, where the negative controls use it too.)
using animus_test::escape;

// Keeps the warm-up loop's accumulated cycle counts observable so it cannot be elided.
volatile uint64_t g_warmup_sink = 0;

struct Options {
    uint64_t iters = 10'000'000;
    uint64_t warmup = 500'000;
    uint64_t floor_samples = 1'000'000;
    std::string target = "all";
    std::string mode = "stream";
    std::string json_path;
    int pin_core = -1;
};

enum class Mode { Stream, Drained };
const char* mode_name(Mode m) { return m == Mode::Stream ? "stream" : "drained"; }

struct Stats {
    uint64_t min = 0, p50 = 0, p90 = 0, p99 = 0, p999 = 0, max = 0;
};

Stats summarize(std::vector<uint64_t>& samples, uint64_t n) {
    Stats s;
    if (n == 0) return s;
    std::sort(samples.begin(), samples.begin() + static_cast<std::ptrdiff_t>(n));
    const auto at = [&](double p) { return samples[static_cast<size_t>(p * static_cast<double>(n - 1))]; };
    s.min = samples[0];
    s.p50 = at(0.50);
    s.p90 = at(0.90);
    s.p99 = at(0.99);
    s.p999 = at(0.999);
    s.max = samples[n - 1];
    return s;
}

struct Result {
    std::string primitive;
    std::string operation;
    Mode mode = Mode::Stream;
    bool ok = false;
    std::string error;
    uint64_t samples = 0;
    uint64_t push_failures = 0;
    uint64_t ring_capacity = 0;
    uint64_t ring_bytes = 0;
    Stats stats;
    animus_test::AllocCounters allocs; // delta across the whole armed window (warm-up + timed)
};

uint64_t round_up_pow2(uint64_t v) {
    uint64_t p = 1;
    while (p < v) p <<= 1;
    return p;
}

std::string unique_name(const char* tag) {
    return std::string("animus_eval_") + tag + "_" +
           std::to_string(animus::sys::lifecycle::current_process_id());
}

constexpr uint64_t kDrainBatch = 1024;
constexpr uint64_t kDrainedCapacity = 8192;
constexpr uint64_t kLossyCapacity = uint64_t{ 1 } << 20;

// Runs warm-up + timed loops for one operation. `op(ev)` performs the call
// under test and returns whether it was accepted; `drain(k)` (drained mode)
// consumes up to k records, untimed. Samples go into the caller's pre-sized
// buffer by index -- nothing in here allocates.
template <typename Op, typename Drain>
bool run_window(const Options& opt, Mode mode, std::vector<uint64_t>& samples, Result& r, Op&& op, Drain&& drain) {
    const bool drained = (mode == Mode::Drained);
    uint64_t failures = 0;
    uint64_t sink = 0;

    const auto before = animus_test::snapshot();
    animus_test::arm();

    // Warm-up: the identical timed code path, caches and branch predictors hot.
    for (uint64_t i = 0; i < opt.warmup; ++i) {
        ExecutionEvent ev{};
        ev.sequence = i;
        ev.dispatch_ts_raw = i;
        ev.instrument_id = 1;
        escape(ev);
        const uint64_t t0 = timing::read_cycles_serialized();
        const bool ok = op(ev);
        const uint64_t t1 = timing::read_cycles_serialized();
        sink += t1 - t0;
        if (!ok) ++failures;
        if (drained && (i + 1) % kDrainBatch == 0) drain(kDrainBatch);
    }
    if (drained) drain(kDrainBatch);

    for (uint64_t i = 0; i < opt.iters; ++i) {
        ExecutionEvent ev{};
        ev.sequence = opt.warmup + i;
        ev.dispatch_ts_raw = opt.warmup + i;
        ev.instrument_id = 1;
        escape(ev);
        const uint64_t t0 = timing::read_cycles_serialized();
        const bool ok = op(ev);
        const uint64_t t1 = timing::read_cycles_serialized();
        samples[i] = t1 - t0;
        if (!ok) ++failures;
        if (drained && (i + 1) % kDrainBatch == 0) drain(kDrainBatch);
    }

    animus_test::disarm();
    r.allocs = animus_test::delta(before, animus_test::snapshot());
    r.push_failures = failures;
    r.samples = opt.iters;
    g_warmup_sink = sink;
    return failures == 0;
}

// ---------------------------------------------------------------------------
// One function per primitive. Each creates its ring, runs the window, tears down.
// ---------------------------------------------------------------------------
uint64_t lossless_capacity(const Options& opt, Mode mode) {
    return mode == Mode::Drained ? kDrainedCapacity : round_up_pow2(opt.warmup + opt.iters);
}

Result measure_shm_ring_spsc(const Options& opt, Mode mode, std::vector<uint64_t>& samples) {
    Result r;
    r.primitive = "shm_ring_spsc";
    r.operation = "ShmRing<ExecutionEvent>::try_push()";
    r.mode = mode;
    const std::string name = unique_name("shm_ring_spsc");
    const uint64_t cap = lossless_capacity(opt, mode);
    ShmRing<ExecutionEvent>::unlink(name.c_str()); // clear a stale segment from a crashed earlier run
    auto ring = ShmRing<ExecutionEvent>::create(name.c_str(), cap);
    if (!ring) { r.error = "ShmRing::create failed (stale segment, or /dev/shm too small for the stream-mode ring?)"; return r; }
    ring->mark_producer_attached();
    ring->mark_consumer_attached();
    r.ring_capacity = ring->capacity();
    r.ring_bytes = sizeof(animus::sys::ipc::RingHeader) + r.ring_capacity * sizeof(ExecutionEvent);
    ExecutionEvent out{};
    r.ok = run_window(opt, mode, samples, r,
        [&](const ExecutionEvent& ev) noexcept { return ring->try_push(ev); },
        [&](uint64_t k) noexcept { for (uint64_t i = 0; i < k; ++i) { if (!ring->try_pop(out)) break; } });
    ring.reset();
    ShmRing<ExecutionEvent>::unlink(name.c_str());
    return r;
}

Result measure_spsc_queue(const Options& opt, Mode mode, std::vector<uint64_t>& samples) {
    Result r;
    r.primitive = "spsc_queue";
    r.operation = "SpscQueue<ExecutionEvent>::try_push()";
    r.mode = mode;
    const std::string name = unique_name("spsc_queue");
    const uint64_t cap = lossless_capacity(opt, mode);
    const size_t bytes = SpscQueue<ExecutionEvent>::required_bytes(static_cast<size_t>(cap));
    SharedMemoryRegion::unlink(name.c_str());
    SharedMemoryRegion region;
    if (!SharedMemoryRegion::create(name.c_str(), bytes, region)) { r.error = "shared-memory create failed"; return r; }
    SpscQueue<ExecutionEvent> producer, consumer;
    if (!SpscQueue<ExecutionEvent>::init(region.data(), region.size(), static_cast<size_t>(cap), producer) ||
        !SpscQueue<ExecutionEvent>::attach(region.data(), region.size(), consumer)) {
        r.error = "SpscQueue init/attach failed";
        SharedMemoryRegion::unlink(name.c_str());
        return r;
    }
    r.ring_capacity = cap;
    r.ring_bytes = bytes;
    ExecutionEvent out{};
    r.ok = run_window(opt, mode, samples, r,
        [&](const ExecutionEvent& ev) noexcept { return producer.try_push(ev); },
        [&](uint64_t k) noexcept { for (uint64_t i = 0; i < k; ++i) { if (!consumer.try_pop(out)) break; } });
    region.close();
    SharedMemoryRegion::unlink(name.c_str());
    return r;
}

Result measure_broadcast_ring(const Options& opt, Mode mode, std::vector<uint64_t>& samples) {
    Result r;
    r.primitive = "broadcast_ring";
    r.operation = "BroadcastRing<ExecutionEvent>::publish()";
    r.mode = mode;
    const std::string name = unique_name("broadcast_ring");
    const size_t bytes = BroadcastRing<ExecutionEvent>::required_bytes(static_cast<size_t>(kLossyCapacity));
    SharedMemoryRegion::unlink(name.c_str());
    SharedMemoryRegion region;
    if (!SharedMemoryRegion::create(name.c_str(), bytes, region)) { r.error = "shared-memory create failed"; return r; }
    BroadcastRing<ExecutionEvent> writer;
    if (!BroadcastRing<ExecutionEvent>::init(region.data(), region.size(), static_cast<size_t>(kLossyCapacity), writer)) {
        r.error = "BroadcastRing init failed";
        SharedMemoryRegion::unlink(name.c_str());
        return r;
    }
    r.ring_capacity = kLossyCapacity;
    r.ring_bytes = bytes;
    r.ok = run_window(opt, mode, samples, r,
        [&](const ExecutionEvent& ev) noexcept { writer.publish(ev); return true; },
        [](uint64_t) noexcept {});
    region.close();
    SharedMemoryRegion::unlink(name.c_str());
    return r;
}

Result measure_spmc_ring(const Options& opt, Mode mode, std::vector<uint64_t>& samples) {
    Result r;
    r.primitive = "spmc_ring";
    r.operation = "SpmcRing<ExecutionEvent>::broadcast()";
    r.mode = mode;
    const std::string name = unique_name("spmc_ring");
    SpmcRing<ExecutionEvent>::unlink(name.c_str());
    auto ring = SpmcRing<ExecutionEvent>::create(name.c_str(), kLossyCapacity);
    if (!ring) { r.error = "SpmcRing::create failed"; return r; }
    ring->mark_producer_attached();
    r.ring_capacity = ring->capacity();
    r.ring_bytes = sizeof(animus::sys::ipc::SpmcRingHeader) + r.ring_capacity * sizeof(ExecutionEvent);
    r.ok = run_window(opt, mode, samples, r,
        [&](const ExecutionEvent& ev) noexcept { ring->broadcast(ev); return true; },
        [](uint64_t) noexcept {});
    ring.reset();
    SpmcRing<ExecutionEvent>::unlink(name.c_str());
    return r;
}

Result measure_shm_ring_mpmc(const Options& opt, Mode mode, std::vector<uint64_t>& samples) {
    Result r;
    r.primitive = "shm_ring_mpmc";
    r.operation = "ShmRingMpmc<ExecutionEvent>::try_push()";
    r.mode = mode;
    const std::string name = unique_name("shm_ring_mpmc");
    const uint64_t cap = lossless_capacity(opt, mode);
    ShmRingMpmc<ExecutionEvent>::unlink(name.c_str());
    auto ring = ShmRingMpmc<ExecutionEvent>::create(name.c_str(), static_cast<size_t>(cap));
    if (!ring) { r.error = "ShmRingMpmc::create failed (stale segment, or /dev/shm too small for the stream-mode ring?)"; return r; }
    r.ring_capacity = ring->capacity();
    r.ring_bytes = sizeof(animus::sys::ipc::MpmcRingHeader) + r.ring_capacity * ring->stride();
    ExecutionEvent out{};
    r.ok = run_window(opt, mode, samples, r,
        [&](const ExecutionEvent& ev) noexcept { return ring->try_push(ev); },
        [&](uint64_t k) noexcept { for (uint64_t i = 0; i < k; ++i) { if (!ring->try_pop(out)) break; } });
    ring.reset();
    ShmRingMpmc<ExecutionEvent>::unlink(name.c_str());
    return r;
}

struct Target {
    const char* name;
    Result (*measure)(const Options&, Mode, std::vector<uint64_t>&);
    bool lossless; // false: the writer never waits and never fills, so `drained` adds nothing
};
const Target kTargets[] = {
    { "shm_ring_spsc", measure_shm_ring_spsc, true },
    { "spsc_queue", measure_spsc_queue, true },
    { "broadcast_ring", measure_broadcast_ring, false },
    { "spmc_ring", measure_spmc_ring, false },
    { "shm_ring_mpmc", measure_shm_ring_mpmc, true },
};

bool selected(const std::string& list, const char* name) {
    if (list == "all") return true;
    size_t pos = 0;
    while (pos <= list.size()) {
        const size_t comma = list.find(',', pos);
        const std::string tok = list.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        if (tok == name) return true;
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------
using animus_test::os_name;
using animus_test::arch_name;
using animus_test::compiler_string;
using animus_test::json_str;
using animus_test::thousands;
using animus_test::u;

void print_result(const Result& r, const timing::Calibration& cal, const char* unit, uint64_t warmup) {
    std::printf("%s  [mode: %s]\n", r.operation.c_str(), mode_name(r.mode));
    if (!r.error.empty()) { std::printf("  ERROR: %s\n\n", r.error.c_str()); return; }
    std::printf("  ring: %s slots, %s bytes (%.1f MiB)   samples: %s after %s warm-up   push failures: %s\n",
        thousands(r.ring_capacity).c_str(), thousands(r.ring_bytes).c_str(),
        static_cast<double>(r.ring_bytes) / (1024.0 * 1024.0), thousands(r.samples).c_str(),
        thousands(warmup).c_str(), thousands(r.push_failures).c_str());
    std::printf("  ---------------------------------------------\n");
    const struct { const char* label; uint64_t v; } rows[] = {
        { "min", r.stats.min }, { "P50", r.stats.p50 }, { "P90", r.stats.p90 },
        { "P99", r.stats.p99 }, { "P99.9", r.stats.p999 }, { "max", r.stats.max } };
    for (const auto& row : rows) {
        std::printf("  %-6s %12s %s   %10.1f ns\n", row.label, thousands(row.v).c_str(), unit, cal.ns(row.v));
    }
    std::printf("  ---------------------------------------------\n");
    std::printf("  heap activity, warm-up + timed window (armed): operator new %s | operator delete %s | malloc %s | free %s\n",
        thousands(r.allocs.new_calls).c_str(), thousands(r.allocs.delete_calls).c_str(),
        animus_test::malloc_instrumented() ? thousands(r.allocs.malloc_calls).c_str() : "n/a",
        animus_test::malloc_instrumented() ? thousands(r.allocs.free_calls).c_str() : "n/a");
    std::printf("  Result: %s\n\n", (r.ok && r.allocs.total() == 0) ? "PASS -- zero heap allocations" : "FAIL");
}

void write_json(const Options& opt, const timing::Calibration& cal, const Stats& floor_stats, uint64_t floor_samples,
                const std::vector<Result>& results, bool pass, bool counters_live) {
    FILE* f = animus_test::open_for_write(opt.json_path);
    if (!f) { std::fprintf(stderr, "warning: cannot write %s\n", opt.json_path.c_str()); return; }
    const char* unit = timing::tick_unit_name(timing::kCounterKind);
    std::fprintf(f, "{\n  \"schema\": \"animus.eval_harness.v2\",\n");
    std::fprintf(f, "  \"platform\": {\"os\": "); json_str(f, os_name());
    std::fprintf(f, ", \"arch\": "); json_str(f, arch_name());
    std::fprintf(f, ", \"compiler\": "); json_str(f, compiler_string());
    std::fprintf(f, ", \"cache_line_bytes\": %zu, \"hardware_threads\": %u, \"pinned_core\": %d},\n",
        static_cast<size_t>(animus::cache_line_size), std::thread::hardware_concurrency(), opt.pin_core);
    std::fprintf(f, "  \"timing\": {\"counter\": "); json_str(f, timing::counter_kind_name(timing::kCounterKind));
    std::fprintf(f, ", \"unit\": "); json_str(f, unit);
    std::fprintf(f, ", \"invariant\": %s, \"ticks_per_ns\": %.6f, \"ticks_per_ns_min\": %.6f, \"ticks_per_ns_max\": %.6f, "
                    "\"spread_ppm\": %.1f, \"trials\": %u, \"resolution_ns\": %.4f, \"arm_cntfrq_hz\": %llu,\n",
        timing::counter_is_invariant() ? "true" : "false", cal.ticks_per_ns, cal.min_ticks_per_ns, cal.max_ticks_per_ns,
        cal.spread_ppm, cal.trials, cal.resolution_ns(), u(cal.arm_cntfrq_hz));
    std::fprintf(f, "    \"floor\": {\"samples\": %llu, \"min\": %llu, \"p50\": %llu, \"p90\": %llu, \"p99\": %llu, \"p999\": %llu, \"max\": %llu}},\n",
        u(floor_samples), u(floor_stats.min), u(floor_stats.p50), u(floor_stats.p90), u(floor_stats.p99), u(floor_stats.p999), u(floor_stats.max));
    std::fprintf(f, "  \"config\": {\"iterations\": %llu, \"warmup\": %llu, \"mode_requested\": ", u(opt.iters), u(opt.warmup));
    json_str(f, opt.mode);
    std::fprintf(f, ", \"malloc_instrumented\": %s, \"allocation_counters_live\": %s},\n",
        animus_test::malloc_instrumented() ? "true" : "false", counters_live ? "true" : "false");
    std::fprintf(f, "  \"results\": [\n");
    for (size_t i = 0; i < results.size(); ++i) {
        const Result& r = results[i];
        std::fprintf(f, "    {\"primitive\": "); json_str(f, r.primitive);
        std::fprintf(f, ", \"operation\": "); json_str(f, r.operation);
        std::fprintf(f, ", \"mode\": "); json_str(f, mode_name(r.mode));
        std::fprintf(f, ", \"ok\": %s, \"error\": ", r.ok ? "true" : "false"); json_str(f, r.error);
        std::fprintf(f, ", \"samples\": %llu, \"push_failures\": %llu, \"ring_capacity\": %llu, \"ring_bytes\": %llu,\n",
            u(r.samples), u(r.push_failures), u(r.ring_capacity), u(r.ring_bytes));
        std::fprintf(f, "     \"ticks\": {\"min\": %llu, \"p50\": %llu, \"p90\": %llu, \"p99\": %llu, \"p999\": %llu, \"max\": %llu},\n",
            u(r.stats.min), u(r.stats.p50), u(r.stats.p90), u(r.stats.p99), u(r.stats.p999), u(r.stats.max));
        std::fprintf(f, "     \"ns\": {\"min\": %.2f, \"p50\": %.2f, \"p90\": %.2f, \"p99\": %.2f, \"p999\": %.2f, \"max\": %.2f},\n",
            cal.ns(r.stats.min), cal.ns(r.stats.p50), cal.ns(r.stats.p90), cal.ns(r.stats.p99), cal.ns(r.stats.p999), cal.ns(r.stats.max));
        std::fprintf(f, "     \"allocations\": {\"operator_new\": %llu, \"operator_delete\": %llu, \"malloc\": %llu, \"free\": %llu}, \"zero_alloc\": %s}%s\n",
            u(r.allocs.new_calls), u(r.allocs.delete_calls), u(r.allocs.malloc_calls), u(r.allocs.free_calls),
            (r.ok && r.allocs.total() == 0) ? "true" : "false", i + 1 < results.size() ? "," : "");
    }
    std::fprintf(f, "  ],\n  \"verdict\": \"%s\"\n}\n", pass ? "PASS" : "FAIL");
    std::fclose(f);
}

// Negative controls: prove the counters move when something really allocates.
bool counters_are_live() {
    bool ok = true;
    const auto a0 = animus_test::snapshot();
    int* p = new int(7);
    escape(p);
    const auto a1 = animus_test::snapshot();
    ok = ok && (a1.new_calls == a0.new_calls + 1);
    delete p;
    ok = ok && (animus_test::snapshot().delete_calls == a1.delete_calls + 1);
#if defined(ANIMUS_WRAP_MALLOC)
    const auto m0 = animus_test::snapshot();
    void* q = std::malloc(64);
    escape(q);
    const auto m1 = animus_test::snapshot();
    ok = ok && (m1.malloc_calls == m0.malloc_calls + 1);
    std::free(q);
    ok = ok && (animus_test::snapshot().free_calls == m1.free_calls + 1);
#endif
    return ok;
}

using animus_test::parse_u64;

} // namespace

int main(int argc, char** argv) {
    // Unbuffered: a 10M-iteration run takes a while per primitive, and progress
    // (and any crash diagnostics) must be visible when stdout is a file or a pipe.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool has_val = i + 1 < argc;
        uint64_t v = 0;
        if (a == "--iters" && has_val && parse_u64(argv[++i], v)) opt.iters = v;
        else if (a == "--warmup" && has_val && parse_u64(argv[++i], v)) opt.warmup = v;
        else if (a == "--floor-samples" && has_val && parse_u64(argv[++i], v)) opt.floor_samples = v;
        else if (a == "--target" && has_val) opt.target = argv[++i];
        else if (a == "--mode" && has_val) opt.mode = argv[++i];
        else if (a == "--json" && has_val) opt.json_path = argv[++i];
        else if (a == "--pin" && has_val && parse_u64(argv[++i], v)) opt.pin_core = static_cast<int>(v);
        else {
            std::fprintf(stderr,
                "usage: %s [--iters N] [--warmup N] [--target all|shm_ring_spsc,spsc_queue,broadcast_ring,spmc_ring,shm_ring_mpmc]\n"
                "          [--mode stream|drained|both] [--pin CORE] [--json PATH] [--floor-samples N]\n", argv[0]);
            return 2;
        }
    }
    if (opt.mode != "stream" && opt.mode != "drained" && opt.mode != "both") { std::fprintf(stderr, "error: bad --mode\n"); return 2; }
    if (opt.iters == 0) { std::fprintf(stderr, "error: --iters must be > 0\n"); return 2; }

    if (opt.pin_core >= 0 && !animus::sys::pin_current_thread_to_core(static_cast<size_t>(opt.pin_core))) {
        std::fprintf(stderr, "warning: could not pin to core %d; continuing unpinned\n", opt.pin_core);
        opt.pin_core = -1;
    }

    const char* unit = timing::tick_unit_name(timing::kCounterKind);
    std::printf("Animus Engine -- External Evaluation Harness (fast-path latency of every public ring primitive)\n");
    std::printf("================================================================================================\n");
    std::printf("Platform:                %s / %s / %s, %u hardware threads%s\n", os_name(), arch_name(), compiler_string().c_str(),
        std::thread::hardware_concurrency(), opt.pin_core >= 0 ? " (main thread pinned)" : "");
    std::printf("Clock source:            %s\n", timing::counter_kind_name(timing::kCounterKind));
    const bool invariant = timing::counter_is_invariant();
    std::printf("Invariant counter:       %s\n", invariant ? "yes" : "NO (tick counts below may not be comparable across the run)");

    const timing::Calibration cal = timing::calibrate();
    if (!cal.valid) { std::fprintf(stderr, "FATAL: counter calibration failed\n"); return 1; }
    std::printf("Calibration:             %.5f %s/ns (median of %u x %llu ms windows; spread %.0f ppm; 1 tick = %.3f ns)\n",
        cal.ticks_per_ns, unit, cal.trials, u(cal.window_ns / 1'000'000), cal.spread_ppm, cal.resolution_ns());
    if (timing::kCounterKind == timing::CounterKind::ArmGenericTimer) {
        std::printf("                         CNTFRQ_EL0 reports %llu Hz; ticks are generic-timer ticks, NOT core cycles.\n", u(cal.arm_cntfrq_hz));
    }
    if (cal.resolution_ns() > 10.0) {
        std::printf("WARNING:                 one tick is %.1f ns, coarser than the operations below: per-call figures are\n"
                    "                         quantised to that resolution. Treat sub-tick values as 0 or 1 ticks, not as ns.\n", cal.resolution_ns());
    }
    std::printf("Warm-up iterations:      %s\n", thousands(opt.warmup).c_str());
    std::printf("Benchmark iterations:    %s per primitive per mode\n", thousands(opt.iters).c_str());
    std::printf("Allocation counters:     operator new/delete: yes | malloc/free: %s\n\n",
        animus_test::malloc_instrumented() ? "yes (GNU ld --wrap)" : "no (this toolchain cannot wrap malloc; operator new only)");

    const bool live = counters_are_live();
    std::printf("Negative control (a deliberate new%s moves the counters): %s\n\n",
        animus_test::malloc_instrumented() ? " and malloc" : "", live ? "PASS" : "FAIL -- instrumentation is not live");
    if (!live) return 1;

    // Telemetry bridge sanity check (untimed) -- confirms the sampler reads a live MPMC ring.
    {
        const std::string name = unique_name("telemetry_probe");
        auto probe = ShmRingMpmc<ExecutionEvent>::create(name.c_str(), 64);
        animus::TelemetrySnapshot snap;
        const bool ok = probe && animus::AnimusGetMetrics(name.c_str(), &snap) && snap.valid &&
                        snap.ring_kind == animus::sys::ipc::RingKind::Mpmc;
        std::printf("Telemetry bridge sanity check (untimed): %s\n\n", ok ? "PASS -- AnimusGetMetrics() reads a live ShmRingMpmc" : "FAIL");
        probe.reset();
        ShmRingMpmc<ExecutionEvent>::unlink(name.c_str());
        if (!ok) return 1;
    }

    // One sample buffer for everything, allocated once, before any armed window.
    std::vector<uint64_t> samples(static_cast<size_t>(std::max<uint64_t>(opt.iters, opt.floor_samples)), 0);

    // Timer floor: back-to-back serialized reads, empty region.
    Stats floor_stats;
    {
        for (uint64_t i = 0; i < 100'000; ++i) { (void)timing::read_cycles_serialized(); (void)timing::read_cycles_serialized(); }
        for (uint64_t i = 0; i < opt.floor_samples; ++i) {
            const uint64_t t0 = timing::read_cycles_serialized();
            const uint64_t t1 = timing::read_cycles_serialized();
            samples[i] = t1 - t0;
        }
        floor_stats = summarize(samples, opt.floor_samples);
        std::printf("Timer floor (empty region, %s samples) -- included in every figure below:\n", thousands(opt.floor_samples).c_str());
        std::printf("  min %s | P50 %s | P90 %s | P99 %s | P99.9 %s | max %s %s   (P50 = %.1f ns)\n\n",
            thousands(floor_stats.min).c_str(), thousands(floor_stats.p50).c_str(), thousands(floor_stats.p90).c_str(),
            thousands(floor_stats.p99).c_str(), thousands(floor_stats.p999).c_str(), thousands(floor_stats.max).c_str(), unit,
            cal.ns(floor_stats.p50));
    }

    std::vector<Result> results;
    bool pass = true;
    for (const Target& t : kTargets) {
        if (!selected(opt.target, t.name)) continue;
        for (const Mode mode : { Mode::Stream, Mode::Drained }) {
            if (mode == Mode::Stream && opt.mode == "drained") continue;
            if (mode == Mode::Drained && (opt.mode == "stream" || !t.lossless)) continue;
            Result r = t.measure(opt, mode, samples);
            if (r.error.empty()) r.stats = summarize(samples, r.samples);
            print_result(r, cal, unit, opt.warmup);
            pass = pass && r.ok && r.error.empty() && r.allocs.total() == 0;
            results.push_back(std::move(r));
        }
    }
    if (results.empty()) { std::fprintf(stderr, "error: no target matched --target %s\n", opt.target.c_str()); return 2; }

    if (!opt.json_path.empty()) write_json(opt, cal, floor_stats, opt.floor_samples, results, pass, live);

    if (!pass) {
        std::printf("EVAL HARNESS: FAIL (a push was refused, a ring failed to build, or the allocator was touched in a hot loop)\n");
        return 1;
    }
    std::printf("EVAL HARNESS: PASS\n");
    return 0;
}
