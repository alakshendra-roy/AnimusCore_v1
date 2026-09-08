#pragma once
// Animus ITCH 5.0 Adapter -- zero-allocation wire decoder.
//
// decode() below is the entire hot path this adapter exists to prove out:
// given a raw wire buffer holding exactly one ITCH message, parse it into
// the uniform ItchFrame (itch50_ring_frame.hpp) with no heap allocation --
// every intermediate value lives in a local (register/stack) variable, out
// is written by reference, and nothing here calls new/malloc or touches a
// container that could reallocate. Byte-order conversion goes through
// itch50_bswap.hpp's intrinsic-backed readers, never a direct
// reinterpret_cast over the #pragma pack(1) wire structs.
//
// Returns false (out left in an unspecified state) for a message shorter
// than its type demands, or an unrecognized message_type byte -- the
// caller's job is deciding what to do with a rejected message (this
// header never throws or aborts on malformed input, since a malformed or
// truncated frame is an expected, recoverable condition on a live feed,
// not a programming error).

#include "itch50_bswap.hpp"
#include "itch50_messages.hpp"
#include "itch50_ring_frame.hpp"

#include <cstddef>
#include <cstring>

namespace adapters {
namespace itch50 {

    namespace detail {
        // Shared by every message type: StockLocate + Timestamp sit at the
        // identical offset (bytes 1..8) in all eight wire messages, right
        // after the one-byte MessageType every caller has already read to
        // dispatch here.
        inline void decode_common_header(const uint8_t* wire, ItchFrame& out) noexcept {
            out.stock_locate    = be16toh_field(wire + 1);
            out.itch_timestamp_ns = be48toh_ns(wire + 5);
        }
    } // namespace detail

    // Decodes exactly one ITCH message starting at wire[0]. `wire_len`
    // must be the number of valid bytes available starting there (the
    // caller's framing/demux layer -- MoldUDP64 payload length, a
    // length-prefixed TCP replay file, etc. -- is what supplies this; this
    // adapter has no opinion on transport). `out.sequence_id` and
    // `out.recv_timestamp_ns` are NOT set here -- both are ingest-pipeline
    // concerns the caller (bench_itch_ingest.cpp's producer loop) owns,
    // not something a stateless per-message decode call can supply.
    inline bool decode(const uint8_t* wire, std::size_t wire_len, ItchFrame& out) noexcept {
        if (wire == nullptr || wire_len == 0) return false;

        // Zero every field not written by the branch below, so every
        // message type leaves a fully-defined ItchFrame (no uninitialized
        // reads downstream) without every branch having to repeat the
        // same boilerplate.
        out.order_ref_number = 0;
        out.secondary_ref_number = 0;
        out.stock_ticker_raw = 0;
        out.price_ticks = 0;
        out.shares = 0;
        out.side_or_flag = 0;

        const auto type = static_cast<MessageType>(wire[0]);
        out.msg_type = wire[0];

        switch (type) {
        case MessageType::SystemEvent: {
            if (wire_len < sizeof(SystemEventMsg)) return false;
            detail::decode_common_header(wire, out);
            out.side_or_flag = wire[11]; // EventCode
            return true;
        }
        case MessageType::AddOrderNoMPID: {
            if (wire_len < sizeof(AddOrderMsg)) return false;
            detail::decode_common_header(wire, out);
            out.order_ref_number = be64toh_field(wire + 11);
            out.side_or_flag     = wire[19]; // BuySellIndicator
            out.shares            = be32toh_field(wire + 20);
            std::memcpy(&out.stock_ticker_raw, wire + 24, 8); // opaque ASCII blob -- see ItchFrame's own decode note
            out.price_ticks       = static_cast<int64_t>(be32toh_field(wire + 32));
            return true;
        }
        case MessageType::AddOrderWithMPID: {
            if (wire_len < sizeof(AddOrderMPIDMsg)) return false;
            detail::decode_common_header(wire, out);
            out.order_ref_number = be64toh_field(wire + 11);
            out.side_or_flag     = wire[19];
            out.shares            = be32toh_field(wire + 20);
            std::memcpy(&out.stock_ticker_raw, wire + 24, 8);
            out.price_ticks       = static_cast<int64_t>(be32toh_field(wire + 32));
            // Attribution (MPID, wire[36..40)) is intentionally not carried
            // into ItchFrame -- see itch50_ring_frame.hpp's scope note.
            return true;
        }
        case MessageType::OrderExecuted: {
            if (wire_len < sizeof(OrderExecutedMsg)) return false;
            detail::decode_common_header(wire, out);
            out.order_ref_number     = be64toh_field(wire + 11);
            out.shares                 = be32toh_field(wire + 19);
            out.secondary_ref_number = be64toh_field(wire + 23); // MatchNumber
            return true;
        }
        case MessageType::OrderExecutedWithPrice: {
            if (wire_len < sizeof(OrderExecutedWithPriceMsg)) return false;
            detail::decode_common_header(wire, out);
            out.order_ref_number     = be64toh_field(wire + 11);
            out.shares                 = be32toh_field(wire + 19);
            out.secondary_ref_number = be64toh_field(wire + 23); // MatchNumber
            out.side_or_flag          = wire[31]; // Printable
            out.price_ticks           = static_cast<int64_t>(be32toh_field(wire + 32));
            return true;
        }
        case MessageType::OrderCancel: {
            if (wire_len < sizeof(OrderCancelMsg)) return false;
            detail::decode_common_header(wire, out);
            out.order_ref_number = be64toh_field(wire + 11);
            out.shares             = be32toh_field(wire + 19); // CancelledShares
            return true;
        }
        case MessageType::OrderDelete: {
            if (wire_len < sizeof(OrderDeleteMsg)) return false;
            detail::decode_common_header(wire, out);
            out.order_ref_number = be64toh_field(wire + 11);
            return true;
        }
        case MessageType::OrderReplace: {
            if (wire_len < sizeof(OrderReplaceMsg)) return false;
            detail::decode_common_header(wire, out);
            out.order_ref_number     = be64toh_field(wire + 11);  // OriginalOrderReferenceNumber
            out.secondary_ref_number = be64toh_field(wire + 19);  // NewOrderReferenceNumber
            out.shares                 = be32toh_field(wire + 27);
            out.price_ticks           = static_cast<int64_t>(be32toh_field(wire + 31));
            return true;
        }
        default:
            return false; // unrecognized message_type -- not one of the eight this adapter decodes
        }
    }

} // namespace itch50
} // namespace adapters
