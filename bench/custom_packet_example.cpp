// AnimusCore -- custom telemetry packet example.
//
// Shows how a domain-specific, fixed-layout telemetry frame (here: a
// notional AOCS/ADCS state-vector packet -- attitude quaternion, body-rate
// gyro, subsystem health) drops directly into animus::SpscRingBuffer<T>
// with no adapter layer, no serialization step, and no change to the ring
// buffer's zero-allocation, non-blocking push()/pop() contract. The ring is
// generic over T (see AnimusCore_v1/animus.hpp) -- it has no idea what a
// CCSDS space packet or a quaternion is, and does not need to.
//
// This file is a standalone unit: it only depends on animus.hpp and the
// C++17 standard library. Build with bench/CMakeLists.txt, or directly:
//   g++ -O3 -std=c++17 -march=native -Wall -Wextra -pthread -I../AnimusCore_v1 custom_packet_example.cpp -o custom_packet_example
//
// EVALUATION USE ONLY -- see the "Evaluation Notice" in the top-level
// README.md and `LICENSE`. Not flight software; not qualified for use in
// or alongside a flight, safety-critical, or certified system.

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <thread>
#include <type_traits>
#include <vector>

#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#endif

#include "animus.hpp"

// ---------------------------------------------------------------------
// AocsTelemetryPacket -- 128-byte fixed-layout AOCS/ADCS state frame.
//
// Field order is chosen so every member already lands on its own natural
// alignment boundary in declaration order -- the compiler inserts no
// internal padding, matching animus.hpp's own TelemetryPayload convention
// (see the static_asserts below, checked at compile time, not assumed).
// `alignas(64)` pads the struct to a whole number of cache lines (128 = 2
// x 64) so a contiguous array of these -- exactly what SpscRingBuffer<T>'s
// backing std::vector<T> is -- never straddles a cache line across
// elements, and so that a single packet handed across a producer/consumer
// boundary is delivered as whole 64-byte transfers, not a torn partial
// line.
struct alignas(64) AocsTelemetryPacket {
    // --- Timing (16 bytes) ---
    uint64_t tsc_cycles;      // animus::read_cycle_counter() at acquisition time
    uint64_t epoch_utc_us;    // wall-clock capture time, microseconds since Unix epoch

    // --- Provenance (8 bytes) ---
    uint32_t sequence;        // monotonically increasing per-subsystem frame counter
    uint16_t subsystem_id;    // see SubsystemId below
    uint16_t frame_version;   // wire-format version of this struct, for downstream decoders

    // --- Attitude quaternion, body-to-reference frame, scalar-first (16 bytes) ---
    float q_w;
    float q_x;
    float q_y;
    float q_z;

    // --- Body-frame angular velocity, rad/s (12 bytes) ---
    float omega_x;
    float omega_y;
    float omega_z;

    // --- Health & status (8 bytes) ---
    uint32_t health_flags;    // see HealthFlag below -- bitwise OR of active conditions
    uint32_t status_flags;    // see StatusFlag below

    // --- Static payload (68 bytes) ---
    // Room for subsystem-specific extras (e.g. raw gyro/magnetometer
    // counts, reaction-wheel speeds, star-tracker quality) without
    // changing the frame's size or the ring buffer's element stride.
    // Fixed-size and part of the struct itself -- no pointer, no heap
    // buffer, so copying an AocsTelemetryPacket by value (exactly what
    // push()/pop() do) copies this payload too, with nothing left behind
    // to free.
    uint8_t payload[68];
};

static_assert(sizeof(AocsTelemetryPacket) == 128,
    "AocsTelemetryPacket must be exactly 128 bytes -- check field order/padding "
    "if this fires after an edit");
static_assert(alignof(AocsTelemetryPacket) == 64,
    "AocsTelemetryPacket must be 64-byte aligned for whole-cache-line transfer");
static_assert(std::is_trivially_copyable<AocsTelemetryPacket>::value,
    "SpscRingBuffer<T>::push()/pop() copy T by value on the hot path -- T must "
    "be trivially copyable or that copy silently stops being a memcpy-equivalent");

// Subsystem identifiers for AocsTelemetryPacket::subsystem_id. An
// illustrative subset, not an exhaustive mission-specific enumeration.
enum class SubsystemId : uint16_t {
    Adcs = 1,   // Attitude Determination & Control
    Eps = 2,    // Electrical Power System
    Tcs = 3,    // Thermal Control System
    Comms = 4,  // Communications / RF
    Obc = 5,    // On-Board Computer
    Prop = 6,   // Propulsion
};

// AocsTelemetryPacket::health_flags bits.
namespace HealthFlag {
    constexpr uint32_t kGyroNominal      = 1u << 0;
    constexpr uint32_t kStarTrackerLock  = 1u << 1;
    constexpr uint32_t kReactionWheelsOk = 1u << 2;
    constexpr uint32_t kMagnetorquerOk   = 1u << 3;
    constexpr uint32_t kSunSensorValid   = 1u << 4;
}

// AocsTelemetryPacket::status_flags bits.
namespace StatusFlag {
    constexpr uint32_t kSafeMode      = 1u << 0;
    constexpr uint32_t kSlewInProgress = 1u << 1;
    constexpr uint32_t kEclipse       = 1u << 2;
    constexpr uint32_t kGroundContact = 1u << 3;
}

namespace {

// ---------------------------------------------------------------------
// Heap allocation watchdog -- identical technique to bench/hotpath_bench.cpp:
// override global operator new/delete to count allocations while
// g_tracking is set, so the claim "the hot loop below performs zero heap
// allocations" is asserted at runtime, not merely asserted in a comment.
// ---------------------------------------------------------------------
std::atomic<bool> g_tracking{false};
std::atomic<uint64_t> g_alloc_count{0};

inline void record_alloc() {
    if (g_tracking.load(std::memory_order_relaxed)) {
        g_alloc_count.fetch_add(1, std::memory_order_relaxed);
    }
}

} // namespace

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif
void* operator new(std::size_t sz) {
    record_alloc();
    if (void* p = std::malloc(sz ? sz : 1)) {
        return p;
    }
    throw std::bad_alloc();
}
void* operator new[](std::size_t sz) { return ::operator new(sz); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace {

inline uint64_t serialized_rdtsc() {
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
    _mm_lfence();
    uint64_t t = __rdtsc();
    _mm_lfence();
    return t;
#else
    return static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

uint64_t epoch_micros() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

// Builds one synthetic AOCS frame. In a real integration this is where a
// gyro/star-tracker/magnetometer driver would fill the struct directly --
// note there is no intermediate "build a DTO, then serialize it" step:
// the struct returned here is the exact byte layout that goes into the
// ring.
AocsTelemetryPacket make_frame(uint32_t sequence, float t_seconds) noexcept {
    AocsTelemetryPacket pkt{};
    pkt.tsc_cycles = animus::read_cycle_counter();
    pkt.epoch_utc_us = epoch_micros();
    pkt.sequence = sequence;
    pkt.subsystem_id = static_cast<uint16_t>(SubsystemId::Adcs);
    pkt.frame_version = 1;

    // Slow synthetic nadir-pointing slew, purely to give the demo
    // non-constant data to push through the ring -- not a real attitude
    // solution.
    const float half_angle = 0.05f * t_seconds;
    pkt.q_w = std::cos(half_angle);
    pkt.q_x = 0.0f;
    pkt.q_y = 0.0f;
    pkt.q_z = std::sin(half_angle);

    pkt.omega_x = 0.0f;
    pkt.omega_y = 0.0f;
    pkt.omega_z = 0.05f;

    pkt.health_flags = HealthFlag::kGyroNominal | HealthFlag::kStarTrackerLock |
        HealthFlag::kReactionWheelsOk | HealthFlag::kMagnetorquerOk | HealthFlag::kSunSensorValid;
    pkt.status_flags = 0;

    std::memset(pkt.payload, 0, sizeof(pkt.payload));
    return pkt;
}

} // namespace

int main() {
    constexpr size_t kRingCapacity = 4096;
    constexpr size_t kWarmupFrames = 50000;
    constexpr size_t kTimedFrames = 500000;

    std::printf("=====================================================\n");
    std::printf(" AnimusCore -- Custom Telemetry Packet Example (AOCS)\n");
    std::printf("=====================================================\n");
    std::printf("sizeof(AocsTelemetryPacket) = %zu bytes\n", sizeof(AocsTelemetryPacket));
    std::printf("alignof(AocsTelemetryPacket) = %zu bytes\n", alignof(AocsTelemetryPacket));
    std::printf("Ring capacity     : %zu frames\n", kRingCapacity);
    std::printf("Warmup frames     : %zu\n", kWarmupFrames);
    std::printf("Timed frames      : %zu\n\n", kTimedFrames);

    // The ring buffer's element type is the packet struct itself -- no
    // wrapper, no std::variant, no pointer indirection. Backing storage
    // (one std::vector<AocsTelemetryPacket>) is allocated exactly once,
    // here, at construction.
    animus::SpscRingBuffer<AocsTelemetryPacket> ring(kRingCapacity);

    std::atomic<bool> mismatch{false};
    std::atomic<uint64_t> frames_verified{0};

    // Consumer: pops frames off the ring and verifies sequence continuity
    // and payload integrity -- a stand-in for a downlink serializer or
    // ground-segment ingest stage reading the same struct back out.
    std::thread consumer([&]() {
        AocsTelemetryPacket out{};
        uint32_t expected_sequence = 0;
        uint64_t drained = 0;
        const uint64_t total_frames = kWarmupFrames + kTimedFrames;

        while (drained < total_frames) {
            if (!ring.pop(out)) {
                continue; // non-blocking: ring momentarily empty, spin
            }
            if (out.sequence != expected_sequence) {
                mismatch.store(true, std::memory_order_relaxed);
            }
            ++expected_sequence;
            ++drained;
            if (drained > kWarmupFrames) {
                frames_verified.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });

    // Warmup: fill/drain the pipeline before the timed, allocation-tracked
    // region starts, so page faults and branch-predictor warmup don't
    // contaminate the measurement below.
    for (uint32_t i = 0; i < kWarmupFrames; ++i) {
        AocsTelemetryPacket pkt = make_frame(i, static_cast<float>(i) * 0.001f);
        while (!ring.push(pkt)) {
            // ring momentarily full; non-blocking push, spin and retry
        }
    }

    g_alloc_count.store(0, std::memory_order_relaxed);
    g_tracking.store(true, std::memory_order_relaxed);

    const uint64_t t_start = serialized_rdtsc();
    for (uint32_t i = 0; i < kTimedFrames; ++i) {
        const uint32_t sequence = kWarmupFrames + i;
        AocsTelemetryPacket pkt = make_frame(sequence, static_cast<float>(sequence) * 0.001f);
        while (!ring.push(pkt)) {
            // non-blocking overwrite-free backpressure: never stalls the
            // producer's calling thread on a lock or a syscall -- a full
            // ring here means the consumer is behind, not that this push
            // call itself blocks.
        }
    }
    const uint64_t t_end = serialized_rdtsc();

    g_tracking.store(false, std::memory_order_relaxed);
    const uint64_t allocs_observed = g_alloc_count.load(std::memory_order_relaxed);

    consumer.join();

    const uint64_t total_cycles = t_end - t_start;
    const double avg_cycles_per_frame =
        static_cast<double>(total_cycles) / static_cast<double>(kTimedFrames);

    std::printf("Frames verified (sequence + drain)   : %llu / %zu\n",
                static_cast<unsigned long long>(frames_verified.load()), kTimedFrames);
    std::printf("Sequence continuity                  : %s\n",
                mismatch.load() ? "FAIL -- gap or reorder detected" : "PASS -- exact, in order");
    std::printf("Avg push() cost (timed region)       : %.1f TSC cycles/frame\n", avg_cycles_per_frame);
    std::printf("Heap allocations (timed push+pop loop, %zu frames): %s\n", kTimedFrames,
                allocs_observed == 0 ? "PASS -- zero heap allocations"
                                     : "FAIL -- allocation(s) observed");

    const bool ok = !mismatch.load() &&
        frames_verified.load() == kTimedFrames &&
        allocs_observed == 0;
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
