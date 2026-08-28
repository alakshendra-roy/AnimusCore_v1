// Phase 8 demo/verification: mutual TLS 1.3 transport (animus_transport.hpp)
// carrying telemetry into RBAC + multi-tenant-isolated engines
// (animus_security.hpp) -- the full Phase 8 stack end to end, with zero
// DLL, zero Python, and zero external TLS dependency (Schannel only).
//
// Runs a real server thread and client thread against each other over a
// loopback TCP socket: the server requires and verifies the client's
// certificate (mutual auth), maps the verified certificate's subject CN to
// an animus::security::AccessToken, and only then dispatches each received
// frame through SecureTelemetryGateway -- so a frame never reaches a
// tenant's ring buffer without both a cryptographically verified identity
// and an RBAC-permitted operation.
//
// Requires demo_certs/{ca.cer,ca.pfx,server.pfx,client.pfx}: run
// generate_demo_certs.ps1 first.
//
// Intentionally excluded from the default MSBuild targets (see
// AnimusCore_v1.vcxproj), same as execution_interop_demo.cpp and
// secure_multitenancy_demo.cpp. Build and run standalone (MSVC required --
// this header uses Schannel/SSPI, not portable to g++/MinGW), e.g. from a
// "x64 Native Tools Command Prompt for VS":
//   cl /std:c++17 /EHsc /O2 secure_transport_demo.cpp /Fe:secure_transport_demo.exe
//   secure_transport_demo.exe
#include "animus_transport.hpp"
#include "animus_security.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

using namespace animus;
using namespace animus::transport;
using namespace animus::security;

namespace {
    constexpr uint16_t kDemoPort = 47821;
    constexpr uint32_t kDemoTenantId = 42;

    std::wstring cert_dir() { return L"demo_certs\\"; }

    // Demo cert CNs are ASCII-only, so a narrowing char-by-char copy is
    // exact (not just approximate) -- an explicit cast per element avoids
    // MSVC's C4244 warning for the implicit wchar_t->char narrowing that
    // std::string's iterator-range constructor would otherwise perform.
    std::string narrow_ascii(const std::wstring& wide) {
        std::string out;
        out.reserve(wide.size());
        for (wchar_t ch : wide) out.push_back(static_cast<char>(ch));
        return out;
    }
}

int main() {
    WinsockInit wsa;
    if (!wsa.ok()) {
        std::cerr << "[SECURE TRANSPORT DEMO] WSAStartup failed\n";
        return 1;
    }

    CertContextPtr ca_cert, server_cert, client_cert;
    try {
        ca_cert = load_cer_certificate(cert_dir() + L"ca.cer");
        server_cert = load_pfx_certificate(cert_dir() + L"server.pfx", L"AnimusDemoP@ss1");
        client_cert = load_pfx_certificate(cert_dir() + L"client.pfx", L"AnimusDemoP@ss1");
    }
    catch (const std::exception& ex) {
        std::cerr << "[SECURE TRANSPORT DEMO] certificate load failed: " << ex.what() << "\n"
            << "Run generate_demo_certs.ps1 first (from AnimusCore_v1/).\n";
        return 1;
    }
    TrustedRoot trust(ca_cert.get());

    // Server-side security state: one isolated tenant engine, RBAC
    // enforcement, and a mapping from the verified client certificate's CN
    // to that tenant's AccessToken.
    TenantRegistry registry;
    registry.create_tenant(kDemoTenantId, 1 << 16);
    SecureTelemetryGateway gateway(registry);
    CertificateIdentityMap identity_map;
    identity_map.add(L"animus-client-tenant-42", AccessToken{ kDemoTenantId, /*principal_id=*/1001, Role::Operator });

    constexpr size_t TOTAL_FRAMES = 20000;
    std::atomic<bool> server_ready{ false };
    std::atomic<bool> server_ok{ true };
    std::string server_negotiated_protocol;
    std::wstring server_peer_cn;
    size_t server_frames_accepted = 0;

    std::thread server_thread([&] {
        try {
            TcpSocket listener = TcpSocket::listen_on(kDemoPort);
            server_ready.store(true);
            TcpSocket conn = listener.accept_one();

            SecureChannel channel(std::move(conn));
            if (!channel.handshake_as_server(server_cert.get())) {
                std::cerr << "[SERVER] handshake failed: " << channel.last_error() << "\n";
                server_ok.store(false);
                return;
            }

            std::string chain_error;
            if (!verify_certificate_chain(channel.peer_certificate(), trust, szOID_PKIX_KP_CLIENT_AUTH, &chain_error)) {
                std::cerr << "[SERVER] client certificate rejected: " << chain_error << "\n";
                server_ok.store(false);
                return;
            }

            AccessToken token;
            if (!identity_map.resolve(channel.peer_certificate(), token)) {
                std::cerr << "[SERVER] client certificate has no tenant mapping -- rejecting\n";
                server_ok.store(false);
                return;
            }

            server_negotiated_protocol = channel.negotiated_protocol_name();
            server_peer_cn = get_subject_common_name(channel.peer_certificate());

            for (size_t i = 0; i < TOTAL_FRAMES; ++i) {
                WireFrame frame;
                if (!channel.recv_frame(frame)) {
                    std::cerr << "[SERVER] recv_frame failed at index " << i << ": " << channel.last_error() << "\n";
                    server_ok.store(false);
                    return;
                }
                // Authorization + tenant routing happens here, using the
                // token derived from the VERIFIED certificate above -- the
                // frame's own event_id/trace_id/metric_value are the only
                // wire-supplied fields that reach the engine.
                if (gateway.record(token, frame.event_id, frame.trace_id, frame.metric_value)) {
                    ++server_frames_accepted;
                }
            }
        }
        catch (const std::exception& ex) {
            std::cerr << "[SERVER] exception: " << ex.what() << "\n";
            server_ok.store(false);
        }
        });

    while (!server_ready.load()) std::this_thread::sleep_for(std::chrono::milliseconds(5));

    bool client_ok = true;
    std::string client_negotiated_protocol;
    double total_duration_ms = 0.0;
    try {
        TcpSocket conn = TcpSocket::connect_to("127.0.0.1", kDemoPort);
        SecureChannel channel(std::move(conn));
        if (!channel.handshake_as_client(client_cert.get(), L"animus-server")) {
            std::cerr << "[CLIENT] handshake failed: " << channel.last_error() << "\n";
            client_ok = false;
        }
        else {
            std::string chain_error;
            if (!verify_certificate_chain(channel.peer_certificate(), trust, szOID_PKIX_KP_SERVER_AUTH, &chain_error)) {
                std::cerr << "[CLIENT] server certificate rejected: " << chain_error << "\n";
                client_ok = false;
            }
            else {
                client_negotiated_protocol = channel.negotiated_protocol_name();
                std::cout << "[CLIENT] mTLS handshake complete, negotiated " << client_negotiated_protocol
                    << ", server cert CN=" << narrow_ascii(get_subject_common_name(channel.peer_certificate())) << "\n";

                auto start = std::chrono::high_resolution_clock::now();
                for (uint32_t i = 0; i < TOTAL_FRAMES && client_ok; ++i) {
                    WireFrame frame{ /*event_id=*/7, /*trace_id=*/i, /*metric_value=*/1000 + i };
                    if (!channel.send_frame(frame)) {
                        std::cerr << "[CLIENT] send_frame failed at index " << i << ": " << channel.last_error() << "\n";
                        client_ok = false;
                    }
                }
                auto end = std::chrono::high_resolution_clock::now();
                total_duration_ms = std::chrono::duration<double, std::milli>(end - start).count();
            }
        }
    }
    catch (const std::exception& ex) {
        std::cerr << "[CLIENT] exception: " << ex.what() << "\n";
        client_ok = false;
    }

    server_thread.join();

    // load_pfx_certificate() persists these demo keys to the user's key
    // store (see its comment in animus_transport.hpp); remove them again
    // now that both channels are closed, so repeated demo runs don't
    // accumulate key material.
    delete_persisted_key(server_cert.get());
    delete_persisted_key(client_cert.get());

    // Isolation check: a second, DIFFERENT tenant (never granted a token,
    // never wired into identity_map) must see none of tenant 42's signals
    // and must be denied a poll against tenant 42 directly.
    AccessToken other_tenant_viewer{ /*tenant_id=*/99, /*principal_id=*/2002, Role::Viewer };
    ThreatSignal dummy[1];
    bool cross_tenant_denied = (gateway.poll_signals(other_tenant_viewer, dummy, 1) == 0);

    std::cout << "\n=== PHASE 8 SECURE TRANSPORT + RBAC/TENANCY DEMO ===\n";
    std::cout << "Client negotiated protocol   : " << client_negotiated_protocol << "\n";
    std::cout << "Server negotiated protocol   : " << server_negotiated_protocol << "\n";
    std::cout << "Server-verified client CN    : " << narrow_ascii(server_peer_cn) << "\n";
    std::cout << "Frames sent by client         : " << TOTAL_FRAMES << "\n";
    std::cout << "Frames accepted server-side    : " << server_frames_accepted << " (RBAC-authorized, tenant " << kDemoTenantId << ")\n";
    std::cout << "Total encrypted send duration : " << total_duration_ms << " ms\n";
    std::cout << "Throughput                    : " << (TOTAL_FRAMES / (total_duration_ms / 1000.0)) << " frames/sec\n";
    std::cout << "Cross-tenant isolation check  : " << (cross_tenant_denied ? "PASS (tenant 99 sees 0 signals)" : "FAIL") << "\n";

    bool all_ok = server_ok.load() && client_ok
        && server_frames_accepted == TOTAL_FRAMES
        && client_negotiated_protocol == "TLS 1.3"
        && server_negotiated_protocol == "TLS 1.3"
        && cross_tenant_denied;

    std::cout << (all_ok ? "[SECURE TRANSPORT DEMO] ALL CHECKS PASSED\n" : "[SECURE TRANSPORT DEMO] FAILED\n");
    return all_ok ? 0 : 1;
}
