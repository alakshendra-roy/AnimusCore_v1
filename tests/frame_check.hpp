#pragma once
// Self-verifying frames for the MPMC tests (tests/test_shm_mpmc.cpp,
// tests/soak_shm_mpmc.cpp).
//
// A frame is a real animus::ExecutionEvent -- the same 40-byte record the
// production fast path carries -- whose fields are a pure function of
// (producer, sequence):
//
//   sequence         per-producer counter 0..N-1
//   instrument_id    the producer's index
//   price_ticks      mix64 of (producer, sequence)
//   quantity         mix64 of (producer, sequence, price_ticks)  <- checksum
//   dispatch_ts_raw  the producer's cycle-counter stamp at ingress (soak only)
//
// so a consumer needs no side channel to know what a correct frame looks like:
// a torn read (fields from two different writes), a stale slot, a bit flip, or
// a frame attributed to the wrong producer all fail frame_ok(). Exactly-once
// delivery is checked separately with a per-(producer, sequence) bitmap.
#include "../include/animus/execution_event.hpp"

#include <cstddef>
#include <cstdint>

namespace animus_test {

inline uint64_t mix64(uint64_t x) noexcept {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

inline int64_t price_for(uint32_t producer, uint64_t seq) noexcept {
    return static_cast<int64_t>(mix64((seq * 0x2545F4914F6CDD1Dull) ^ producer) >> 1);
}

inline int64_t checksum_for(uint32_t producer, uint64_t seq, int64_t price) noexcept {
    return static_cast<int64_t>(
        mix64((static_cast<uint64_t>(producer) << 40) ^ seq ^ static_cast<uint64_t>(price)) >> 1);
}

inline animus::ExecutionEvent make_frame(uint32_t producer, uint64_t seq, uint64_t ingress_stamp = 0) noexcept {
    animus::ExecutionEvent e;
    e.sequence = seq;
    e.dispatch_ts_raw = ingress_stamp;
    e.price_ticks = price_for(producer, seq);
    e.quantity = checksum_for(producer, seq, e.price_ticks);
    e.instrument_id = producer;
    e.flags = 0;
    return e;
}

// True iff every derived field matches what (instrument_id, sequence) imply.
inline bool frame_ok(const animus::ExecutionEvent& e, uint32_t producer_count) noexcept {
    return e.instrument_id < producer_count &&
           e.flags == 0 &&
           e.price_ticks == price_for(e.instrument_id, e.sequence) &&
           e.quantity == checksum_for(e.instrument_id, e.sequence, e.price_ticks);
}

inline unsigned popcount64(uint64_t v) noexcept {
    v = v - ((v >> 1) & 0x5555555555555555ull);
    v = (v & 0x3333333333333333ull) + ((v >> 2) & 0x3333333333333333ull);
    v = (v + (v >> 4)) & 0x0F0F0F0F0F0F0F0Full;
    return static_cast<unsigned>((v * 0x0101010101010101ull) >> 56);
}

} // namespace animus_test
