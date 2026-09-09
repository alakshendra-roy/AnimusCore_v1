#pragma once
// Animus CME MDP 3.0 Adapter -- zero-copy ingestion loop.
//
// The production decode path: one UDP datagram in, zero or more
// MdBookUpdateFrame records pushed onto the SPSC ring, zero heap
// allocations anywhere in between. Every step from recv_one_packet()
// through decode_packet_into_ring() operates on pointers into a single
// caller-owned stack buffer (run_ingest_loop()'s `buf`) -- no copy of the
// wire bytes is made until a decoded MDEntry46's fields are written into
// the MdBookUpdateFrame that gets pushed, the same one-unavoidable-copy
// accounting adapters/itch50/README.md documents for ItchFrame.
//
// BACKPRESSURE POLICY -- deliberate, not an oversight: if the ring is full,
// decode_packet_into_ring() drops the entry and counts it
// (IngestStats::entries_ring_full_dropped) rather than spinning until
// space frees up. On a live multicast feed, this thread's only job is
// draining the kernel's socket receive buffer as fast as datagrams arrive;
// blocking it for even a handful of microseconds risks that kernel buffer
// overflowing, which is real, unrecoverable packet loss with no sequence
// number to even detect it by. A full downstream ring, by contrast, is
// visible (RptSeq/PacketHeader::sequence_number gaps on the consumer side)
// and recoverable (CME's snapshot/recovery feed). Never trade a detectable,
// recoverable problem for an invisible, unrecoverable one -- so this loop
// always keeps recv()ing, never blocks on ring space.
//
// This differs deliberately from adapters/itch50/bench_itch_ingest.cpp's
// producer, which spin-retries push() until it succeeds -- correct there
// because that harness is driving the ring from an in-process generator
// with no kernel receive buffer behind it to overflow; wrong here.

#include "cme_ring_frame.hpp"
#include "cme_sbe_messages.hpp"
#include "cme_udp_socket.hpp"

#include "animus/thread_affinity.hpp"
#include "spsc_ring_buffer.hpp"

#include <atomic>
#include <cstdint>
#include <cstring>

#if defined(_WIN32)
    #include <windows.h>
#else
    #include <cerrno>
    #include <ctime>
    #include <sys/socket.h>
    #include <sys/types.h>
#endif

namespace adapters {
namespace cme_mdp3 {

    struct IngestStats {
        uint64_t packets_received = 0;
        uint64_t packets_malformed = 0;             // failed a bounds/length sanity check -- see decode_packet_into_ring()
        uint64_t receive_errors = 0;                 // a hard socket error on one recv call (not EWOULDBLOCK)
        uint64_t messages_decoded_t46 = 0;           // MDIncrementalRefreshBook46 messages fully walked
        uint64_t messages_skipped_other_template = 0; // any other template_id -- see the note at its use site below
        uint64_t entries_pushed = 0;
        uint64_t entries_ring_full_dropped = 0;      // see this file's backpressure policy note above
    };

    // Arrival-timestamp clock, platform-forked per this adapter's design:
    //   - POSIX: recv_one_packet() below reads the kernel-attached
    //     SO_TIMESTAMPNS control message on every recvmsg() call (enabled
    //     on the socket by cme_udp_socket.hpp's open_udp_receiver()) and
    //     only falls back to this clock if that control message is absent.
    //   - Windows: no kernel-level UDP arrival timestamp facility
    //     comparable to SO_TIMESTAMPNS ships with stock Winsock, so this
    //     clock IS the timestamp source -- sampled via QueryPerformanceCounter
    //     immediately after recvfrom() returns, per this adapter's stated
    //     design. That means the Windows measurement includes whatever
    //     userspace scheduling latency elapsed between the kernel
    //     completing the receive and this thread resuming to sample the
    //     counter -- typically small, but a real, non-zero gap versus the
    //     POSIX path's kernel-side stamp. Don't compare absolute latency
    //     numbers captured on the two platforms without accounting for it.
#if defined(_WIN32)
    class ArrivalClock {
    public:
        ArrivalClock() noexcept {
            LARGE_INTEGER freq;
            QueryPerformanceFrequency(&freq);
            ns_per_tick_ = 1e9 / static_cast<double>(freq.QuadPart);
        }
        uint64_t now_ns() const noexcept {
            LARGE_INTEGER counter;
            QueryPerformanceCounter(&counter);
            return static_cast<uint64_t>(static_cast<double>(counter.QuadPart) * ns_per_tick_);
        }
    private:
        double ns_per_tick_;
    };
#else
    class ArrivalClock {
    public:
        uint64_t now_ns() const noexcept {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ull + static_cast<uint64_t>(ts.tv_nsec);
        }
    };
#endif

    enum class RecvResult { kOk, kWouldBlock, kError };

    // Receives exactly one UDP datagram into `buf` (capacity
    // `buf_capacity`). On kOk, `len_out`/`arrival_ts_ns_out` are valid;
    // otherwise both are left at 0. kWouldBlock means "no datagram ready
    // right now" (EWOULDBLOCK/EAGAIN/WSAEWOULDBLOCK) -- expected and
    // frequent on a non-blocking socket, not an error condition.
    inline RecvResult recv_one_packet(socket_t sock, uint8_t* buf, std::size_t buf_capacity,
                                       std::size_t& len_out, uint64_t& arrival_ts_ns_out,
                                       const ArrivalClock& clock) noexcept {
        len_out = 0;
        arrival_ts_ns_out = 0;

#if defined(_WIN32)
        sockaddr_in from{};
        int from_len = sizeof(from);
        const int n = recvfrom(sock, reinterpret_cast<char*>(buf), static_cast<int>(buf_capacity), 0,
                                reinterpret_cast<sockaddr*>(&from), &from_len);
        if (n == SOCKET_ERROR) {
            return (WSAGetLastError() == WSAEWOULDBLOCK) ? RecvResult::kWouldBlock : RecvResult::kError;
        }
        // Sampled immediately after recvfrom() returns -- see this file's
        // ArrivalClock comment above for why this is the timestamp source
        // on Windows, not merely a fallback.
        arrival_ts_ns_out = clock.now_ns();
        len_out = static_cast<std::size_t>(n);
        return RecvResult::kOk;
#else
        alignas(struct cmsghdr) uint8_t control[CMSG_SPACE(sizeof(struct timespec))];
        sockaddr_in from{};
        struct iovec iov;
        iov.iov_base = buf;
        iov.iov_len = buf_capacity;
        struct msghdr msg{};
        msg.msg_name = &from;
        msg.msg_namelen = sizeof(from);
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = control;
        msg.msg_controllen = sizeof(control);

        const ssize_t n = recvmsg(sock, &msg, 0);
        if (n < 0) {
            return (errno == EWOULDBLOCK || errno == EAGAIN) ? RecvResult::kWouldBlock : RecvResult::kError;
        }
        len_out = static_cast<std::size_t>(n);

        for (struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg); cmsg != nullptr; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
            if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_TIMESTAMPNS) {
                struct timespec ts;
                std::memcpy(&ts, CMSG_DATA(cmsg), sizeof(ts));
                arrival_ts_ns_out = static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ull + static_cast<uint64_t>(ts.tv_nsec);
                return RecvResult::kOk;
            }
        }
        // No SO_TIMESTAMPNS control message came back (driver/kernel
        // didn't attach one) -- fall back to a userspace sample, same
        // accuracy caveat as the Windows path above.
        arrival_ts_ns_out = clock.now_ns();
        return RecvResult::kOk;
#endif
    }

    // Decodes one raw UDP datagram (starting at its PacketHeader) and
    // pushes one MdBookUpdateFrame per MDEntry46 it contains onto `ring`.
    // Every bounds check below exists because `buf` is untrusted network
    // input -- a truncated, corrupted, or adversarial datagram must be
    // rejected (counted in stats, never read past its own length), not
    // trusted to be well-formed just because it arrived on the expected
    // multicast group/port.
    inline void decode_packet_into_ring(const uint8_t* buf, std::size_t len, uint64_t arrival_ts_ns,
                                         animus::eval::SpscRingBuffer<MdBookUpdateFrame>& ring,
                                         uint64_t& next_frame_sequence,
                                         IngestStats& stats) noexcept {
        if (len < sizeof(PacketHeader)) {
            ++stats.packets_malformed;
            return;
        }
        PacketHeader packet_header;
        std::memcpy(&packet_header, buf, sizeof(packet_header));

        std::size_t offset = sizeof(PacketHeader);
        // CME may bundle more than one SBE message into a single datagram
        // (e.g. several book-update events from the same matching-engine
        // burst) -- walk all of them, not just the first.
        while (offset + sizeof(SbeMessageHeader) <= len) {
            SbeMessageHeader mh;
            std::memcpy(&mh, buf + offset, sizeof(mh));
            const std::size_t root_start = offset + sizeof(SbeMessageHeader);

            if (root_start + mh.block_length > len) {
                ++stats.packets_malformed; // declared root block runs past the datagram
                return;
            }

            if (mh.template_id != 46) {
                // Can't safely find the next message's start without
                // fully decoding this template's length -- a template
                // other than 46 may carry its own repeating groups this
                // adapter doesn't map (MDInstrumentDefinitionFuture27's
                // scope note, cme_sbe_messages.hpp), so root_start +
                // mh.block_length is only that template's ROOT block end,
                // not necessarily the whole message's end. Stop parsing
                // this datagram here rather than guess and misread
                // whatever bytes follow as a bogus SbeMessageHeader.
                ++stats.messages_skipped_other_template;
                return;
            }

            if (mh.block_length < sizeof(MDIncrementalRefreshBook46)) {
                ++stats.packets_malformed; // narrower than the fields this adapter reads
                return;
            }
            MDIncrementalRefreshBook46 root;
            std::memcpy(&root, buf + root_start, sizeof(root));

            const std::size_t group_start = root_start + mh.block_length; // wire block_length, not sizeof(root) -- see cme_sbe_messages.hpp
            if (group_start + sizeof(GroupSizeEncoding) > len) {
                ++stats.packets_malformed;
                return;
            }

            MDEntryGroupView view(buf + group_start);
            if (view.wire_block_length() < sizeof(MDEntry46)) {
                ++stats.packets_malformed; // entries narrower than the fields this adapter reads
                return;
            }
            if (group_start + view.byte_length() > len) {
                ++stats.packets_malformed; // declared entry count/width runs past the datagram
                return;
            }

            ++stats.messages_decoded_t46;
            for (const MDEntry46& entry : view) {
                MdBookUpdateFrame frame{};
                frame.sequence_id = next_frame_sequence++;
                frame.recv_timestamp_ns = arrival_ts_ns;
                frame.transact_time_ns = root.transact_time;
                frame.md_entry_px = entry.md_entry_px;
                frame.rpt_seq = entry.rpt_seq;
                frame.packet_sequence_number = packet_header.sequence_number;
                frame.security_id = entry.security_id;
                frame.md_entry_size = entry.md_entry_size;
                frame.number_of_orders = entry.number_of_orders;
                frame.md_price_level = entry.md_price_level;
                frame.md_update_action = entry.md_update_action;
                frame.md_entry_type = static_cast<uint8_t>(entry.md_entry_type);
                frame.match_event_indicator = root.match_event_indicator;
                frame.flags = 0;

                if (ring.push(frame)) {
                    ++stats.entries_pushed;
                } else {
                    ++stats.entries_ring_full_dropped; // see this file's backpressure policy note above -- never spin here
                }
            }

            offset = group_start + view.byte_length();
        }
    }

    // Drains `sock` until `keep_running` clears. Never allocates from the
    // heap: `buf` is a fixed-size stack array sized for MDP 3.0's
    // documented maximum UDP payload; a deployment sending jumbo frames
    // must widen kMaxDatagramSize accordingly (a too-small buffer would
    // silently truncate a legitimate oversized datagram, which
    // decode_packet_into_ring()'s bounds checks would then correctly
    // reject as malformed rather than crash on -- safe, but a real message
    // gets dropped, so size this generously for your actual deployment).
    inline void run_ingest_loop(socket_t sock,
                                 animus::eval::SpscRingBuffer<MdBookUpdateFrame>& ring,
                                 const std::atomic<bool>& keep_running,
                                 IngestStats& stats) noexcept {
        static constexpr std::size_t kMaxDatagramSize = 1500; // standard Ethernet MTU; widen for a jumbo-frame deployment
        uint8_t buf[kMaxDatagramSize];
        uint64_t next_frame_sequence = 0;
        const ArrivalClock clock;

        while (keep_running.load(std::memory_order_acquire)) {
            std::size_t len = 0;
            uint64_t arrival_ts_ns = 0;
            const RecvResult result = recv_one_packet(sock, buf, sizeof(buf), len, arrival_ts_ns, clock);

            if (result == RecvResult::kWouldBlock) {
                animus::cpu_relax(); // nothing to read right now -- spin, never sleep (see this file's backpressure policy note)
                continue;
            }
            if (result == RecvResult::kError) {
                ++stats.receive_errors; // one bad recv must not stop draining the rest of the feed
                continue;
            }

            ++stats.packets_received;
            decode_packet_into_ring(buf, len, arrival_ts_ns, ring, next_frame_sequence, stats);
        }
    }

} // namespace cme_mdp3
} // namespace adapters
