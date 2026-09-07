#pragma once
// Animus Evaluation Kit -- Wire-format telemetry frame shared by the C++
// ingestion benchmark harness (benchmarks/animus_harness.cpp) and the
// zero-copy Python SDK (sdk/python/). Factored out of animus_harness.cpp
// into its own header specifically so both consumers compile against the
// exact same struct layout -- a hand-duplicated copy in each translation
// unit would silently drift out of sync the moment either side changed a
// field, which is exactly the failure mode a wire-format struct can never
// afford.
//
// alignas(64) plus the field layout below pads sizeof(TelemetryFrame) up to
// exactly 64 bytes, so one frame never straddles two cache lines and an
// array of frames never causes false sharing between adjacent elements --
// see animus-eval-kit/include/spsc_ring_buffer.hpp's own header comment for
// why that matters on the ring's hot path.

#include <cstdint>
#include <type_traits>

namespace animus::eval {

struct alignas(64) TelemetryFrame {
    uint64_t sequence_id;
    uint64_t timestamp_ns;
    char symbol[8];
    double price;
    uint32_t volume;
    uint8_t flags;
};
static_assert(sizeof(TelemetryFrame) == 64, "TelemetryFrame must occupy exactly one cache line");
static_assert(std::is_trivially_copyable_v<TelemetryFrame>, "TelemetryFrame must be trivially copyable for the SPSC ring");

inline constexpr uint8_t kFlagBurst = 0x01;

// struct.calcsize-compatible wire format for TelemetryFrame, used by the
// Python SDK (sdk/python/animus_sdk/consumer.py) to build a matching NumPy
// structured dtype without hand-decoding offsets on both sides. Field
// order and sizes must track the struct above exactly, including the
// explicit padding bytes: 8+8+8+8+4+1 = 37 real bytes, padded to the
// struct's 64-byte alignment by 27 trailing pad bytes ("27x").
inline constexpr const char* kTelemetryFrameWireFormat = "<QQ8sdIB27x";

} // namespace animus::eval
