#pragma once
// Animus ITCH 5.0 Adapter -- wire message layouts.
//
// Structurally faithful #pragma pack(1) definitions of the eight core
// NASDAQ TotalView-ITCH 5.0 message types this adapter ingests, laid out
// per the publicly documented ITCH 5.0 message specification (field order
// and byte widths below; excludes the outer SoupBinTCP session-layer
// framing -- the 2-byte packet length prefix and session header that wrap
// every ITCH message in a live multicast/MoldUDP64 feed are a transport
// concern, not part of the ITCH message body itself, and are out of scope
// for this adapter). Verify field offsets against your own copy of
// Nasdaq's official ITCH 5.0 specification before pointing this at a
// production feed -- this header is an evaluation/benchmark-grade
// reference decoder, not a vendor-certified one.
//
// Every multi-byte integer field is declared as a `uint8_t[N]` byte array,
// deliberately never as a `uint16_t`/`uint32_t`/`uint64_t` directly. Two
// reasons:
//   1. Every such field is big-endian on the wire; giving it a native
//      integer type would invite exactly one bug -- someone reads it
//      directly as a host-order value further down the line and gets
//      silently wrong results on every little-endian target this adapter
//      runs on. Declaring it as bytes forces every access through this
//      adapter's own itch50_bswap.hpp readers, which do the flip.
//   2. ITCH's Timestamp field is 6 bytes (48 bits) -- there is no native
//      C++ integer type of that width to declare it as in the first place.
//
// #pragma pack(push, 1) removes all compiler-inserted inter-field padding,
// so sizeof(...) below matches the exact wire length the ITCH 5.0 spec
// documents for each message type -- verified by the static_asserts that
// follow each struct.

#include <cstdint>
#include <type_traits>

namespace adapters {
namespace itch50 {

    // The eight core message types this adapter decodes, tagged by their
    // one-byte ITCH MessageType field (ASCII, matches the wire byte
    // exactly -- no translation table needed to dispatch on it).
    enum class MessageType : uint8_t {
        SystemEvent            = 'S',
        AddOrderNoMPID          = 'A',
        AddOrderWithMPID        = 'F',
        OrderExecuted           = 'E',
        OrderExecutedWithPrice  = 'C',
        OrderCancel             = 'X',
        OrderDelete             = 'D',
        OrderReplace            = 'U',
    };

#pragma pack(push, 1)

    // 'S' -- System Event Message (12 bytes). Marks feed-wide lifecycle
    // events (start/end of messages, start/end of system hours, start/end
    // of market hours, etc.) via a single ASCII EventCode.
    struct SystemEventMsg {
        uint8_t message_type;          // 'S'
        uint8_t stock_locate[2];
        uint8_t tracking_number[2];
        uint8_t timestamp[6];          // nanoseconds since midnight, 48-bit big-endian
        uint8_t event_code;            // 'O','S','Q','M','E','C','A','R'
    };
    static_assert(sizeof(SystemEventMsg) == 12, "SystemEventMsg must be exactly 12 bytes per ITCH 5.0");

    // 'A' -- Add Order (No MPID Attribution) (36 bytes). A new visible,
    // anonymous limit order enters the book.
    struct AddOrderMsg {
        uint8_t message_type;          // 'A'
        uint8_t stock_locate[2];
        uint8_t tracking_number[2];
        uint8_t timestamp[6];
        uint8_t order_reference_number[8];
        uint8_t buy_sell_indicator;    // 'B' or 'S'
        uint8_t shares[4];
        char    stock[8];              // right-padded with spaces, not NUL-terminated
        uint8_t price[4];              // fixed-point, 4 implied decimal digits
    };
    static_assert(sizeof(AddOrderMsg) == 36, "AddOrderMsg must be exactly 36 bytes per ITCH 5.0");

    // 'F' -- Add Order (with MPID Attribution) (40 bytes). Same as 'A'
    // plus a 4-character market participant identifier for attributed
    // (non-anonymous) orders.
    struct AddOrderMPIDMsg {
        uint8_t message_type;          // 'F'
        uint8_t stock_locate[2];
        uint8_t tracking_number[2];
        uint8_t timestamp[6];
        uint8_t order_reference_number[8];
        uint8_t buy_sell_indicator;
        uint8_t shares[4];
        char    stock[8];
        uint8_t price[4];
        char    attribution[4];        // MPID, right-padded with spaces
    };
    static_assert(sizeof(AddOrderMPIDMsg) == 40, "AddOrderMPIDMsg must be exactly 40 bytes per ITCH 5.0");

    // 'E' -- Order Executed (31 bytes). Full or partial execution against
    // resting liquidity at the order's own displayed price.
    struct OrderExecutedMsg {
        uint8_t message_type;          // 'E'
        uint8_t stock_locate[2];
        uint8_t tracking_number[2];
        uint8_t timestamp[6];
        uint8_t order_reference_number[8];
        uint8_t executed_shares[4];
        uint8_t match_number[8];
    };
    static_assert(sizeof(OrderExecutedMsg) == 31, "OrderExecutedMsg must be exactly 31 bytes per ITCH 5.0");

    // 'C' -- Order Executed With Price (36 bytes). Same as 'E', for the
    // less common case of an execution away from the order's displayed
    // price (e.g. sub-penny or cross execution); carries its own price
    // and a Printable flag (whether the trade should be reflected in the
    // public tape).
    struct OrderExecutedWithPriceMsg {
        uint8_t message_type;          // 'C'
        uint8_t stock_locate[2];
        uint8_t tracking_number[2];
        uint8_t timestamp[6];
        uint8_t order_reference_number[8];
        uint8_t executed_shares[4];
        uint8_t match_number[8];
        uint8_t printable;             // 'Y' or 'N'
        uint8_t execution_price[4];
    };
    static_assert(sizeof(OrderExecutedWithPriceMsg) == 36, "OrderExecutedWithPriceMsg must be exactly 36 bytes per ITCH 5.0");

    // 'X' -- Order Cancel (23 bytes). Partial reduction of a resting
    // order's remaining size without fully removing it from the book.
    struct OrderCancelMsg {
        uint8_t message_type;          // 'X'
        uint8_t stock_locate[2];
        uint8_t tracking_number[2];
        uint8_t timestamp[6];
        uint8_t order_reference_number[8];
        uint8_t cancelled_shares[4];
    };
    static_assert(sizeof(OrderCancelMsg) == 23, "OrderCancelMsg must be exactly 23 bytes per ITCH 5.0");

    // 'D' -- Order Delete (19 bytes). Full removal of a resting order from
    // the book (no remaining size).
    struct OrderDeleteMsg {
        uint8_t message_type;          // 'D'
        uint8_t stock_locate[2];
        uint8_t tracking_number[2];
        uint8_t timestamp[6];
        uint8_t order_reference_number[8];
    };
    static_assert(sizeof(OrderDeleteMsg) == 19, "OrderDeleteMsg must be exactly 19 bytes per ITCH 5.0");

    // 'U' -- Order Replace (35 bytes). Atomically deletes the referenced
    // order and inserts a new one at a (possibly) new price/size, under a
    // new order reference number -- the standard "modify" operation, since
    // ITCH otherwise has no in-place mutate message.
    struct OrderReplaceMsg {
        uint8_t message_type;          // 'U'
        uint8_t stock_locate[2];
        uint8_t tracking_number[2];
        uint8_t timestamp[6];
        uint8_t original_order_reference_number[8];
        uint8_t new_order_reference_number[8];
        uint8_t shares[4];
        uint8_t price[4];
    };
    static_assert(sizeof(OrderReplaceMsg) == 35, "OrderReplaceMsg must be exactly 35 bytes per ITCH 5.0");

#pragma pack(pop)

    static_assert(std::is_trivially_copyable<SystemEventMsg>::value &&
                  std::is_trivially_copyable<AddOrderMsg>::value &&
                  std::is_trivially_copyable<AddOrderMPIDMsg>::value &&
                  std::is_trivially_copyable<OrderExecutedMsg>::value &&
                  std::is_trivially_copyable<OrderExecutedWithPriceMsg>::value &&
                  std::is_trivially_copyable<OrderCancelMsg>::value &&
                  std::is_trivially_copyable<OrderDeleteMsg>::value &&
                  std::is_trivially_copyable<OrderReplaceMsg>::value,
                  "every ITCH wire message must be trivially copyable -- these are read directly "
                  "off a raw byte buffer with no deserialization step");

    // Largest of the eight wire messages above (AddOrderMPIDMsg, 40 bytes)
    // -- the fixed stack-buffer size the benchmark harness's synthetic
    // generator and any fixed-size wire read buffer needs to hold any one
    // of them.
    inline constexpr std::size_t kMaxWireMessageSize = sizeof(AddOrderMPIDMsg);

} // namespace itch50
} // namespace adapters
