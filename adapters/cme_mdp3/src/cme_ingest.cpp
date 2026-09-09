// Animus CME MDP 3.0 Adapter -- standalone ingestion binary (cme_ingest).
//
// Owns a UDP receiver socket (adapters/cme_mdp3/include/cme_udp_socket.hpp),
// runs the zero-copy decode loop (cme_ingest_loop.hpp) on the calling
// thread, and drains the resulting animus::eval::SpscRingBuffer<MdBookUpdateFrame>
// on a second thread standing in for a real downstream consumer (a book
// builder, or a Python nanobind reader attached via the schema this frame
// registers -- see cme_ring_frame.hpp). This binary's own scope stops at
// "decode + enqueue"; the consumer thread here exists only so the ring has
// somewhere to drain to when this is run stand-alone.
//
// Default mode (no --group given) binds a plain unicast/broadcast UDP
// socket -- deliberately, since a real CME multicast group requires a
// multicast-capable network path this binary can't assume exists on
// whatever host it's first tried on. Pass --group/--iface to join a real
// production multicast feed; see cme_udp_socket.hpp's UdpReceiverConfig.

#include "cme_ingest_loop.hpp"
#include "cme_ring_frame.hpp"
#include "cme_udp_socket.hpp"

#include "animus/thread_affinity.hpp"
#include "spsc_ring_buffer.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <thread>

#include <csignal>

using adapters::cme_mdp3::IngestStats;
using adapters::cme_mdp3::MdBookUpdateFrame;
using adapters::cme_mdp3::UdpReceiverConfig;
using adapters::cme_mdp3::WinsockGuard;
using adapters::cme_mdp3::socket_t;

namespace {

    // Ctrl+C/SIGINT clears this instead of terminating the process outright,
    // so the ingest loop and consumer thread both get a chance to drain and
    // report final stats rather than being killed mid-packet.
    std::atomic<bool> g_keep_running{true};

    void handle_sigint(int) {
        g_keep_running.store(false, std::memory_order_release);
    }

    struct CliConfig {
        std::string bind_address = "0.0.0.0";
        uint16_t port = 14310; // placeholder -- a real deployment's port comes from CME's connectivity/session docs for the channel in question
        std::optional<std::string> multicast_group;
        std::optional<std::string> multicast_interface;
        int rcvbuf_bytes = 64 * 1024 * 1024;
        std::size_t ring_capacity = 1u << 16;
    };

    void print_usage(const char* argv0) {
        std::fprintf(stderr,
            "Usage: %s [--bind <addr>] [--port <n>] [--group <mcast-addr>] [--iface <addr>] "
            "[--rcvbuf <bytes>] [--ring-capacity <slots>]\n"
            "  --bind <addr>          Local interface to bind (default: 0.0.0.0)\n"
            "  --port <n>             UDP port (default: 14310)\n"
            "  --group <mcast-addr>   Multicast group to join; omit for plain unicast bind (default: unset)\n"
            "  --iface <addr>         Local interface to join the multicast group on (default: INADDR_ANY)\n"
            "  --rcvbuf <bytes>       Requested SO_RCVBUF ceiling (default: 67108864)\n"
            "  --ring-capacity <n>    SPSC ring capacity in frames, rounded up to a power of two (default: 65536)\n",
            argv0);
    }

    bool parse_args(int argc, char** argv, CliConfig& cfg) {
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            auto next = [&]() -> const char* {
                if (i + 1 >= argc) { std::fprintf(stderr, "error: %s requires a value\n", arg.c_str()); return nullptr; }
                return argv[++i];
            };
            if (arg == "--bind") {
                const char* v = next(); if (!v) return false; cfg.bind_address = v;
            } else if (arg == "--port") {
                const char* v = next(); if (!v) return false; cfg.port = static_cast<uint16_t>(std::strtoul(v, nullptr, 10));
            } else if (arg == "--group") {
                const char* v = next(); if (!v) return false; cfg.multicast_group = v;
            } else if (arg == "--iface") {
                const char* v = next(); if (!v) return false; cfg.multicast_interface = v;
            } else if (arg == "--rcvbuf") {
                const char* v = next(); if (!v) return false; cfg.rcvbuf_bytes = std::atoi(v);
            } else if (arg == "--ring-capacity") {
                const char* v = next(); if (!v) return false; cfg.ring_capacity = static_cast<std::size_t>(std::strtoull(v, nullptr, 10));
            } else if (arg == "--help" || arg == "-h") {
                print_usage(argv[0]);
                std::exit(0);
            } else {
                std::fprintf(stderr, "error: unrecognized argument '%s'\n", argv[i]);
                return false;
            }
        }
        return true;
    }

    // Stands in for a real downstream consumer (a book builder, or a
    // Python nanobind reader over the same ring's schema-registered
    // frame). Drains until told to stop AND the ring reports empty, so no
    // frame the producer already pushed is lost to a premature exit.
    void consumer_thread_fn(animus::eval::SpscRingBuffer<MdBookUpdateFrame>& ring,
                             const std::atomic<bool>& keep_running,
                             uint64_t& frames_consumed_out) {
        MdBookUpdateFrame frame{};
        uint64_t consumed = 0;
        for (;;) {
            if (ring.pop(frame)) {
                ++consumed;
                continue;
            }
            if (!keep_running.load(std::memory_order_acquire)) {
                if (ring.pop(frame)) { ++consumed; continue; }
                break;
            }
            animus::cpu_relax();
        }
        frames_consumed_out = consumed;
    }

} // namespace

int main(int argc, char** argv) {
    CliConfig cfg;
    if (!parse_args(argc, argv, cfg)) {
        print_usage(argv[0]);
        return 1;
    }

    std::signal(SIGINT, handle_sigint);

    WinsockGuard winsock;
    if (!winsock.ok()) {
        return 1;
    }

    UdpReceiverConfig sock_cfg;
    sock_cfg.bind_address = cfg.bind_address;
    sock_cfg.port = cfg.port;
    sock_cfg.multicast_group = cfg.multicast_group;
    sock_cfg.multicast_interface = cfg.multicast_interface;
    sock_cfg.requested_rcvbuf_bytes = cfg.rcvbuf_bytes;

    int actual_rcvbuf = 0;
    const socket_t sock = adapters::cme_mdp3::open_udp_receiver(sock_cfg, actual_rcvbuf);
    if (sock == adapters::cme_mdp3::kInvalidSocket) {
        return 1;
    }

    std::fprintf(stderr, "cme_ingest: listening on %s:%u%s%s -- SO_RCVBUF=%d bytes (requested %d)\n",
                 cfg.bind_address.c_str(), cfg.port,
                 cfg.multicast_group ? ", multicast group " : "",
                 cfg.multicast_group ? cfg.multicast_group->c_str() : "",
                 actual_rcvbuf, cfg.rcvbuf_bytes);

    animus::eval::SpscRingBuffer<MdBookUpdateFrame> ring(cfg.ring_capacity);
    IngestStats stats;
    uint64_t frames_consumed = 0;

    std::thread consumer(consumer_thread_fn, std::ref(ring), std::cref(g_keep_running), std::ref(frames_consumed));

    adapters::cme_mdp3::run_ingest_loop(sock, ring, g_keep_running, stats);

    consumer.join();
    adapters::cme_mdp3::close_udp_socket(sock);

    std::fprintf(stderr,
        "cme_ingest: stopped.\n"
        "  packets_received                = %llu\n"
        "  packets_malformed                = %llu\n"
        "  receive_errors                   = %llu\n"
        "  messages_decoded (template 46)   = %llu\n"
        "  messages_skipped (other template)= %llu\n"
        "  entries_pushed                   = %llu\n"
        "  entries_ring_full_dropped        = %llu\n"
        "  frames_consumed                  = %llu\n",
        static_cast<unsigned long long>(stats.packets_received),
        static_cast<unsigned long long>(stats.packets_malformed),
        static_cast<unsigned long long>(stats.receive_errors),
        static_cast<unsigned long long>(stats.messages_decoded_t46),
        static_cast<unsigned long long>(stats.messages_skipped_other_template),
        static_cast<unsigned long long>(stats.entries_pushed),
        static_cast<unsigned long long>(stats.entries_ring_full_dropped),
        static_cast<unsigned long long>(frames_consumed));

    return 0;
}
