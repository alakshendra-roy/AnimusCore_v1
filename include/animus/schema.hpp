#pragma once
// Milestone 1: Dynamic & User-Defined Wire Schemas.
//
// Until now, ShmRing<T> (shm_ipc.hpp) was mechanically generic over T (any
// trivially-copyable type compiles) but every concrete wire schema in this
// repo hardcoded exactly one T -- animus::ExecutionEvent -- end to end:
// the C++ producer, the nanobind Python binding, and even ShmRing<T>::open()
// itself, which validated a segment's capacity/mask but never checked that
// the *record type* the caller is about to reinterpret raw bytes as
// actually matches what create() was called with. Two different
// trivially-copyable T's of the same size could attach to the same segment
// and silently misread each other's fields -- wrong offsets, wrong types,
// no error.
//
// This header is the client-facing half of the fix: a macro/template
// interface (ANIMUS_DEFINE_SCHEMA) for registering a wire struct's
// identity -- its name, its struct.calcsize-compatible format string, and
// a version hash derived from both plus sizeof(T) -- so ShmRing<T>::create()
// can stamp that identity into the segment's header and open() can refuse
// to attach with the wrong T (see shm_ipc.hpp's RingHeader/create()/open()).
//
// Usage, for a client-defined struct like MarketDepthEvent, OrderBookL2, or
// AlphaSignal: define a plain trivially-copyable POD struct as usual, then
// register it with one macro call naming its wire format string --
// precisely the same two-step convention execution_event.hpp already
// established for the built-in ExecutionEvent, just factored out here so
// any struct can opt in, not only that one:
//
//   struct AlphaSignal {
//       uint64_t sequence;
//       uint64_t dispatch_ts_raw;
//       double   signal_strength;
//       uint32_t instrument_id;
//       uint32_t model_id;
//   };
//   ANIMUS_DEFINE_SCHEMA(AlphaSignal, "<QQdII")
//
// A T with no ANIMUS_DEFINE_SCHEMA call still works with ShmRing<T> exactly
// as before (the primary Traits<T> template below supplies a fallback
// identity derived from sizeof(T) alone) -- registration is opt-in, not a
// breaking requirement for the RawEvent/OrderRequest/etc. record types
// already in use elsewhere in this codebase (animus.hpp) that predate this
// milestone and have no wire format string of their own to advertise.
#include "execution_event.hpp"

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace animus {
namespace schema {
namespace detail {

    // Compile-time FNV-1a over a NUL-terminated string literal. Two
    // processes that both compiled this same header always compute the
    // identical hash for the identical {name, wire format, size} triple,
    // with no need to actually exchange the strings themselves at attach
    // time -- the whole point of storing just the hash (plus the format
    // string itself, kept too for the Python dynamic-dtype path -- see
    // shm_ipc.hpp's RingHeader) in the shared segment header.
    constexpr uint64_t kFnvOffsetBasis = 14695981039346656037ull;
    constexpr uint64_t kFnvPrime = 1099511628211ull;

    constexpr uint64_t fnv1a(const char* s, uint64_t hash = kFnvOffsetBasis) noexcept {
        return (*s == '\0') ? hash
                             : fnv1a(s + 1, (hash ^ static_cast<uint64_t>(static_cast<unsigned char>(*s))) * kFnvPrime);
    }

    // Mixes a plain 64-bit integer (sizeof(T)) into an existing FNV-1a
    // hash, one byte at a time, the same way the string hash above folds
    // in each character.
    constexpr uint64_t fnv1a_mix_u64(uint64_t hash, uint64_t value) noexcept {
        for (int byte = 0; byte < 8; ++byte) {
            hash = (hash ^ ((value >> (byte * 8)) & 0xFFull)) * kFnvPrime;
        }
        return hash;
    }

    constexpr uint64_t schema_version_hash(const char* name, const char* wire_format, uint64_t payload_size) noexcept {
        return fnv1a_mix_u64(fnv1a(wire_format, fnv1a(name)), payload_size);
    }

} // namespace detail

    // Compile-time wire-schema identity for T. Every ShmRing<T> writes
    // Traits<T>::kVersionHash/kName/kWireFormat into the segment header at
    // create() time and checks them again at open() time (shm_ipc.hpp) --
    // this primary template is the fallback used by any T with no
    // ANIMUS_DEFINE_SCHEMA registration: a version hash derived purely from
    // sizeof(T), which still lets two same-schema, unregistered T's attach
    // to each other's segments (the pre-Milestone-1 behavior, preserved),
    // just without the stronger name+format-string collision resistance a
    // registered schema gets.
    template <typename T>
    struct Traits {
        static_assert(std::is_trivially_copyable<T>::value,
            "T must be trivially copyable to carry an animus::schema::Traits<T> wire descriptor -- "
            "ShmRing<T> places T directly in shared memory with no serialization step");
        static constexpr const char* kName = "<unregistered>";
        static constexpr const char* kWireFormat = ""; // no known struct.calcsize-compatible layout for this T
        static constexpr uint64_t kVersionHash =
            detail::schema_version_hash(kName, kWireFormat, static_cast<uint64_t>(sizeof(T)));
    };

    // Ready-made alternate wire schema used to prove ShmRing<T> is
    // genuinely schema-agnostic and not just hardcoded for the 40-byte
    // ExecutionEvent -- see tests/test_dynamic_schema.cpp, which pushes
    // both this and ExecutionEvent across a real process boundary through
    // two independent ShmRing<T> segments. A Level-2 order book update:
    // one price level's bid/ask quote plus enough identity fields
    // (sequence, timestamp, instrument, level) to reconstruct a book from
    // a stream of these. Exactly 64 bytes, no padding -- twice
    // ExecutionEvent's stride, deliberately, so the regression test
    // exercises a genuinely different slot size, not just a different
    // field layout at the same size.
    struct OrderBookL2 {
        uint64_t sequence;        // monotonically increasing update sequence number
        uint64_t dispatch_ts_raw; // raw clock sample at enqueue time (same convention as ExecutionEvent)
        uint32_t instrument_id;   // synthetic instrument identifier
        uint32_t level;           // book depth level this update refers to (0 == top of book)
        int64_t  bid_price_ticks;
        int64_t  ask_price_ticks;
        int64_t  bid_quantity;
        int64_t  ask_quantity;
        uint64_t flags;           // reserved, always 0
    };
    static_assert(sizeof(OrderBookL2) == 64, "OrderBookL2 wire format assumes exactly 64 bytes, no padding");
    static_assert(std::is_trivially_copyable<OrderBookL2>::value, "ShmRing<T> places T directly in shared memory");

} // namespace schema
} // namespace animus

// Registers TypeName as a wire schema: a full specialization of
// animus::schema::Traits<TypeName> naming it, giving its
// struct.calcsize-compatible wire format string, and computing its version
// hash from both plus sizeof(TypeName). Must be invoked at namespace scope
// (it opens namespace animus::schema itself), after TypeName's own
// definition is complete. static_asserts the two hard requirements
// ShmRing<T> itself also enforces (shm_ipc.hpp) -- trivially copyable,
// alignof <= 64 -- redundantly here too, so a schema that could never
// actually be placed in a ShmRing<T> fails to compile at the point it's
// registered, not only wherever it's first instantiated as a ring.
#define ANIMUS_DEFINE_SCHEMA(TypeName, WireFormatString)                                                     \
    namespace animus { namespace schema {                                                                   \
        template <> struct Traits<TypeName> {                                                               \
            static_assert(std::is_trivially_copyable<TypeName>::value,                                      \
                #TypeName " must be trivially copyable -- ShmRing<T> places T directly in shared memory");  \
            static_assert(alignof(TypeName) <= 64,                                                          \
                #TypeName "'s alignment must not exceed 64 bytes for ShmRing<T> placement");                 \
            static constexpr const char* kName = #TypeName;                                                 \
            static constexpr const char* kWireFormat = WireFormatString;                                    \
            static constexpr uint64_t kVersionHash = animus::schema::detail::schema_version_hash(           \
                kName, kWireFormat, static_cast<uint64_t>(sizeof(TypeName)));                                \
        };                                                                                                   \
    } }

// The primary, backward-compatible specialization: ExecutionEvent (40
// bytes, execution_event.hpp) using its existing kExecutionEventWireFormat
// string -- the single source of truth for that format string stays
// execution_event.hpp, this just wires it into the schema registry.
ANIMUS_DEFINE_SCHEMA(animus::ExecutionEvent, animus::kExecutionEventWireFormat)

// The alternate schema defined above.
ANIMUS_DEFINE_SCHEMA(animus::schema::OrderBookL2, "<QQIIqqqqQ")
