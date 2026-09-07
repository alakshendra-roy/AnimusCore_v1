#pragma once
// Animus ITCH 5.0 Adapter -- uniform ring frame.
//
// The eight ITCH wire messages (itch50_messages.hpp) are eight different
// sizes (12 to 40 bytes) and layouts -- not a shape either
// animus::eval::SpscRingBuffer<T> (fixed-size slots, one T per slot) or
// animus::sys::ipc::ShmRing<T> (same constraint, cross-process) can hold
// directly. ItchFrame below is the fixed-size, tagged-union-style record
// every decoded message normalizes into before it is pushed onto the ring
// -- decode once at the network boundary, carry one uniform 64-byte
// record for every message type from there on, so nothing downstream of
// the ring (the consumer thread, a Python reader) needs to know which of
// the eight wire shapes originally produced a given slot beyond reading
// msg_type.
//
// Field reuse across message types (documented per-field below) is
// deliberate, not an oversight: a full order-book reconstruction engine
// would want e.g. a separate MPID/attribution field for 'F', but this
// adapter's stated scope is proving zero-copy/zero-allocation ring
// ingestion of parsed ITCH telemetry, not shipping a complete book
// builder -- see adapters/itch50/README.md's Scope section.
//
// alignas(64) + the field layout below pads sizeof(ItchFrame) to exactly
// 64 bytes (one cache line), the same reasoning as
// animus-eval-kit/include/telemetry_frame.hpp's TelemetryFrame: one frame
// never straddles two cache lines, and an array of frames never causes
// false sharing between adjacent slots.

#include "itch50_messages.hpp"

#include "animus/schema.hpp"

#include <cstdint>
#include <type_traits>

namespace adapters {
namespace itch50 {

    struct alignas(64) ItchFrame {
        uint64_t sequence_id;         // adapter-assigned monotonic ingest sequence (not an ITCH wire field) -- verifies ring ordering end to end
        uint64_t recv_timestamp_ns;   // host ingest clock sample, taken immediately before decode -- NOT the ITCH Timestamp field below
        uint64_t itch_timestamp_ns;   // decoded ITCH Timestamp (nanoseconds since midnight), every message type
        uint64_t order_ref_number;    // OrderReferenceNumber (A/F/E/C/X/D) or OriginalOrderReferenceNumber (U); 0 for S
        uint64_t secondary_ref_number;// MatchNumber (E/C) or NewOrderReferenceNumber (U); 0 otherwise
        uint64_t stock_ticker_raw;    // A/F only: the 8-byte ITCH Stock field, carried as an opaque byte blob (see decode note below) -- NOT byte-swapped, 0 otherwise
        int64_t  price_ticks;         // decoded Price/ExecutionPrice, ITCH's native fixed-point unit (4 implied decimal digits); 0 where absent
        uint32_t shares;              // Shares (A/F/U) / ExecutedShares (E/C) / CancelledShares (X); 0 for S/D
        uint16_t stock_locate;        // StockLocate -- every message type carries this; the natural per-symbol routing key downstream
        uint8_t  msg_type;            // adapters::itch50::MessageType tag
        uint8_t  side_or_flag;        // BuySellIndicator ('B'/'S', A/F) or Printable ('Y'/'N', C) or EventCode (S); 0 otherwise -- mutually exclusive by msg_type, see decode note below
    };
    static_assert(sizeof(ItchFrame) == 64, "ItchFrame must occupy exactly one cache line");
    static_assert(std::is_trivially_copyable<ItchFrame>::value,
        "ItchFrame must be trivially copyable -- pushed onto the SPSC ring by plain assignment, no serialization step");
    static_assert(alignof(ItchFrame) <= 64,
        "ItchFrame's alignment must not exceed 64 bytes -- SpscRingBuffer<T>/ShmRing<T> both pack slots "
        "back-to-back with no per-slot padding beyond T's own size");

    // stock_ticker_raw decode note: the ITCH Stock field is eight raw ASCII
    // bytes (right-padded with spaces), not a big-endian integer -- there
    // is no numeric value to byte-swap. It is memcpy'd verbatim into this
    // field's 8 bytes in itch50_codec.hpp, so on this adapter's
    // little-endian target (x86_64/ARM64) byte 0 of the ticker lands at
    // byte 0 of stock_ticker_raw both here and in a Python reader's
    // little-endian NumPy view of the same bytes -- decode it back to text
    // with `.tobytes()` / `struct.pack("<Q", value)`, never by treating it
    // as a number to print.

    // side_or_flag decode note: this one byte carries a different,
    // message-type-specific ASCII value depending on msg_type -- callers
    // must branch on msg_type before interpreting it (documented per field
    // above), the same reuse discipline animus::ExecutionEvent's own
    // `flags` field already establishes for a single shared byte meaning
    // different things depending on context (include/animus/execution_event.hpp).

} // namespace itch50
} // namespace adapters

// Wire-schema registration (Milestone 1, include/animus/schema.hpp) --
// this is what lets a cross-process animus::sys::ipc::ShmRing<ItchFrame>
// stamp ItchFrame's identity into its segment header, and what lets
// animus.dynamic_schema.to_structured_array() on the Python side build a
// matching NumPy structured dtype at runtime with no compiled extension
// specific to ItchFrame -- see adapters/itch50/verify_zero_copy_numpy.py.
//
// All eleven fields are fixed-width scalar types (Q/q/I/H/B), deliberately
// never a struct.calcsize "Ns" byte-string field: wire_format_to_dtype()
// (animus/dynamic_schema.py) only understands fixed-width scalars, not
// character arrays -- seeing stock_ticker_raw declared as a plain uint64_t
// (`Q`) rather than an 8-byte string is exactly what keeps this schema
// decodable by that existing, unmodified Python helper.
ANIMUS_DEFINE_SCHEMA(adapters::itch50::ItchFrame, "<QQQQQQqIHBB")
