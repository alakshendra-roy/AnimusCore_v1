// Animus CME MDP 3.0 Adapter -- standalone ingestion smoke test.
//
// End-to-end verification of the real production code path -- not a
// function-call simulation: this test opens a genuine UDP receiver socket
// via the same open_udp_receiver()/run_ingest_loop() this adapter's
// standalone binary (cme_ingest.cpp) uses, sends real synthetic CME MDP 3.0
// packets to it over an actual loopback socket (sendto() -> kernel -> the
// receiver's recv path), and verifies every decoded MdBookUpdateFrame that
// comes out the other side of the SPSC ring against a deterministic
// formula the sender encoded into the wire bytes.
//
// LOOPBACK, NOT MULTICAST: binds 127.0.0.1 unicast rather than joining a
// real multicast group, deliberately -- a sandboxed/CI network namespace
// commonly has no multicast-capable interface or IGMP path (see
// cme_udp_socket.hpp's UdpReceiverConfig comment), and this test's job is
// verifying the decode/ring pipeline, not IP_ADD_MEMBERSHIP plumbing.
// open_udp_receiver()'s multicast-join branch is exercised structurally by
// this same call (just not taken, since UdpReceiverConfig::multicast_group
// is left unset below) -- it is NOT independently verified by this test.
//
// SELF-VERIFYING WIRE DATA: every synthetic MDEntry46's fields are pure
// functions of a single global per-entry counter g (0..total_entries-1),
// encoded as that entry's RptSeq -- see build_synthetic_packet(). The
// consumer thread below re-derives the same formulas from each decoded
// frame's own rpt_seq field and compares, which makes verification
// correct regardless of packet arrival order (UDP does not guarantee
// order, even on loopback) -- unlike checking against ring-assigned
// sequence_id, which is arrival-order-dependent by construction.

#include "cme_ingest_loop.hpp"
#include "cme_ring_frame.hpp"
#include "cme_sbe_messages.hpp"
#include "cme_udp_socket.hpp"

#include "animus/thread_affinity.hpp"
#include "spsc_ring_buffer.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

using namespace adapters::cme_mdp3;

namespace {

    // Deterministic per-packet entry count -- shared by the sender and by
    // compute_total_entries() so both agree on the total without the
    // consumer needing to wait on the sender to find out.
    uint32_t entries_in_packet(uint32_t packet_index) noexcept {
        return 1 + (packet_index % 8); // 1..8 entries per packet
    }

    uint64_t compute_total_entries(uint32_t num_packets) noexcept {
        uint64_t total = 0;
        for (uint32_t p = 0; p < num_packets; ++p) total += entries_in_packet(p);
        return total;
    }

    // Encodes one synthetic MDIncrementalRefreshBook46 datagram (PacketHeader
    // + SbeMessageHeader + root block + NoMDEntries group) into `buf`.
    // `global_entry_counter` is threaded through call to call so RptSeq
    // values are globally unique and contiguous across the whole run.
    std::size_t build_synthetic_packet(uint8_t* buf, std::size_t buf_capacity, uint32_t packet_index,
                                        uint64_t& global_entry_counter) noexcept {
        const uint32_t entry_count = entries_in_packet(packet_index);
        const std::size_t needed = sizeof(PacketHeader) + sizeof(SbeMessageHeader) +
                                    sizeof(MDIncrementalRefreshBook46) + sizeof(GroupSizeEncoding) +
                                    static_cast<std::size_t>(entry_count) * sizeof(MDEntry46);
        if (needed > buf_capacity) {
            std::fprintf(stderr, "smoke test: synthetic packet (%zu bytes) exceeds buffer capacity (%zu)\n", needed, buf_capacity);
            std::abort();
        }

        std::size_t off = 0;

        PacketHeader ph{};
        ph.sequence_number = packet_index;
        ph.sending_time = 0; // not checked by this test
        std::memcpy(buf + off, &ph, sizeof(ph)); off += sizeof(ph);

        SbeMessageHeader mh{};
        mh.block_length = sizeof(MDIncrementalRefreshBook46);
        mh.template_id = 46;
        mh.schema_id = 1;
        mh.version = 13;
        std::memcpy(buf + off, &mh, sizeof(mh)); off += sizeof(mh);

        MDIncrementalRefreshBook46 root{};
        root.transact_time = 1'700'000'000'000'000'000ull + packet_index;
        root.match_event_indicator = MatchEventIndicatorBits::kEndOfEvent;
        std::memcpy(buf + off, &root, sizeof(root)); off += sizeof(root);

        GroupSizeEncoding dim{};
        dim.block_length = sizeof(MDEntry46);
        dim.num_in_group = static_cast<uint8_t>(entry_count);
        std::memcpy(buf + off, &dim, sizeof(dim)); off += sizeof(dim);

        for (uint32_t i = 0; i < entry_count; ++i) {
            const uint64_t g = global_entry_counter++;
            MDEntry46 e{};
            e.md_entry_px = static_cast<int64_t>(1'000'000 + g);
            e.md_entry_size = static_cast<int32_t>(100 + (g % 50));
            e.security_id = static_cast<int32_t>(5000 + (g % 4));
            e.rpt_seq = static_cast<uint32_t>(g);
            e.number_of_orders = static_cast<int32_t>(1 + (g % 5));
            e.md_price_level = static_cast<uint8_t>(1 + (g % 10));
            e.md_update_action = static_cast<uint8_t>(g % 3);
            e.md_entry_type = (g % 2) ? static_cast<char>(MDEntryType::Offer) : static_cast<char>(MDEntryType::Bid);
            std::memcpy(buf + off, &e, sizeof(e)); off += sizeof(e);
        }

        return off;
    }

    void send_all_packets(socket_t sock, const sockaddr_in& dest, uint32_t num_packets, uint64_t& total_entries_sent_out) {
        uint8_t buf[1500];
        uint64_t g = 0;
        for (uint32_t p = 0; p < num_packets; ++p) {
            const std::size_t len = build_synthetic_packet(buf, sizeof(buf), p, g);
            sendto(sock, reinterpret_cast<const char*>(buf), static_cast<int>(len), 0,
                   reinterpret_cast<const sockaddr*>(&dest), sizeof(dest));
        }
        total_entries_sent_out = g;
    }

    struct VerifyResult {
        uint64_t frames_consumed = 0;
        uint64_t corruption_count = 0;   // decoded fields didn't match the g-derived formula
        uint64_t duplicate_count = 0;    // same rpt_seq decoded more than once
        uint64_t out_of_range_count = 0; // rpt_seq >= total_entries (can't happen unless something is badly wrong)
    };

    void consumer_thread_fn(animus::eval::SpscRingBuffer<MdBookUpdateFrame>& ring,
                             const std::atomic<bool>& producer_done,
                             uint64_t total_entries,
                             std::vector<uint8_t>& seen,
                             VerifyResult& result_out) {
        VerifyResult r;

        auto verify_one = [&](const MdBookUpdateFrame& f) {
            ++r.frames_consumed;
            const uint64_t g = f.rpt_seq;
            if (g >= total_entries) { ++r.out_of_range_count; return; }
            if (seen[g]) { ++r.duplicate_count; }
            seen[g] = 1;

            const int64_t expected_px = 1'000'000 + static_cast<int64_t>(g);
            const int32_t expected_size = static_cast<int32_t>(100 + (g % 50));
            const int32_t expected_security = static_cast<int32_t>(5000 + (g % 4));
            const int32_t expected_norders = static_cast<int32_t>(1 + (g % 5));
            const uint8_t expected_level = static_cast<uint8_t>(1 + (g % 10));
            const uint8_t expected_action = static_cast<uint8_t>(g % 3);
            const uint8_t expected_type = (g % 2) ? static_cast<uint8_t>('1') : static_cast<uint8_t>('0');

            if (f.md_entry_px != expected_px || f.md_entry_size != expected_size ||
                f.security_id != expected_security || f.number_of_orders != expected_norders ||
                f.md_price_level != expected_level || f.md_update_action != expected_action ||
                f.md_entry_type != expected_type) {
                ++r.corruption_count;
            }
        };

        MdBookUpdateFrame frame{};
        for (;;) {
            if (ring.pop(frame)) {
                verify_one(frame);
                continue;
            }
            if (producer_done.load(std::memory_order_acquire)) {
                if (ring.pop(frame)) { verify_one(frame); continue; }
                break;
            }
            animus::cpu_relax();
        }

        result_out = r;
    }

} // namespace

int main(int argc, char** argv) {
    uint32_t num_packets = 20000;
    if (argc > 1) {
        num_packets = static_cast<uint32_t>(std::strtoul(argv[1], nullptr, 10));
    }

    WinsockGuard winsock;
    if (!winsock.ok()) {
        return 1;
    }

    UdpReceiverConfig cfg;
    cfg.bind_address = "127.0.0.1";
    cfg.port = 47623;
    cfg.multicast_group = std::nullopt; // loopback unicast -- see file header note
    cfg.requested_rcvbuf_bytes = 16 * 1024 * 1024;

    int actual_rcvbuf = 0;
    const socket_t recv_sock = open_udp_receiver(cfg, actual_rcvbuf);
    if (recv_sock == kInvalidSocket) {
        std::fprintf(stderr, "smoke test: failed to open receiver socket\n");
        return 1;
    }

    const socket_t send_sock = open_udp_sender();
    if (send_sock == kInvalidSocket) {
        std::fprintf(stderr, "smoke test: failed to open sender socket\n");
        close_udp_socket(recv_sock);
        return 1;
    }

    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(cfg.port);
    inet_pton(AF_INET, cfg.bind_address.c_str(), &dest.sin_addr);

    const uint64_t total_entries = compute_total_entries(num_packets);
    const std::size_t ring_capacity = 1u << 17; // generous headroom over total_entries for a clean 0-drop run
    animus::eval::SpscRingBuffer<MdBookUpdateFrame> ring(ring_capacity);

    std::atomic<bool> ingest_keep_running{true};
    std::atomic<bool> producer_done{false};
    IngestStats stats;
    std::vector<uint8_t> seen(total_entries, 0);
    VerifyResult verify;

    std::thread ingest_thread(run_ingest_loop, recv_sock, std::ref(ring), std::cref(ingest_keep_running), std::ref(stats));
    std::thread consumer_thread(consumer_thread_fn, std::ref(ring), std::cref(producer_done), total_entries, std::ref(seen), std::ref(verify));

    uint64_t sent_entries = 0;
    const auto t0 = std::chrono::steady_clock::now();
    send_all_packets(send_sock, dest, num_packets, sent_entries);
    // Loopback delivery is effectively immediate, but give the ingest
    // thread a moment to finish draining the last few datagrams off the
    // socket before signaling it to stop.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    ingest_keep_running.store(false, std::memory_order_release);
    ingest_thread.join();
    const auto t1 = std::chrono::steady_clock::now();

    producer_done.store(true, std::memory_order_release);
    consumer_thread.join();

    close_udp_socket(send_sock);
    close_udp_socket(recv_sock);

    uint64_t missing = 0;
    for (uint8_t b : seen) if (!b) ++missing;

    const double elapsed_s = std::chrono::duration<double>(t1 - t0).count();
    const double packets_per_sec = elapsed_s > 0.0 ? static_cast<double>(stats.packets_received) / elapsed_s : 0.0;
    const double entries_per_sec = elapsed_s > 0.0 ? static_cast<double>(verify.frames_consumed) / elapsed_s : 0.0;

    std::printf("================================================================================\n");
    std::printf("      ANIMUS CME MDP 3.0 ADAPTER -- UDP INGESTION SMOKE TEST\n");
    std::printf("================================================================================\n");
    std::printf("  Configuration\n");
    std::printf("  --------------------------------------------------------------------------\n");
    std::printf("  Packets requested             : %u\n", num_packets);
    std::printf("  Entries per packet             : 1..8 (deterministic, packet_index %% 8 + 1)\n");
    std::printf("  Total entries (expected)       : %llu\n", static_cast<unsigned long long>(total_entries));
    std::printf("  SO_RCVBUF (actual/requested)   : %d / %d bytes\n", actual_rcvbuf, cfg.requested_rcvbuf_bytes);
    std::printf("  Ring capacity                  : %zu frames\n", ring_capacity);
    std::printf("\n");
    std::printf("  Receiver-loop stats (production code path -- cme_ingest_loop.hpp)\n");
    std::printf("  --------------------------------------------------------------------------\n");
    std::printf("  packets_received                : %llu\n", static_cast<unsigned long long>(stats.packets_received));
    std::printf("  packets_malformed                : %llu\n", static_cast<unsigned long long>(stats.packets_malformed));
    std::printf("  receive_errors                    : %llu\n", static_cast<unsigned long long>(stats.receive_errors));
    std::printf("  messages_decoded (template 46)   : %llu\n", static_cast<unsigned long long>(stats.messages_decoded_t46));
    std::printf("  messages_skipped (other template): %llu\n", static_cast<unsigned long long>(stats.messages_skipped_other_template));
    std::printf("  entries_pushed                    : %llu\n", static_cast<unsigned long long>(stats.entries_pushed));
    std::printf("  entries_ring_full_dropped        : %llu\n", static_cast<unsigned long long>(stats.entries_ring_full_dropped));
    std::printf("\n");
    std::printf("  Correctness (sender-encoded formula vs. decoded frame)\n");
    std::printf("  --------------------------------------------------------------------------\n");
    std::printf("  entries_sent                      : %llu\n", static_cast<unsigned long long>(sent_entries));
    std::printf("  frames_consumed                   : %llu\n", static_cast<unsigned long long>(verify.frames_consumed));
    std::printf("  missing (never decoded)           : %llu\n", static_cast<unsigned long long>(missing));
    std::printf("  duplicate_count                    : %llu\n", static_cast<unsigned long long>(verify.duplicate_count));
    std::printf("  out_of_range_count                 : %llu\n", static_cast<unsigned long long>(verify.out_of_range_count));
    std::printf("  corruption_count (field mismatch) : %llu\n", static_cast<unsigned long long>(verify.corruption_count));
    std::printf("\n");
    std::printf("  Throughput\n");
    std::printf("  --------------------------------------------------------------------------\n");
    std::printf("  Elapsed (send start -> ingest stop): %.3f s\n", elapsed_s);
    std::printf("  Packet ingest rate                  : %.0f packets/sec\n", packets_per_sec);
    std::printf("  Entry decode+ring rate              : %.0f entries/sec\n", entries_per_sec);
    std::printf("================================================================================\n");

    const bool pass = (sent_entries == total_entries) &&
                       (missing == 0) &&
                       (verify.duplicate_count == 0) &&
                       (verify.out_of_range_count == 0) &&
                       (verify.corruption_count == 0) &&
                       (stats.packets_malformed == 0) &&
                       (stats.messages_skipped_other_template == 0) &&
                       (stats.entries_ring_full_dropped == 0);

    std::printf("RESULT: %s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
