#pragma once
// Animus ITCH 5.0 Adapter -- portable big-endian field extraction.
//
// NASDAQ TotalView-ITCH 5.0 is a big-endian wire protocol; every host this
// adapter is expected to run on (x86_64, ARM64) is little-endian, so every
// multi-byte field needs a byte-order flip on the way in. glibc's own
// be16toh/be32toh/be64toh would do this on Linux, but they don't exist on
// MSVC -- and CLAUDE.md/this adapter's own build matrix requires compiling
// cleanly there too -- so this header provides the same operation as a
// small, portable wrapper over each toolchain's native byte-swap
// intrinsic (__builtin_bswap16/32/64 on GCC/Clang, _byteswap_ushort/ulong/
// uint64 on MSVC) instead of depending on <endian.h>.
//
// Field reads go through memcpy (never a reinterpret_cast<uint32_t*> or
// similar over a #pragma pack(1) struct's field address) so that an
// unaligned wire offset is never read via a misaligned typed pointer --
// technically undefined behavior in the C++ standard even though x86
// tolerates it in practice. A fixed-size memcpy of 2/4/8 bytes is
// recognized by every mainstream compiler at -O2/-O3 (GCC, Clang, MSVC)
// and folded into a single unaligned load, so this costs nothing at
// runtime versus the unsafe direct-cast version -- see each reader below.

#include <cstdint>
#include <cstring>

#if defined(_MSC_VER)
    #include <cstdlib> // _byteswap_ushort / _byteswap_ulong / _byteswap_uint64
#endif

namespace adapters {
namespace itch50 {

    // --- Raw byte-swap primitives -------------------------------------
    // Compiler-intrinsic byte reversal, no loop, no branch -- these lower
    // to a single BSWAP (x86) or REV16/REV (ARM64) instruction under any
    // of GCC, Clang, or MSVC at -O1 and above.
    inline uint16_t bswap16(uint16_t v) noexcept {
#if defined(_MSC_VER)
        return _byteswap_ushort(v);
#else
        return __builtin_bswap16(v);
#endif
    }

    inline uint32_t bswap32(uint32_t v) noexcept {
#if defined(_MSC_VER)
        return _byteswap_ulong(v);
#else
        return __builtin_bswap32(v);
#endif
    }

    inline uint64_t bswap64(uint64_t v) noexcept {
#if defined(_MSC_VER)
        return _byteswap_uint64(v);
#else
        return __builtin_bswap64(v);
#endif
    }

    // --- Wire-order (big-endian) field readers -------------------------
    // `p` must point at exactly sizeof(returned type) readable bytes --
    // every call site below reads from inside a #pragma pack(1) message
    // struct whose field sizes are fixed by the ITCH 5.0 spec, so this is
    // always in-bounds by construction, never a length the caller has to
    // separately validate per field.
    inline uint16_t be16toh_field(const void* p) noexcept {
        uint16_t v;
        std::memcpy(&v, p, sizeof(v));
        return bswap16(v);
    }

    inline uint32_t be32toh_field(const void* p) noexcept {
        uint32_t v;
        std::memcpy(&v, p, sizeof(v));
        return bswap32(v);
    }

    inline uint64_t be64toh_field(const void* p) noexcept {
        uint64_t v;
        std::memcpy(&v, p, sizeof(v));
        return bswap64(v);
    }

    // ITCH 5.0's Timestamp field is a nonstandard 6-byte (48-bit) unsigned
    // big-endian integer -- nanoseconds since midnight -- with no native
    // C++ integer type of that width. Assembled by hand via shifts rather
    // than memcpy-into-a-uint64_t-then-bswap64, since that would require
    // reading 8 bytes from a field that is only 6 wide (an out-of-bounds
    // read at the last Timestamp field in a message with nothing after
    // it). Six shifts and ORs is also branch-free and just as cheap.
    inline uint64_t be48toh_ns(const uint8_t* p) noexcept {
        return (static_cast<uint64_t>(p[0]) << 40) |
               (static_cast<uint64_t>(p[1]) << 32) |
               (static_cast<uint64_t>(p[2]) << 24) |
               (static_cast<uint64_t>(p[3]) << 16) |
               (static_cast<uint64_t>(p[4]) << 8)  |
                static_cast<uint64_t>(p[5]);
    }

    // --- Wire-order (big-endian) field writers --------------------------
    // The inverse of the readers above -- used only by the benchmark
    // harness's synthetic message generator (bench_itch_ingest.cpp) to
    // fabricate realistic big-endian ITCH bytes to then decode. A real
    // deployment never calls these: wire bytes arrive already encoded
    // from the exchange feed.
    inline void put_be16(void* p, uint16_t v) noexcept {
        const uint16_t be = bswap16(v);
        std::memcpy(p, &be, sizeof(be));
    }

    inline void put_be32(void* p, uint32_t v) noexcept {
        const uint32_t be = bswap32(v);
        std::memcpy(p, &be, sizeof(be));
    }

    inline void put_be64(void* p, uint64_t v) noexcept {
        const uint64_t be = bswap64(v);
        std::memcpy(p, &be, sizeof(be));
    }

    inline void put_be48_ns(uint8_t* p, uint64_t ns48) noexcept {
        p[0] = static_cast<uint8_t>((ns48 >> 40) & 0xFF);
        p[1] = static_cast<uint8_t>((ns48 >> 32) & 0xFF);
        p[2] = static_cast<uint8_t>((ns48 >> 24) & 0xFF);
        p[3] = static_cast<uint8_t>((ns48 >> 16) & 0xFF);
        p[4] = static_cast<uint8_t>((ns48 >> 8)  & 0xFF);
        p[5] = static_cast<uint8_t>( ns48         & 0xFF);
    }

} // namespace itch50
} // namespace adapters
