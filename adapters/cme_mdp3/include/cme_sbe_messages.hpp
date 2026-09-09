#pragma once
// Animus CME MDP 3.0 Adapter -- SBE wire framing & zero-copy repeating-group access.
//
// CME MDP 3.0 encodes every message with SBE (Simple Binary Encoding,
// FIX/FIX Trading Community standard), wrapped in CME's own UDP-multicast
// packet framing. This header defines the three framing layers a decoder
// walks through, outermost first, plus the fixed root-block layout for two
// concrete message templates (46 = MDIncrementalRefreshBook,
// 27 = MDInstrumentDefinitionFuture) and a generic, allocation-free view
// type for reading SBE repeating groups directly off the wire.
//
//   UDP payload
//     -> PacketHeader                (12B, CME transport framing)
//     -> { SbeMessageHeader (8B) + message body } , repeated             <- 1+ SBE messages per packet
//          -> fixed root block (template-specific fields)
//          -> 0+ repeating groups, each: GroupSizeEncoding (3B) + N fixed-size entries
//          -> 0+ variable-length fields (not used by templates 46/27 below)
//
// COMPLIANCE NOTE -- read before pointing this at a production feed: field
// order, byte widths, and enum values below are drawn from public
// documentation of the MDP 3.0 SBE wire architecture and are believed
// structurally correct, but CME revises exact blockLengths and appends
// fields to both templates between schema versions. This is an
// architecture/evaluation-grade reference decoder, not a vendor-certified
// one -- verify every offset in this file against your own copy of CME's
// published MDP3.0.xml messageSchema for the schema version your feed
// handler is pinned to before trading on it. Two things make that
// verification safe to do incrementally rather than all-or-nothing:
//
//   1. SBE's extension rule is additive-only: a later schema version may
//      append new fields to the end of an existing block or add new
//      trailing repeating groups, but never reorders or removes fields a
//      decoder already depends on. So a struct below that only maps a
//      *prefix* of a real block's fields still decodes those fields
//      correctly against any schema version at or above the one it was
//      written for -- it just doesn't see the newer trailing fields yet.
//   2. This file's repeating-group iterator (SbeRepeatingGroupView, below)
//      steps entries by the wire's own declared blockLength, never by
//      sizeof(EntryT). A schema bump that widens an entry is transparent
//      to iteration; only the specific fields you haven't mapped yet are
//      invisible until you extend the struct.
//
// Every struct here is #pragma pack(1). Two independent reasons, not one:
//   - sizeof(...) must match the wire's byte width exactly (no compiler
//     padding), the same reason itch50_messages.hpp packs its structs.
//   - Unlike ITCH's structs (which are only ever read field-by-field
//     through itch50_bswap.hpp, never overlaid directly), the structs here
//     ARE overlaid directly on raw UDP payload bytes at arbitrary,
//     wire-determined offsets via reinterpret_cast (see
//     SbeRepeatingGroupView::iterator below) -- offsets that have no
//     reason to satisfy a naturally-aligned type's alignment requirement
//     (e.g. an 8-byte price field can land at an odd byte offset). Packing
//     every struct to alignof 1 makes that overlay well-defined: the
//     compiler must already emit unaligned-safe loads/stores for any
//     access to a member of an alignof(1) type, on every target this repo
//     builds for (MSVC and GCC, x86_64/ARM64). A naturally-aligned struct
//     reinterpret_cast onto the same bytes would be undefined behavior the
//     moment the wire offset didn't happen to be a multiple of the
//     member's alignment.
//
// SBE is little-endian on the wire (unlike ITCH, which is big-endian) and
// x86_64/ARM64 (this repo's targets) are little-endian hosts, so -- unlike
// itch50_bswap.hpp -- no byte-swap step is needed anywhere in this file.
// If this adapter is ever ported to a big-endian host, every multi-byte
// field access through these structs needs the same swap treatment ITCH's
// fields get today.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace adapters {
namespace cme_mdp3 {

#pragma pack(push, 1)

    // ---------------------------------------------------------------
    // Layer 1: CME's transport-level packet header. Precedes every UDP
    // multicast datagram on both the A and B feeds; wraps one or more SBE
    // messages (each with its own SbeMessageHeader below). Not part of the
    // SBE standard itself -- this is CME-specific framing.
    // ---------------------------------------------------------------
    struct PacketHeader {
        uint32_t sequence_number;  // per-channel monotonic sequence, used for gap detection/A-B arbitration
        uint64_t sending_time;     // CME matching-engine send time, nanoseconds since Unix epoch
    };
    static_assert(sizeof(PacketHeader) == 12, "PacketHeader must be exactly 12 bytes per MDP 3.0 binary packet framing");

    // ---------------------------------------------------------------
    // Layer 2: standard SBE message header. Precedes every individual SBE
    // message body within a packet (a packet may carry several, back to
    // back, when CME bundles multiple book updates into one datagram).
    // ---------------------------------------------------------------
    struct SbeMessageHeader {
        uint16_t block_length;  // size in bytes of this message's fixed root block (NOT including repeating groups/var-length fields)
        uint16_t template_id;   // message type -- 46 = MDIncrementalRefreshBook, 27 = MDInstrumentDefinitionFuture, etc.
        uint16_t schema_id;     // MDP 3.0 schema namespace identifier
        uint16_t version;       // schema version this message was encoded against -- see the compliance note above
    };
    static_assert(sizeof(SbeMessageHeader) == 8, "SbeMessageHeader must be exactly 8 bytes per the SBE standard");

    // Total bytes to skip from the start of an SBE message to reach the
    // first byte after its root block (where a repeating group's
    // GroupSizeEncoding, a further repeating group, or the next message's
    // SbeMessageHeader begins) -- root header size plus the header's own
    // *wire-declared* block_length, deliberately not sizeof(RootT): see
    // the compliance note's point 2 on why this must track the wire value.
    inline std::size_t root_block_end_offset(const SbeMessageHeader& header) noexcept {
        return sizeof(SbeMessageHeader) + header.block_length;
    }

    // ---------------------------------------------------------------
    // SBE repeating-group dimension header. Precedes every repeating
    // group's entries (e.g. MDIncrementalRefreshBook's NoMDEntries).
    // ---------------------------------------------------------------
    struct GroupSizeEncoding {
        uint16_t block_length;   // size in bytes of ONE entry in this group, per the wire -- see SbeRepeatingGroupView
        uint8_t  num_in_group;   // entry count
    };
    static_assert(sizeof(GroupSizeEncoding) == 3, "GroupSizeEncoding must be exactly 3 bytes per the SBE standard");

    // ---------------------------------------------------------------
    // Template 46 -- MDIncrementalRefreshBook. Root block: 9 bytes, one
    // TransactTime plus a bitmask describing what else is bundled with
    // this message. The book-level updates themselves are the NoMDEntries
    // repeating group that follows this root block (see MDEntry below).
    // ---------------------------------------------------------------

    // MatchEventIndicator bitmask (root block field, both templates 46 and
    // 27 carry one). Bits, low to high per the wire: LastTradeMsg,
    // LastQuoteMsg, LastStatsMsg, LastImpliedMsg, RecoveryMsg,
    // Reserved, EndOfEvent. Kept as raw bit constants rather than an enum
    // class since callers test bits independently, not a single value.
    namespace MatchEventIndicatorBits {
        inline constexpr uint8_t kLastTradeMsg  = 0x01;
        inline constexpr uint8_t kLastQuoteMsg  = 0x02;
        inline constexpr uint8_t kLastStatsMsg  = 0x04;
        inline constexpr uint8_t kLastImpliedMsg = 0x08;
        inline constexpr uint8_t kRecoveryMsg   = 0x10;
        inline constexpr uint8_t kReserved      = 0x20;
        inline constexpr uint8_t kEndOfEvent    = 0x40; // last message of this matching-engine event within the packet
    }

    struct MDIncrementalRefreshBook46 {
        uint64_t transact_time;         // matching-engine event time, nanoseconds since Unix epoch
        uint8_t  match_event_indicator; // MatchEventIndicatorBits mask above
    };
    static_assert(sizeof(MDIncrementalRefreshBook46) == 9, "MDIncrementalRefreshBook46 root block must be 9 bytes");

    // MDUpdateAction enum (one per book entry, below). Values per the
    // MDP 3.0 schema's MDUpdateActionEnum.
    enum class MDUpdateAction : uint8_t {
        New        = 0,
        Change     = 1,
        Delete     = 2,
        DeleteThru = 3, // delete this price level and every level worse than it
        DeleteFrom = 4, // delete this price level and every level better than it
        Overlay    = 5,
    };

    // MDEntryType (ASCII, FIX-style) -- which book side/kind this entry is.
    // MDIncrementalRefreshBook carries outright bid/offer entries; implied
    // and other entry types exist on other MDP 3.0 templates and are out
    // of scope here.
    enum class MDEntryType : char {
        Bid   = '0',
        Offer = '1',
    };

    // One NoMDEntries repeating-group entry -- a single price-level update.
    // MDEntryPx is carried as a raw mantissa (CME's MDP3.0 Price9/PRICE
    // type): the scaling exponent is instrument-dependent (see the
    // instrument definition's DisplayFactor, MDInstrumentDefinitionFuture27
    // below) and is intentionally NOT hardcoded here -- multiplying by a
    // fixed exponent guess would silently misprice every instrument whose
    // tick convention differs from whatever guess was baked in.
    struct MDEntry46 {
        int64_t  md_entry_px;      // price mantissa; scale via the instrument's DisplayFactor, never a hardcoded exponent
        int32_t  md_entry_size;    // resting quantity at this level
        int32_t  security_id;      // CME SecurityID -- join key against MDInstrumentDefinitionFuture27::security_id
        uint32_t rpt_seq;          // per-instrument sequence number -- gap detection independent of PacketHeader::sequence_number
        int32_t  number_of_orders; // resting order count at this level
        uint8_t  md_price_level;   // 1 = top of book
        uint8_t  md_update_action; // MDUpdateAction
        char     md_entry_type;    // MDEntryType
    };
    static_assert(sizeof(MDEntry46) == 27, "MDEntry46 fixed block, as mapped here, must be 27 bytes");
    static_assert(std::is_trivially_copyable<MDEntry46>::value,
        "MDEntry46 must be trivially copyable -- overlaid directly on raw UDP payload bytes, no deserialization step");

    // ---------------------------------------------------------------
    // Template 27 -- MDInstrumentDefinitionFuture.
    //
    // SCOPE: the real MDInstrumentDefinitionFuture is one of the largest
    // messages in the MDP 3.0 schema (100+ root fields plus several
    // repeating groups: Events, InstrumentAttribute, Underlying,
    // MDFeedTypes, and more). Mapping all of it correctly requires the
    // exact XML for the schema version in use -- guessing the rest here
    // would mean shipping fabricated field offsets in a header whose
    // stated job is memory-safe wire decoding, which is worse than
    // mapping less and saying so. This struct covers the identity and
    // price-scaling fields a consumer needs first (join key, symbol,
    // tick/price-scale conversion, trading status) and stops there. The
    // four repeating groups above are explicitly NOT covered by this
    // draft. Extending this struct is additive-only and safe to do
    // incrementally -- see the compliance note at the top of this file.
    // ---------------------------------------------------------------
    struct MDInstrumentDefinitionFuture27 {
        uint8_t  match_event_indicator;   // MatchEventIndicatorBits mask
        char     security_update_action;  // 'A' Add / 'D' Delete / 'M' Modify
        uint64_t last_update_time;        // nanoseconds since Unix epoch
        int32_t  security_id;             // join key against MDEntry46::security_id
        uint16_t md_security_trading_status; // SecurityTradingStatusEnum -- see MDSecurityTradingStatus below
        char     symbol[20];              // right-padded with NUL, not spaces (differs from ITCH's space-padding convention)
        char     security_group[6];       // product code, e.g. "ES"
        char     asset[6];
        char     security_exchange[4];    // MIC, e.g. "XCME"
        char     currency[3];
        uint16_t maturity_month_year_year;  // MaturityMonthYear composite, decoded to plain fields (raw wire type is a 5-byte packed composite)
        uint8_t  maturity_month_year_month;
        uint8_t  maturity_month_year_day;   // 0 if not a weekly/daily-dated instrument
        double   display_factor;          // multiply md_entry_px's raw mantissa by this to get a real price
        int64_t  min_price_increment;     // smallest legal price mantissa increment, same raw units as MDEntry46::md_entry_px
    };
    static_assert(sizeof(MDInstrumentDefinitionFuture27) == 75,
        "MDInstrumentDefinitionFuture27 subset, as mapped here, must be 75 bytes");
    static_assert(std::is_trivially_copyable<MDInstrumentDefinitionFuture27>::value,
        "MDInstrumentDefinitionFuture27 must be trivially copyable -- overlaid directly on raw UDP payload bytes");

    // A representative subset of MDSecurityTradingStatus values a consumer
    // branches on immediately (halts must stop a book builder from acting
    // on stale levels). Not exhaustive -- see the schema for the full
    // enumeration.
    enum class MDSecurityTradingStatus : uint16_t {
        TradingHalt      = 2,
        Close             = 4,
        NewPriceIndication = 15,
        NotAvailableForTrading = 19,
        ReadyToTrade      = 17,
        PreOpen           = 21,
    };

#pragma pack(pop)

    // ---------------------------------------------------------------
    // Zero-copy, zero-allocation repeating-group view.
    //
    // Construct directly over the raw payload bytes at a group's
    // GroupSizeEncoding header (immediately after a message's root block,
    // per root_block_end_offset() above). No copy is made of the entries
    // themselves; begin()/end() hand back pointers computed by walking
    // GroupSizeEncoding::block_length-sized strides over the caller's own
    // buffer, which must outlive this view (it borrows, the same
    // discipline every other zero-copy structure in this codebase follows
    // -- compare animus::sys::ipc::ShmRing<T>'s slot views).
    //
    // Stepping by the wire's declared block_length rather than
    // sizeof(EntryT) is the load-bearing detail here: EntryT is this
    // adapter's own idea of "the fields we understand," which may be a
    // strict prefix of what a newer schema version actually sends per
    // entry (see the compliance note at the top of this file). Iteration
    // stays correct either way; only decode() of fields beyond EntryT's
    // last mapped member would need updating.
    // ---------------------------------------------------------------
    template <typename EntryT>
    class SbeRepeatingGroupView {
    public:
        static_assert(std::is_trivially_copyable<EntryT>::value,
            "SbeRepeatingGroupView<EntryT> overlays EntryT directly on raw wire bytes -- EntryT must be trivially copyable");

        // `group_start` must point at the first byte of this group's
        // GroupSizeEncoding header.
        explicit SbeRepeatingGroupView(const uint8_t* group_start) noexcept {
            std::memcpy(&dimension_, group_start, sizeof(GroupSizeEncoding));
            first_entry_ = group_start + sizeof(GroupSizeEncoding);
        }

        uint32_t entry_count() const noexcept { return dimension_.num_in_group; }
        uint16_t wire_block_length() const noexcept { return dimension_.block_length; }

        // Total bytes this group occupies on the wire (header + every
        // entry, using the wire's own block_length) -- what a caller adds
        // to this group's start offset to find whatever follows it
        // (another repeating group, or the end of the message).
        std::size_t byte_length() const noexcept {
            return sizeof(GroupSizeEncoding) +
                   static_cast<std::size_t>(dimension_.num_in_group) * dimension_.block_length;
        }

        class iterator {
        public:
            iterator(const uint8_t* ptr, uint16_t stride) noexcept : ptr_(ptr), stride_(stride) {}

            const EntryT& operator*() const noexcept {
                // Well-defined despite `ptr_` having no particular
                // alignment relative to alignof(EntryT): EntryT is
                // #pragma pack(1) (alignof 1) for exactly this reason --
                // see the file-level comment above.
                return *reinterpret_cast<const EntryT*>(ptr_);
            }

            iterator& operator++() noexcept { ptr_ += stride_; return *this; }
            bool operator!=(const iterator& other) const noexcept { return ptr_ != other.ptr_; }

        private:
            const uint8_t* ptr_;
            uint16_t stride_;
        };

        iterator begin() const noexcept { return iterator(first_entry_, dimension_.block_length); }
        iterator end() const noexcept {
            return iterator(first_entry_ + static_cast<std::size_t>(dimension_.num_in_group) * dimension_.block_length,
                             dimension_.block_length);
        }

    private:
        GroupSizeEncoding dimension_;
        const uint8_t*    first_entry_;
    };

    // Convenience alias for the one repeating group templates 46/27 above
    // are decoded through in this draft (MDIncrementalRefreshBook's
    // NoMDEntries). A caller decoding MDInstrumentDefinitionFuture27's
    // as-yet-unmapped repeating groups would instantiate
    // SbeRepeatingGroupView<T> against its own EntryT once those are
    // mapped.
    using MDEntryGroupView = SbeRepeatingGroupView<MDEntry46>;

} // namespace cme_mdp3
} // namespace adapters
