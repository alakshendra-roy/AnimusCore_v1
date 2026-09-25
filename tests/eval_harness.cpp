// Animus Engine -- External Evaluation Harness for the Telemetry Bridge.
//
// Measures ShmRing<ExecutionEvent>::try_push() -- the actual lock-free
// ring-buffer write a producer calls on its hot path -- not
// animus::AnimusGetMetrics() (telemetry.hpp). That distinction matters:
// AnimusGetMetrics() is documented in telemetry.hpp as an unattached
// diagnostic sampler that opens/reads/closes a fresh OS shared-memory
// mapping on every single call, by design (it never holds a persistent
// handle, so an arbitrary external monitor can sample a ring it was never
// compiled against). Timing that call reports CreateFileMappingA/
// MapViewOfFile syscall overhead, not the engine's ring-write latency --
// an earlier revision of this harness made exactly that mistake. The ring
// itself is mapped once, outside every timed loop; the code under
// measurement is only the produce-side atomic head/tail bookkeeping plus
// the fixed-size slot copy.
//
// Zero-allocation claim is measured, not asserted by inspection: global
// operator new/delete are overridden with atomic counters below, snapshotted
// immediately before the 1,000,000-iteration benchmark loop and read back
// immediately after, so any allocation shows up as a nonzero delta. Ring
// creation and the two std::vector reserve() calls happen strictly before
// that snapshot.
//
// Latency methodology matches benchmarks/telemetry_benchmark.cpp: lfence-
// serialized RDTSC, a warm-up phase through the identical path before timing
// starts, and percentile (not mean) reporting.
#include "../include/animus/telemetry.hpp"
#include "../include/animus/shm_ipc.hpp"
#include "../include/animus/execution_event.hpp"
#include "../include/animus/thread_affinity.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <vector>

#if defined(_MSC_VER)
    #include <intrin.h>
#else
    #include <cpuid.h>
    #include <x86intrin.h>
#endif

// ===== Global operator new/delete override with allocation counters =====
// std::malloc/std::free directly underneath -- never routes back through
// `new`/`delete`, so there is no recursion risk here.
namespace {
    std::atomic<std::uint64_t> g_new_count{0};
    std::atomic<std::uint64_t> g_delete_count{0};
} // namespace

void* operator new(std::size_t size) {
    g_new_count.fetch_add(1, std::memory_order_relaxed);
    void* p = std::malloc(size);
    if (!p) throw std::bad_alloc();
    return p;
}
void* operator new[](std::size_t size) {
    g_new_count.fetch_add(1, std::memory_order_relaxed);
    void* p = std::malloc(size);
    if (!p) throw std::bad_alloc();
    return p;
}
void operator delete(void* p) noexcept {
    g_delete_count.fetch_add(1, std::memory_order_relaxed);
    std::free(p);
}
void operator delete(void* p, std::size_t) noexcept {
    g_delete_count.fetch_add(1, std::memory_order_relaxed);
    std::free(p);
}
void operator delete[](void* p) noexcept {
    g_delete_count.fetch_add(1, std::memory_order_relaxed);
    std::free(p);
}
void operator delete[](void* p, std::size_t) noexcept {
    g_delete_count.fetch_add(1, std::memory_order_relaxed);
    std::free(p);
}

// ===== Compile-time cache-line alignment checks =====
// animus::sys::ipc::RingHeader is the struct the timed loop's try_push()
// actually touches (header_->head/tail) -- not a harness-invented type --
// so this is a direct check on the real hot-path layout, not a proxy for
// it. animus::CacheAligned/CacheLinePad (thread_affinity.hpp) are the
// engine's general-purpose cache-line primitives, checked alongside it.
static_assert(animus::cache_line_size >= 64,
    "ANIMUS_CACHE_LINE_SIZE must be at least 64 bytes on any supported target");
static_assert(alignof(animus::sys::ipc::RingHeader) >= 64,
    "RingHeader (holds the producer-owned head cursor try_push() writes) must be cache-line aligned");
static_assert(alignof(animus::CacheAligned<animus::TelemetrySnapshot>) >= 64,
    "CacheAligned<T> must be cache-line aligned");
static_assert(alignof(animus::CacheLinePad) >= 64,
    "CacheLinePad must be cache-line aligned -- it exists specifically to separate hot fields onto distinct lines");

using animus::AnimusGetMetrics;
using animus::TelemetrySnapshot;
using animus::ExecutionEvent;
using SpscRing = animus::sys::ipc::ShmRing<ExecutionEvent>;

namespace {

// Serialized RDTSC: see benchmarks/telemetry_benchmark.cpp's file header for
// why both fences are required (rdtsc alone is not a serializing
// instruction and can be reordered relative to surrounding code).
inline std::uint64_t rdtsc_serialized() noexcept {
    _mm_lfence();
    const std::uint64_t t = __rdtsc();
    _mm_lfence();
    return t;
}

// CPUID leaf 0x80000007, EDX bit 8: "Invariant TSC" -- the TSC ticks at a
// fixed rate regardless of core P-state/C-state transitions, which is the
// property that makes raw cycle counts meaningful to report at all here.
bool has_invariant_tsc() noexcept {
    std::array<unsigned int, 4> regs{};
#if defined(_MSC_VER)
    int cpu_info[4];
    __cpuid(cpu_info, static_cast<int>(0x80000007));
    regs = {static_cast<unsigned>(cpu_info[0]), static_cast<unsigned>(cpu_info[1]),
             static_cast<unsigned>(cpu_info[2]), static_cast<unsigned>(cpu_info[3])};
#else
    __cpuid(0x80000007, regs[0], regs[1], regs[2], regs[3]);
#endif
    return (regs[3] & (1u << 8)) != 0; // EDX bit 8
}

struct PercentileReport {
    std::uint64_t p50 = 0, p90 = 0, p99 = 0, min = 0, max = 0;
};

PercentileReport summarize(std::vector<std::uint64_t>& samples) {
    std::sort(samples.begin(), samples.end());
    const std::size_t n = samples.size();
    auto at_percentile = [&](double p) -> std::uint64_t {
        std::size_t idx = static_cast<std::size_t>(p * static_cast<double>(n - 1));
        if (idx >= n) idx = n - 1;
        return samples[idx];
    };
    PercentileReport r;
    r.min = samples.front();
    r.p50 = at_percentile(0.50);
    r.p90 = at_percentile(0.90);
    r.p99 = at_percentile(0.99);
    r.max = samples.back();
    return r;
}

} // namespace

int main() {
    constexpr const char* kRingName = "animus_eval_harness_ring";
    constexpr std::uint64_t kWarmupIters = 100'000;
    constexpr std::uint64_t kBenchIters  = 1'000'000;
    // Sized so try_push() never sees a full ring across warm-up + benchmark
    // (no consumer drains it) -- the timed region is pure produce-side cost,
    // with no pop-side interleaving and no push_overwrite() reclaim branch.
    // Next power of two >= kWarmupIters + kBenchIters (1,100,000).
    constexpr std::size_t kRingCapacity = 1u << 21; // 2,097,152

    std::printf("Animus Engine -- Telemetry Bridge External Evaluation Harness\n");
    std::printf("================================================================\n");
    std::printf("Target:                  ShmRing<ExecutionEvent>::try_push() [include/animus/shm_ipc.hpp]\n");
    std::printf("Clock source:            RDTSC (lfence-serialized), invariant TSC: %s\n",
        has_invariant_tsc() ? "yes" : "NO (cycle counts below may not be comparable across the run)");
    std::printf("Warm-up iterations:      %llu\n", static_cast<unsigned long long>(kWarmupIters));
    std::printf("Benchmark iterations:    %llu\n\n", static_cast<unsigned long long>(kBenchIters));

    // ---- Setup: the persistent ring handle is mapped ONCE, here, outside every timed loop ----
    auto ring = SpscRing::create(kRingName, kRingCapacity);
    if (!ring) {
        std::fprintf(stderr, "FATAL: SpscRing::create('%s') failed (stale segment from a previous run?)\n", kRingName);
        return 1;
    }
    ring->mark_producer_attached();

    // One untimed AnimusGetMetrics() call -- confirms the telemetry bridge
    // actually reads this ring's header correctly. Not part of any timed
    // loop; this is exactly the "external monitor" use case telemetry.hpp
    // is built for, distinct from the producer's own try_push() hot path
    // measured below.
    {
        TelemetrySnapshot snap;
        const bool bridge_ok = AnimusGetMetrics(kRingName, &snap);
        std::printf("Telemetry bridge sanity check (untimed): %s\n\n",
            (bridge_ok && snap.valid && snap.ring_kind == animus::sys::ipc::RingKind::Spsc)
                ? "PASS -- AnimusGetMetrics() reads this ring correctly" : "FAIL");
    }

    std::vector<std::uint64_t> warmup_samples;
    std::vector<std::uint64_t> bench_samples;
    warmup_samples.reserve(kWarmupIters); // sole heap activity for warm-up; happens before its own timed reads
    bench_samples.reserve(kBenchIters);   // sole heap allocation for the benchmark; happens before the counter snapshot below

    // ---- Warm-up: identical try_push() path, unmeasured, caches/branch predictors hot ----
    for (std::uint64_t i = 0; i < kWarmupIters; ++i) {
        ExecutionEvent ev{};
        ev.sequence = i;
        ev.dispatch_ts_raw = i;
        ev.instrument_id = 1;

        const std::uint64_t t0 = rdtsc_serialized();
        const bool ok = ring->try_push(ev);
        const std::uint64_t t1 = rdtsc_serialized();
        if (!ok) {
            std::fprintf(stderr, "FATAL: try_push() reported the ring full during warm-up (iteration %llu) -- capacity too small\n",
                static_cast<unsigned long long>(i));
            return 1;
        }
        warmup_samples.push_back(t1 - t0); // reserved above -- no reallocation
    }

    // ---- Snapshot allocation counters immediately before the measured region ----
    const std::uint64_t new_before = g_new_count.load(std::memory_order_relaxed);
    const std::uint64_t delete_before = g_delete_count.load(std::memory_order_relaxed);

    // ---- Benchmark: 1,000,000 TSC-timed calls to the lock-free ring write, nothing else ----
    for (std::uint64_t i = 0; i < kBenchIters; ++i) {
        ExecutionEvent ev{};
        ev.sequence = kWarmupIters + i;
        ev.dispatch_ts_raw = kWarmupIters + i;
        ev.instrument_id = 1;

        const std::uint64_t t0 = rdtsc_serialized();
        const bool ok = ring->try_push(ev);
        const std::uint64_t t1 = rdtsc_serialized();
        if (!ok) {
            std::fprintf(stderr, "FATAL: try_push() reported the ring full during benchmark (iteration %llu) -- capacity too small\n",
                static_cast<unsigned long long>(i));
            return 1;
        }
        bench_samples.push_back(t1 - t0); // reserved above -- no reallocation
    }

    const std::uint64_t new_after = g_new_count.load(std::memory_order_relaxed);
    const std::uint64_t delete_after = g_delete_count.load(std::memory_order_relaxed);
    const std::uint64_t new_delta = new_after - new_before;
    const std::uint64_t delete_delta = delete_after - delete_before;
    const bool zero_alloc = (new_delta == 0) && (delete_delta == 0);

    const PercentileReport report = summarize(bench_samples);

    std::printf("Ring segment:            '%s' (ShmRing<ExecutionEvent>, capacity %llu)\n",
        kRingName, static_cast<unsigned long long>(ring->capacity()));
    std::printf("Cache-line size:         %zu bytes\n",  animus::cache_line_size);
    std::printf("alignof(RingHeader):     %zu bytes\n\n", alignof(animus::sys::ipc::RingHeader));

    std::printf("Heap allocations during the %llu-iteration benchmark loop:\n",
        static_cast<unsigned long long>(kBenchIters));
    std::printf("  operator new/new[] calls:    %llu\n", static_cast<unsigned long long>(new_delta));
    std::printf("  operator delete/delete[] calls: %llu\n", static_cast<unsigned long long>(delete_delta));
    std::printf("  Result: %s\n\n", zero_alloc ? "PASS -- zero heap allocations" : "FAIL -- heap allocation detected");

    std::printf("ShmRing<ExecutionEvent>::try_push() latency, raw invariant-TSC cycles (%llu samples):\n",
        static_cast<unsigned long long>(kBenchIters));
    std::printf("  ---------------------------------------------\n");
    std::printf("  %-8s %15llu cycles\n", "min", static_cast<unsigned long long>(report.min));
    std::printf("  %-8s %15llu cycles\n", "P50", static_cast<unsigned long long>(report.p50));
    std::printf("  %-8s %15llu cycles\n", "P90", static_cast<unsigned long long>(report.p90));
    std::printf("  %-8s %15llu cycles\n", "P99", static_cast<unsigned long long>(report.p99));
    std::printf("  %-8s %15llu cycles\n", "max", static_cast<unsigned long long>(report.max));
    std::printf("  ---------------------------------------------\n\n");

    SpscRing::unlink(kRingName);

    if (!zero_alloc) {
        std::printf("EVAL HARNESS: FAIL (heap allocation detected in hot loop)\n");
        return 1;
    }
    std::printf("EVAL HARNESS: PASS\n");
    return 0;
}
