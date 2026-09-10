#pragma once
// Animus Evaluation Kit -- Public Engine Interface
//
// This is the ONLY header a client needs to compile against libanimus: it
// exposes AnimusEngine's public surface (initialize/ingest_packet/poll/
// get_telemetry_metrics), the wire-compatible packet format replay_bench.cpp
// and python/animus_py.cpp both build on, and the cache-aligned ring entry
// type -- nothing else. AnimusEngine's actual ring-buffer storage, atomics,
// and cursor bookkeeping live behind a pointer-to-implementation (Impl,
// forward-declared below, defined in src/engine.cpp) so this header carries
// no internal layout for a client to depend on or reverse-engineer from a
// vtable/struct layout. Rebuilding libanimus with a different internal ring
// implementation never breaks a client compiled against this header, as
// long as the public surface below is unchanged.
//
// Target: Linux x86_64, GCC 12+/Clang 15+, C++17. The affinity utilities
// below use pthread_setaffinity_np (a thin wrapper over the sched_setaffinity
// syscall) on Linux; a best-effort Windows fallback is included purely so
// this header still compiles on a non-Linux development machine, matching
// this kit's own build (see README.md's supported-platform note).

#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>

#if defined(__linux__)
    #include <pthread.h>
    #include <sched.h>
#elif defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
    #define NOMINMAX
    #endif
    #include <windows.h>
#endif

// Symbol visibility for libanimus's shared-library boundary. GCC/Clang on
// Linux export every public symbol from a .so by default, so this matters
// only for the Windows development build (see README.md's supported-
// platform note): MSVC exports nothing from a DLL unless explicitly
// annotated, so without this AnimusEngine's methods would compile but the
// linker would produce no import library for a client to link against.
// ANIMUS_EVAL_BUILDING_DLL is defined only for the libanimus target itself
// (see CMakeLists.txt); every other translation unit that includes this
// header -- replay_bench.cpp, animus_py.cpp, any client -- gets the
// dllimport side.
#if defined(_WIN32)
    #if defined(ANIMUS_EVAL_BUILDING_DLL)
        #define ANIMUS_EVAL_API __declspec(dllexport)
    #else
        #define ANIMUS_EVAL_API __declspec(dllimport)
    #endif
#elif defined(__GNUC__)
    #define ANIMUS_EVAL_API __attribute__((visibility("default")))
#else
    #define ANIMUS_EVAL_API
#endif

namespace animus::eval {

// 64 bytes covers essentially all x86_64 server/desktop parts. Widen this
// (and the alignas() uses below) if targeting a 128-byte-line ARM part.
inline constexpr std::size_t kCacheLineSize = 64;

// ---------------------------------------------------------------------------
// CPU affinity pinning
// ---------------------------------------------------------------------------
// Best-effort by design: a latency benchmark should still run (just with
// noisier tail latency) if the host denies affinity changes, so failures
// are reported via the bool return rather than an exception -- these calls
// happen once at thread startup, never on a hot loop, so there is no cost
// concern with checking the return value at every call site.
namespace affinity {

// Pins the calling thread to logical core `core_id`. Returns false on any
// platform or call failure; never throws.
inline bool pin_current_thread_to_core(uint32_t core_id) noexcept {
#if defined(__linux__)
    // pthread_setaffinity_np is glibc's wrapper around the sched_setaffinity
    // syscall, scoped to a single thread's cpu_set_t rather than the whole
    // process -- the right granularity for pinning one producer/consumer
    // thread to its own core without disturbing sibling threads.
    if (core_id >= static_cast<uint32_t>(CPU_SETSIZE)) {
        return false;
    }
    cpu_set_t cpu_set;
    CPU_ZERO(&cpu_set);
    CPU_SET(static_cast<int>(core_id), &cpu_set);
    return ::pthread_setaffinity_np(::pthread_self(), sizeof(cpu_set_t), &cpu_set) == 0;
#elif defined(_WIN32)
    if (core_id >= 64) {
        return false; // single processor-group limit; see thread_affinity.hpp for the full rationale.
    }
    const DWORD_PTR mask = static_cast<DWORD_PTR>(1) << core_id;
    return ::SetThreadAffinityMask(::GetCurrentThread(), mask) != 0;
#else
    (void)core_id;
    return false;
#endif
}

} // namespace affinity

// ---------------------------------------------------------------------------
// Wire format
// ---------------------------------------------------------------------------
// Fixed-layout synthetic market-data tick, framed similarly to CME MDP 3.0 /
// ITCH 5.0 (sequence-numbered, fixed-point price, side-flagged quantity) but
// deliberately simplified and self-contained: this eval kit does not ship
// the production CME SBE / ITCH wire codecs, only a representative packet
// shape sized and aligned the same way a real tick would be. Field order is
// widest-first so the compiler inserts no implicit padding -- what you see
// below is the full 40-byte wire layout, no hidden bytes.
struct WireTick {
    uint64_t send_timestamp_ns; // stamped by the sender immediately before the wire write/ingest call
    uint64_t sequence;
    int64_t price_fixed;        // fixed-point price, scaled 1e-9 (nano-units) to avoid float in the wire format
    uint32_t symbol_id;
    uint32_t quantity;
    uint8_t side;                // 0 = bid, 1 = ask
    uint8_t msg_type;            // 0 = new, 1 = modify, 2 = cancel
    uint8_t reserved[2];
};
static_assert(sizeof(WireTick) == 40, "WireTick wire layout must stay exactly 40 bytes -- see field-order comment above");
static_assert(std::is_trivially_copyable_v<WireTick>, "WireTick is read directly off the wire by reinterpret_cast, not deserialized");

// One ring slot: WireTick's fields plus the receive-side timestamp
// AnimusEngine stamps at ingest_packet() time, padded to a full 64-byte
// cache line so an array of these never lets one thread's write to slot N
// invalidate a line slot N+1 also lives on (false sharing) and so a single
// entry never straddles two lines when read.
struct alignas(kCacheLineSize) MarketDataEntry {
    uint64_t send_timestamp_ns;
    uint64_t recv_timestamp_ns;
    uint64_t sequence;
    int64_t price_fixed;
    uint32_t symbol_id;
    uint32_t quantity;
    uint8_t side;
    uint8_t msg_type;
    uint8_t reserved[22];
};
static_assert(sizeof(MarketDataEntry) == kCacheLineSize, "MarketDataEntry must occupy exactly one cache line");
static_assert(std::is_trivially_copyable_v<MarketDataEntry>, "MarketDataEntry is stored in the ring by plain assignment, not serialized");

// ---------------------------------------------------------------------------
// Telemetry
// ---------------------------------------------------------------------------
struct TelemetryMetrics {
    uint64_t packets_ingested;      // ingest_packet() calls that pushed successfully
    uint64_t packets_dropped;       // ingest_packet() calls where the ring was full
    uint64_t packets_malformed;     // ingest_packet() calls rejected for a bad length
    uint64_t ring_capacity;         // fixed at initialize() time (rounded up to a power of two)
    uint64_t ring_occupancy_approx; // instantaneous, racy w.r.t. the producer/consumer -- diagnostic only
};

// Invoked once per event drained by poll(), on the calling (consumer)
// thread. `user_data` is passed through unchanged from the poll() call --
// use it to avoid capturing state in a heap-allocated closure on a path
// that must stay allocation-free.
using ConsumerCallback = void (*)(const MarketDataEntry& entry, void* user_data);

// ---------------------------------------------------------------------------
// AnimusEngine
// ---------------------------------------------------------------------------
// Single-producer/single-consumer contract: exactly one thread may call
// ingest_packet() and exactly one thread may call poll(), concurrently, for
// the lifetime of an initialized engine -- the same narrower contract that
// buys a lock-free ring its latency (see src/engine.cpp). Not enforced at
// runtime for the same reason it isn't in the production engine: a
// debug-only thread-identity check would cost real cycles on the exact path
// this class exists to make fast.
class ANIMUS_EVAL_API AnimusEngine {
public:
    AnimusEngine() noexcept;
    ~AnimusEngine();

    AnimusEngine(const AnimusEngine&) = delete;
    AnimusEngine& operator=(const AnimusEngine&) = delete;
    AnimusEngine(AnimusEngine&&) noexcept;
    AnimusEngine& operator=(AnimusEngine&&) noexcept;

    // Allocates ring storage (rounded up to the next power of two, minimum
    // 2) sized to `ring_capacity` slots and pins the *calling* thread --
    // expected to be the producer thread that will go on to call
    // ingest_packet() -- to logical core `cpu_core`. All allocation for the
    // lifetime of the engine happens here; ingest_packet()/poll() never
    // allocate. Returns false (and leaves the engine uninitialized) if
    // ring_capacity is zero or the affinity pin fails.
    bool initialize(uint32_t cpu_core, size_t ring_capacity) noexcept;

    // Producer-thread-only. Interprets `buffer` as a WireTick (rejecting
    // and counting as malformed anything shorter than sizeof(WireTick)),
    // stamps it with the receive-side timestamp, and pushes it onto the
    // ring. Never blocks and never allocates: a full ring counts the packet
    // as dropped and returns false rather than retrying or growing.
    bool ingest_packet(const uint8_t* buffer, size_t len) noexcept;

    // Consumer-thread-only. Pops up to `max_events` entries (SIZE_MAX means
    // "drain everything currently available"), invoking `cb` once per entry
    // in FIFO order. Never blocks: returns as soon as the ring is empty or
    // the requested count is reached. Returns the number of entries
    // actually delivered.
    size_t poll(ConsumerCallback cb, void* user_data, size_t max_events) noexcept;

    // Snapshot of the running counters. Safe to call from any thread at any
    // time (including concurrently with ingest_packet()/poll()); the
    // snapshot itself is not atomic across fields, so treat it as
    // diagnostic/telemetry data, not a synchronization primitive.
    TelemetryMetrics get_telemetry_metrics() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace animus::eval
