#pragma once
// Animus CME MDP 3.0 Adapter -- cross-platform UDP multicast socket setup.
//
// Isolates every platform-specific socket API call this adapter needs
// behind one small surface (open_udp_receiver() / close_udp_socket() /
// open_udp_sender(), plus the WinsockGuard RAII lifetime helper) so
// cme_ingest_loop.hpp and every binary that includes it stay free of
// #ifdef _WIN32 noise in the actual receive-loop logic.
//
// PORTABILITY NOTE: this repo's available toolchain in this sandbox is
// MinGW-w64 (g++ targeting Windows, x86_64-w64-mingw32) -- there is no
// Linux/BSD toolchain available here to compile-check the POSIX branch
// below against. It is written to the standard, widely-documented Linux
// socket API (setsockopt/fcntl/bind/IP_ADD_MEMBERSHIP) the same way the
// Windows branch is written to and verified against real Winsock2 headers
// in this sandbox -- but build and smoke-test it on your actual Linux
// deployment target before trusting it in production, the same posture
// cme_sbe_messages.hpp's compliance note takes toward exact wire offsets.

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
#else
    #include <arpa/inet.h>
    #include <cerrno>
    #include <cstring>
    #include <fcntl.h>
    #include <netinet/in.h>
    #include <sys/socket.h>
    #include <unistd.h>
#endif

namespace adapters {
namespace cme_mdp3 {

#if defined(_WIN32)
    using socket_t = SOCKET;
    inline constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#else
    using socket_t = int;
    inline constexpr socket_t kInvalidSocket = -1;
#endif

    // RAII Winsock process lifetime. WSAStartup()/WSACleanup() must bracket
    // every Winsock call in a process; every binary in this adapter that
    // touches a socket (cme_ingest.cpp's main(), the smoke test's main())
    // constructs exactly one of these before opening any socket. No-op on
    // POSIX, which has no equivalent per-process init step.
    class WinsockGuard {
    public:
        WinsockGuard() noexcept {
#if defined(_WIN32)
            WSADATA wsa_data;
            ok_ = (WSAStartup(MAKEWORD(2, 2), &wsa_data) == 0);
            if (!ok_) {
                std::fprintf(stderr, "cme_mdp3: WSAStartup failed\n");
            }
#endif
        }
        ~WinsockGuard() {
#if defined(_WIN32)
            if (ok_) {
                WSACleanup();
            }
#endif
        }
        WinsockGuard(const WinsockGuard&) = delete;
        WinsockGuard& operator=(const WinsockGuard&) = delete;

        bool ok() const noexcept {
#if defined(_WIN32)
            return ok_;
#else
            return true;
#endif
        }

    private:
#if defined(_WIN32)
        bool ok_ = false;
#endif
    };

    struct UdpReceiverConfig {
        std::string bind_address = "0.0.0.0"; // local interface to bind for recv; 0.0.0.0 == all interfaces
        uint16_t port = 0;
        // Unset (default) == plain unicast/broadcast bind, no multicast
        // join -- the mode this adapter's own smoke test uses, since a
        // sandboxed/CI network namespace commonly has no multicast-capable
        // interface or IGMP path to a real group. Set to a class-D address
        // (e.g. "239.192.0.1") for a live production CME feed.
        std::optional<std::string> multicast_group;
        std::optional<std::string> multicast_interface; // local interface to join the group on; unset == INADDR_ANY
        int requested_rcvbuf_bytes = 64 * 1024 * 1024; // starting ceiling for set_max_rcvbuf()'s ladder, see below
    };

    // Attempts to set SO_RCVBUF to `requested_bytes`, and on failure backs
    // off through a fixed ladder of smaller candidates rather than giving
    // up outright -- some kernels (notably Linux without CAP_NET_ADMIN,
    // capped by net.core.rmem_max) reject an oversized request rather than
    // silently clamping it, so "ask for the max, fall back until one
    // sticks" gets a working socket on both permissive and restrictive
    // hosts without the caller needing to know which kind it's running on.
    // Returns the ACTUAL buffer size the OS reports via getsockopt() after
    // the accepted call, which is not guaranteed to equal what was
    // requested even on success (many kernels store double the requested
    // value to account for bookkeeping overhead) -- callers should log
    // this value, never assume the request was honored verbatim.
    inline int set_max_rcvbuf(socket_t sock, int requested_bytes) noexcept {
        static constexpr int kLadder[] = {
            64 * 1024 * 1024, 32 * 1024 * 1024, 16 * 1024 * 1024,
            8 * 1024 * 1024, 4 * 1024 * 1024, 2 * 1024 * 1024, 1024 * 1024, 256 * 1024,
        };
        for (int candidate : kLadder) {
            if (candidate > requested_bytes) {
                continue;
            }
#if defined(_WIN32)
            const int rc = setsockopt(sock, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&candidate), sizeof(candidate));
#else
            const int rc = setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &candidate, sizeof(candidate));
#endif
            if (rc == 0) {
                int actual = 0;
#if defined(_WIN32)
                int actual_len = sizeof(actual);
                getsockopt(sock, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<char*>(&actual), &actual_len);
#else
                socklen_t actual_len = sizeof(actual);
                getsockopt(sock, SOL_SOCKET, SO_RCVBUF, &actual, &actual_len);
#endif
                return actual;
            }
        }
        return 0; // every candidate, down to the floor, was rejected
    }

    inline bool set_nonblocking(socket_t sock) noexcept {
#if defined(_WIN32)
        u_long mode = 1;
        return ioctlsocket(sock, FIONBIO, &mode) == 0;
#else
        const int flags = fcntl(sock, F_GETFL, 0);
        if (flags == -1) return false;
        return fcntl(sock, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
    }

    inline void close_udp_socket(socket_t sock) noexcept;

    // Opens a non-blocking UDP socket, binds it per `cfg`, and joins
    // `cfg.multicast_group` if one is set. Returns kInvalidSocket on any
    // failure (a diagnostic is printed to stderr). `actual_rcvbuf_bytes_out`
    // receives set_max_rcvbuf()'s result -- log it, don't assume it equals
    // cfg.requested_rcvbuf_bytes.
    inline socket_t open_udp_receiver(const UdpReceiverConfig& cfg, int& actual_rcvbuf_bytes_out) noexcept {
        actual_rcvbuf_bytes_out = 0;

        const socket_t sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock == kInvalidSocket) {
            std::fprintf(stderr, "cme_mdp3: socket() failed\n");
            return kInvalidSocket;
        }

        // Lets multiple processes (e.g. an A-feed and B-feed handler, or a
        // handler restarting quickly after a crash) bind the same
        // multicast group/port concurrently -- standard practice for a
        // multicast receiver, harmless for the unicast/no-multicast case.
        int reuse = 1;
#if defined(_WIN32)
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
#else
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif

        actual_rcvbuf_bytes_out = set_max_rcvbuf(sock, cfg.requested_rcvbuf_bytes);

        if (!set_nonblocking(sock)) {
            std::fprintf(stderr, "cme_mdp3: failed to set socket non-blocking\n");
            close_udp_socket(sock);
            return kInvalidSocket;
        }

#if !defined(_WIN32)
        // Kernel-stamped arrival time on every recvmsg() -- see
        // cme_ingest_loop.hpp's ArrivalClock/recv_one_packet() POSIX path.
        // No Windows equivalent; the Windows path timestamps in userspace
        // immediately after recvfrom() instead (this header's own
        // portability note above, and the task's explicit design call).
        int enable_ts = 1;
        setsockopt(sock, SOL_SOCKET, SO_TIMESTAMPNS, &enable_ts, sizeof(enable_ts));
#endif

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(cfg.port);
        if (inet_pton(AF_INET, cfg.bind_address.c_str(), &addr.sin_addr) != 1) {
            std::fprintf(stderr, "cme_mdp3: invalid bind address '%s'\n", cfg.bind_address.c_str());
            close_udp_socket(sock);
            return kInvalidSocket;
        }
        if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            std::fprintf(stderr, "cme_mdp3: bind() to %s:%u failed\n", cfg.bind_address.c_str(), cfg.port);
            close_udp_socket(sock);
            return kInvalidSocket;
        }

        if (cfg.multicast_group.has_value()) {
            ip_mreq mreq{};
            if (inet_pton(AF_INET, cfg.multicast_group->c_str(), &mreq.imr_multiaddr) != 1) {
                std::fprintf(stderr, "cme_mdp3: invalid multicast group '%s'\n", cfg.multicast_group->c_str());
                close_udp_socket(sock);
                return kInvalidSocket;
            }
            if (cfg.multicast_interface.has_value()) {
                inet_pton(AF_INET, cfg.multicast_interface->c_str(), &mreq.imr_interface);
            } else {
                mreq.imr_interface.s_addr = htonl(INADDR_ANY);
            }
#if defined(_WIN32)
            const int rc = setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, reinterpret_cast<const char*>(&mreq), sizeof(mreq));
#else
            const int rc = setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));
#endif
            if (rc != 0) {
                std::fprintf(stderr, "cme_mdp3: IP_ADD_MEMBERSHIP for group '%s' failed -- is a multicast-capable "
                                      "interface available? (this is the join a loopback-only sandbox commonly can't satisfy)\n",
                              cfg.multicast_group->c_str());
                close_udp_socket(sock);
                return kInvalidSocket;
            }
        }

        return sock;
    }

    // Plain unconnected UDP socket for sending (the smoke test's synthetic
    // packet sender uses this; a production deployment never sends on the
    // market-data socket, only receives).
    inline socket_t open_udp_sender() noexcept {
        return socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    }

    inline void close_udp_socket(socket_t sock) noexcept {
        if (sock == kInvalidSocket) return;
#if defined(_WIN32)
        closesocket(sock);
#else
        close(sock);
#endif
    }

} // namespace cme_mdp3
} // namespace adapters
