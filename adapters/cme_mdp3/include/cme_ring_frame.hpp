#pragma once
// Animus CME MDP 3.0 Adapter -- uniform ring frame.
//
// Same normalize-once-at-the-network-boundary shape adapters/itch50 already
// established with ItchFrame (itch50_ring_frame.hpp): decode a wire message
// exactly once, then carry one fixed-size, cache-line-sized record for
// every book-level update from there on, so nothing downstream of the ring
// (a book-building consumer thread, a Python nanobind reader) needs to walk
// SBE framing, a repeating group, or branch on template_id ever again.
//
// One MdBookUpdateFrame == one MDEntry46 (cme_sbe_messages.hpp) entry from
// a MDIncrementalRefreshBook46's NoMDEntries repeating group, plus the
// root-block and transport-header fields a consumer needs without holding
// onto (or re-parsing) the original packet: TransactTime, the packet's own
// sequence number, and MatchEventIndicator. Building this frame is the one
// place in the pipeline that reads the SBE repeating-group iterator's
// output and copies fields out of it -- everything before this point
// (cme_sbe_messages.hpp's SbeRepeatingGroupView walking the raw recv
// buffer) is pointer arithmetic over borrowed memory, not a copy.
//
// alignas(64) plus the field layout below sums to exactly 64 bytes (one
// cache line) with no compiler-inserted padding -- same reasoning
// itch50_ring_frame.hpp's ItchFrame and animus/schema.hpp's OrderBookL2
// both document: one frame never straddles two cache lines, and an array
// of frames (a ring's backing store) never false-shares between adjacent
// slots.

#include "cme_sbe_messages.hpp"

#include "animus/schema.hpp"

#include <cstdint>
#include <type_traits>

namespace adapters {
namespace cme_mdp3 {

    struct alignas(64) MdBookUpdateFrame {
        uint64_t sequence_id;             // adapter-assigned monotonic ingest sequence (not a wire field) -- verifies ring ordering end to end
        uint64_t recv_timestamp_ns;       // host ingest clock sample, taken immediately after recvfrom() -- NOT transact_time_ns below
        uint64_t transact_time_ns;        // decoded MDIncrementalRefreshBook46::transact_time -- matching-engine event time, nanoseconds since Unix epoch
        int64_t  md_entry_px;             // raw price mantissa (MDEntry46::md_entry_px) -- scale via the instrument's DisplayFactor, never a hardcoded exponent
        uint32_t rpt_seq;                 // MDEntry46::rpt_seq -- per-instrument sequence number, the primary per-symbol gap-detection key
        uint32_t packet_sequence_number;  // PacketHeader::sequence_number this entry's packet carried -- per-channel gap detection / A-B feed arbitration
        int32_t  security_id;             // MDEntry46::security_id -- join key against a decoded MDInstrumentDefinitionFuture27
        int32_t  md_entry_size;           // MDEntry46::md_entry_size -- resting quantity at this level
        int32_t  number_of_orders;        // MDEntry46::number_of_orders -- resting order count at this level
        uint8_t  md_price_level;          // MDEntry46::md_price_level -- 1 == top of book
        uint8_t  md_update_action;        // MDUpdateAction (cme_sbe_messages.hpp)
        uint8_t  md_entry_type;           // MDEntryType ('0' Bid / '1' Offer), carried as its raw ASCII byte -- see decode note below
        uint8_t  match_event_indicator;   // MDIncrementalRefreshBook46::match_event_indicator -- MatchEventIndicatorBits mask, carried per-entry so a consumer never needs the original root block
        uint64_t flags;                   // reserved, always 0
    };
    static_assert(sizeof(MdBookUpdateFrame) == 64, "MdBookUpdateFrame must occupy exactly one cache line");
    static_assert(std::is_trivially_copyable<MdBookUpdateFrame>::value,
        "MdBookUpdateFrame must be trivially copyable -- pushed onto the SPSC ring by plain assignment, no serialization step");
    static_assert(alignof(MdBookUpdateFrame) <= 64,
        "MdBookUpdateFrame's alignment must not exceed 64 bytes -- SpscRingBuffer<T>/ShmRing<T> both pack slots "
        "back-to-back with no per-slot padding beyond T's own size");

    // md_entry_type decode note: stored as the raw ASCII byte
    // (MDEntryType::Bid == '0', MDEntryType::Offer == '1'), not translated
    // to a 0/1 boolean -- keeps this frame decodable without pulling in
    // cme_sbe_messages.hpp's enum on the reader side (a Python consumer,
    // notably, reading raw bytes off a NumPy structured view has no access
    // to that C++ enum at all).

} // namespace cme_mdp3
} // namespace adapters

// Wire-schema registration (Milestone 1, include/animus/schema.hpp) -- see
// itch50_ring_frame.hpp's identical registration of ItchFrame for the full
// rationale. All fourteen fields are fixed-width scalar types (Q/q/I/i/B),
// so wire_format_to_dtype() (animus/dynamic_schema.py) can build a matching
// NumPy structured dtype for this frame at runtime with no compiled
// extension specific to it, the same way it already does for ItchFrame.
ANIMUS_DEFINE_SCHEMA(adapters::cme_mdp3::MdBookUpdateFrame, "<QQQqIIiiiBBBBQ")
