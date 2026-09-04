#pragma once
// Milestone 3: Non-Blocking Telemetry & Observability Layer.
//
// A read-only diagnostic sampler for ShmRing<T> (SPSC) and SpmcRing<T>
// (broadcast) segments (shm_ipc.hpp) that never attaches as a consumer --
// no cursor registration, no pid/heartbeat writes, not even the same-T
// schema check ShmRing<T>::open()/SpmcRing<T>::open() enforce, since a
// monitoring tool has to work for a ring whose exact record type it was
// never compiled against. AnimusGetMetrics() below opens the named
// segment, reads whichever wire-descriptor header it turns out to be
// (RingHeader or SpmcRingHeader -- see RingKind's own comment in
// shm_ipc.hpp for why their leading fields are laid out identically), and
// returns a plain-data TelemetrySnapshot. The mapping is closed again
// before the call returns; nothing about this call is visible to the
// producer or to any real consumer.
//
// Zero locks on the hot path: this header touches no mutex anywhere, and
// every field it samples is already a plain load of a value the producer
// (and, for the SPSC ring, the one consumer) publish via
// std::memory_order_release regardless of whether anyone is watching --
// AnimusGetMetrics() doesn't add any synchronization of its own, so it
// cannot introduce contention on the producer's or consumer's own
// std::atomic<uint64_t> operations no matter how often or how many
// processes call it concurrently.
//
// std::memory_order_relaxed throughout (the explicit constraint this
// milestone calls for): unlike ShmRing<T>::try_pop/SpmcRing<T>::poll,
// nothing here uses a sampled value to gate a subsequent read of the T
// payload array -- there is no slots_[...] access anywhere in this file,
// only numbers read for display -- so there is no acquire/release pairing
// to uphold, and a relaxed load is both correct and cheaper (no memory
// fence, no cross-core cache-coherency stall) than the acquire loads
// ShmRing<T>/SpmcRing<T>'s own consumer-facing accessors use.
#include "shm_ipc.hpp"
#include "shm_lifecycle.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>

namespace animus {

    // Plain-data snapshot of one ring's diagnostic state at the moment
    // AnimusGetMetrics() was called. Deliberately not tied to any T
    // (unlike ShmRing<T>/SpmcRing<T> themselves) -- a monitoring tool
    // samples rings it was never compiled against, so every field here
    // comes from the wire-descriptor header alone, never from the T
    // payload array.
    struct TelemetrySnapshot {
        bool valid = false; // false if the segment couldn't be opened or wasn't a recognized ring header at all

        sys::ipc::RingKind ring_kind = sys::ipc::RingKind::Spsc;
        uint64_t capacity = 0;
        uint64_t schema_version_hash = 0;
        uint64_t payload_size = 0;
        uint64_t stride = 0;
        // Sized for the larger of the two header types' wire_format
        // buffers (currently identical, RingHeader::kWireFormatBufSize ==
        // SpmcRingHeader::kWireFormatBufSize == 80) plus a NUL -- a static
        // assert below catches either buffer ever exceeding this.
        char wire_format[81] = {};

        // current_write_head and total_pushed_events are the SAME
        // underlying counter (ShmRing<T>/SpmcRing<T>'s shared `head` is
        // already a monotonic count of every successful push/broadcast,
        // never reset) -- exposed under both names because this
        // milestone's own field list names them separately, and a reader
        // of a dashboard or a Prometheus scrape may reasonably look for
        // either.
        uint64_t current_write_head = 0;
        uint64_t total_pushed_events = 0;

        // SPSC-only (ShmRing<T>::push_overwrite's dropped_count): an SPMC
        // broadcast ring has no shared drop counter at all -- overruns are
        // a per-consumer, local concept there (see SpmcRing<T>::poll's own
        // doc comment) that no unattached observer could read even in
        // principle, since it depends on which consumer and how far behind
        // it happens to be. has_dropped_count is false, and
        // total_overruns/dropped_events stay 0, for an SpmcBroadcast ring.
        bool has_dropped_count = false;
        uint64_t total_overruns = 0;
        uint64_t dropped_events = 0; // alias of total_overruns, same reasoning as current_write_head/total_pushed_events

        // SPSC-only (ShmRing<T> has exactly one shared tail). An SPMC ring
        // has no shared consumer cursor at all -- each consumer's position
        // lives only in that consumer's own process memory (see
        // SpmcRing<T>'s class comment) -- so consumer lag for a broadcast
        // ring is not observable from the ring itself by any unattached
        // sampler, has_consumer_lag is false, and the two fields stay 0.
        bool has_consumer_lag = false;
        uint64_t consumer_tail = 0;
        uint64_t consumer_lag = 0; // current_write_head - consumer_tail

        uint64_t producer_pid = 0;
        uint64_t producer_heartbeat = 0; // raw monotonic counter from the header (ShmRing<T>::producer_heartbeat()/SpmcRing<T>::producer_heartbeat())
        bool producer_alive = false;     // (producer_pid != 0) && sys::lifecycle::is_process_alive(producer_pid) at sample time

        // This process's own wall-clock timestamp (epoch nanoseconds) of
        // when THIS snapshot was captured -- not a value read from the
        // ring at all. Pairing this with producer_heartbeat across two
        // successive AnimusGetMetrics() calls is how a caller (e.g.
        // scripts/animus_stat.py) detects staleness ("the counter hasn't
        // moved in N wall-clock seconds") without needing any clock
        // synchronization assumption between the producer's and the
        // sampler's processes/cores, which a raw TSC value stored in the
        // ring itself would have required.
        uint64_t sampled_at_epoch_ns = 0;
    };
    static_assert(sizeof(TelemetrySnapshot::wire_format) >= sys::ipc::RingHeader::kWireFormatBufSize + 1,
        "TelemetrySnapshot::wire_format must be big enough for RingHeader's wire_format buffer plus a NUL");
    static_assert(sizeof(TelemetrySnapshot::wire_format) >= sys::ipc::SpmcRingHeader::kWireFormatBufSize + 1,
        "TelemetrySnapshot::wire_format must be big enough for SpmcRingHeader's wire_format buffer plus a NUL");

    namespace detail {
        inline void copy_wire_format_into(char (&dest)[81], const char* src, size_t src_buf_size) noexcept {
            size_t i = 0;
            const size_t limit = src_buf_size < sizeof(TelemetrySnapshot::wire_format) - 1
                ? src_buf_size : sizeof(TelemetrySnapshot::wire_format) - 1;
            for (; i < limit && src[i] != '\0'; ++i) {
                dest[i] = src[i];
            }
            dest[i] = '\0';
        }
    } // namespace detail

    // Samples a named ring segment without attaching as a consumer.
    // Returns true (and fills *out) if `ring_name` names a segment this
    // module recognizes as either kind of animus wire-descriptor header;
    // returns false (and resets *out to its default, invalid state) if the
    // segment doesn't exist, is too small, or its ring_kind field isn't
    // one this module knows how to read. Safe to call from any process,
    // at any rate, concurrently with an arbitrary number of other
    // AnimusGetMetrics() callers, the ring's own producer, and (for an
    // SPSC ring) its one consumer -- see this file's own header comment
    // for why none of that contends with any of this.
    inline bool AnimusGetMetrics(const char* ring_name, TelemetrySnapshot* out) noexcept {
        if (!out) return false;
        *out = TelemetrySnapshot{};
        if (!ring_name) return false;

        sys::ipc::SharedMemoryRegion region;
        if (!sys::ipc::SharedMemoryRegion::open(ring_name, region)) {
            return false;
        }

        // The wire-descriptor prefix (capacity, mask, ring_kind,
        // schema_version_hash, payload_size, stride, wire_format) is
        // byte-identical between RingHeader and SpmcRingHeader by
        // construction (RingKind's own comment, shm_ipc.hpp) -- safe to
        // read ring_kind through either type before knowing which this
        // segment actually is.
        if (region.size() < sizeof(uint64_t) * 3) {
            return false; // too small to even hold capacity/mask/ring_kind
        }
        const auto* prefix = reinterpret_cast<const sys::ipc::RingHeader*>(region.data());
        const uint64_t ring_kind_raw = prefix->ring_kind;

        const auto now_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());

        if (ring_kind_raw == static_cast<uint64_t>(sys::ipc::RingKind::Spsc)) {
            if (region.size() < sizeof(sys::ipc::RingHeader)) return false;
            const auto* header = reinterpret_cast<const sys::ipc::RingHeader*>(region.data());

            out->ring_kind = sys::ipc::RingKind::Spsc;
            out->capacity = header->capacity;
            out->schema_version_hash = header->schema_version_hash;
            out->payload_size = header->payload_size;
            out->stride = header->stride;
            detail::copy_wire_format_into(out->wire_format, header->wire_format, sys::ipc::RingHeader::kWireFormatBufSize);

            const uint64_t head = header->head.load(std::memory_order_relaxed);
            const uint64_t tail = header->tail.load(std::memory_order_relaxed);
            out->current_write_head = head;
            out->total_pushed_events = head;
            out->has_dropped_count = true;
            out->total_overruns = header->dropped_count.load(std::memory_order_relaxed);
            out->dropped_events = out->total_overruns;
            out->has_consumer_lag = true;
            out->consumer_tail = tail;
            out->consumer_lag = (head >= tail) ? (head - tail) : 0; // defensive only -- head < tail should never happen
            out->producer_pid = header->producer_pid.load(std::memory_order_relaxed);
            out->producer_heartbeat = header->producer_heartbeat.load(std::memory_order_relaxed);
        } else if (ring_kind_raw == static_cast<uint64_t>(sys::ipc::RingKind::SpmcBroadcast)) {
            if (region.size() < sizeof(sys::ipc::SpmcRingHeader)) return false;
            const auto* header = reinterpret_cast<const sys::ipc::SpmcRingHeader*>(region.data());

            out->ring_kind = sys::ipc::RingKind::SpmcBroadcast;
            out->capacity = header->capacity;
            out->schema_version_hash = header->schema_version_hash;
            out->payload_size = header->payload_size;
            out->stride = header->stride;
            detail::copy_wire_format_into(out->wire_format, header->wire_format, sys::ipc::SpmcRingHeader::kWireFormatBufSize);

            const uint64_t head = header->head.load(std::memory_order_relaxed);
            out->current_write_head = head;
            out->total_pushed_events = head;
            // has_dropped_count / has_consumer_lag stay false -- see
            // TelemetrySnapshot's own field comments.
            out->producer_pid = header->producer_pid.load(std::memory_order_relaxed);
            out->producer_heartbeat = header->producer_heartbeat.load(std::memory_order_relaxed);
        } else {
            return false; // not a ring_kind this module recognizes -- an unrelated or foreign segment
        }

        out->producer_alive = (out->producer_pid != 0) && sys::lifecycle::is_process_alive(out->producer_pid);
        out->sampled_at_epoch_ns = now_ns;
        out->valid = true;
        return true;
    }

} // namespace animus
