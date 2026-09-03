#pragma once
// Canonical wire record for a synthetic execution/order event exchanged
// over a ShmRing<ExecutionEvent> segment (shm_ipc.hpp).
//
// Single source of truth shared by every process that can be on either
// end of that segment: benchmarks/harness_benchmark.cpp (the C++
// producer) and bindings/animus_shm_py.cpp (the nanobind Python
// consumer/producer) both include this header rather than each declaring
// their own copy of the struct -- two independently-maintained
// definitions of the same wire layout is exactly how a producer and
// consumer silently drift out of sync and start misinterpreting each
// other's bytes (wrong field offsets, wrong slot stride) with no error,
// just garbage data. benchmarks/consumer.py is the one exception: it has
// no C++ toolchain to include this header from, so it hardcodes the same
// 40-byte "<QQqqII" layout by hand instead -- keep that struct format
// string in sync with this one if either ever changes.
#include <cstdint>
#include <type_traits>

namespace animus {

struct ExecutionEvent {
    uint64_t sequence;        // monotonically increasing dispatch sequence number
    uint64_t dispatch_ts_raw; // raw clock sample (RDTSC ticks or clock_gettime ns) at enqueue time
    int64_t  price_ticks;     // synthetic instrument price, in ticks
    int64_t  quantity;        // synthetic order/quote quantity
    uint32_t instrument_id;   // synthetic instrument identifier
    uint32_t flags;           // reserved, always 0 -- kept for 8-byte struct alignment, not currently used
};
static_assert(sizeof(ExecutionEvent) == 40, "wire format assumes exactly 40 bytes, no padding");
static_assert(std::is_trivially_copyable<ExecutionEvent>::value, "ShmRing<T> places T directly in shared memory");

// struct.calcsize/struct.unpack_from format string for the layout above --
// matches field-for-field, byte-for-byte (see benchmarks/consumer.py).
constexpr const char* kExecutionEventWireFormat = "<QQqqII";

} // namespace animus
