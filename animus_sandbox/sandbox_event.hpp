#pragma once
// Shared wire format between benchmark_harness.cpp and nanobind_bridge.cpp
// so both measure the exact same payload shape -- one cache line, 64-byte
// aligned, trivially copyable so the ring buffer can plain-copy it with no
// serialization step.
#include <cstdint>
#include <type_traits>

#include "ring_buffer.hpp"

namespace sandbox {

struct alignas(kCacheLineSize) Event {
    std::uint64_t sequence;      // per-producer monotonic index
    std::uint64_t dispatch_tsc;  // sandbox::read_tsc() at enqueue time
    std::uint32_t producer_id;
    std::uint32_t reserved0;
    std::uint64_t value;         // globally unique payload, used for the
                                  // correctness checksum in benchmark_harness.cpp
    std::uint64_t reserved1[4];  // pads the struct to exactly one cache line
};

static_assert(sizeof(Event) == kCacheLineSize,
    "Event must occupy exactly one cache line");
static_assert(alignof(Event) == kCacheLineSize,
    "Event must be cache-line aligned");
static_assert(std::is_trivially_copyable_v<Event>,
    "ring slots are plain-copied, not serialized -- Event must be trivially copyable");

} // namespace sandbox
