#pragma once
// Phase 8: mTLS / TLS 1.3 encrypted transport for remote telemetry ingestion.
//
// Windows-only: built directly on the OS-native Schannel SSPI provider
// (secur32.lib / crypt32.lib / ws2_32.lib -- all part of the Windows SDK)
// rather than a third-party TLS library such as OpenSSL. This keeps
// animus.hpp's "zero external dependency" property intact on the one
// platform this project currently ships a native build for (see
// AnimusCore_v1.vcxproj) -- the tradeoff, made explicitly rather than
// silently, is that this header does not build on non-Windows platforms.
// animus_security.hpp (RBAC + multi-tenant isolation) has no such
// restriction and is plain portable C++17.
//
// Design: a verified client certificate is mapped to an
// animus::security::AccessToken (tenant_id + role) via CertificateIdentityMap
// *after* Schannel has cryptographically verified the certificate chain --
// so RBAC/tenant-isolation decisions downstream are made against an
// identity TLS has already proven the peer possesses the private key for,
// never a value the client merely asserts in application-layer data.
#if !defined(_WIN32)
#error "animus_transport.hpp is Windows-only (Schannel/SSPI). See animus_security.hpp for the platform-independent RBAC/tenancy layer."
#endif

#define SECURITY_WIN32
// SCH_CREDENTIALS/TLS_PARAMETERS (the modern credential structure --
// required to actually negotiate TLS 1.3; see the comment in
// SecureChannel::handshake_as_client()) are compiled into schannel.h only
// under SCHANNEL_USE_BLACKLISTS, and that block itself uses UNICODE_STRING/
// PUNICODE_STRING, which come from subauth.h rather than windows.h.
#define SCHANNEL_USE_BLACKLISTS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <subauth.h>
#include <sspi.h>
#include <schannel.h>
#include <wincrypt.h>
#include <ncrypt.h>

#include "animus_security.hpp"

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <fstream>
#include <iterator>
#include <stdexcept>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "secur32.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "ncrypt.lib")
#pragma comment(lib, "advapi32.lib")

// Windows SDKs prior to 10.0.20348 (Server 2022 / 21H2) don't define the
// TLS 1.3 protocol bits in schannel.h. Fall back to the documented values
// so this header still compiles (and correctly negotiates TLS 1.3) against
// an older SDK, rather than silently downgrading to TLS 1.2.
#ifndef SP_PROT_TLS1_3_SERVER
#define SP_PROT_TLS1_3_SERVER 0x00001000
#endif
#ifndef SP_PROT_TLS1_3_CLIENT
#define SP_PROT_TLS1_3_CLIENT 0x00002000
#endif
#ifndef SP_PROT_TLS1_3
#define SP_PROT_TLS1_3 (SP_PROT_TLS1_3_SERVER | SP_PROT_TLS1_3_CLIENT)
#endif

namespace animus {
namespace transport {

    // One length-prefixed-free telemetry frame sent per encrypted TLS
    // record. Mirrors TelemetryPayload's event_id/trace_id/metric_value.
    // tenant_id/role are intentionally NOT part of this struct: which
    // tenant a frame is attributed to, and under what role, is derived
    // server-side from the already-verified client certificate via
    // CertificateIdentityMap, never taken from wire-supplied fields --
    // a client cannot claim to be a different tenant just by changing a
    // value in its own request.
#pragma pack(push, 1)
    struct WireFrame {
        uint32_t event_id;
        uint32_t trace_id;
        uint64_t metric_value;
    };
#pragma pack(pop)

    inline std::string hresult_hex(long value) {
        char buf[24];
        std::snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(value));
        return std::string(buf);
    }

    // -----------------------------------------------------------------------
    // Minimal blocking TCP wrapper (WinSock). Only what the handshake/frame
    // loops below need: connect, listen+accept, send-all, receive-some.
    // -----------------------------------------------------------------------
    class WinsockInit {
    public:
        WinsockInit() {
            WSADATA wsa;
            ok_ = (WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
        }
        ~WinsockInit() { if (ok_) WSACleanup(); }
        WinsockInit(const WinsockInit&) = delete;
        WinsockInit& operator=(const WinsockInit&) = delete;
        bool ok() const noexcept { return ok_; }
    private:
        bool ok_ = false;
    };

    class TcpSocket {
    public:
        TcpSocket() = default;
        explicit TcpSocket(SOCKET s) noexcept : sock_(s) {}
        TcpSocket(const TcpSocket&) = delete;
        TcpSocket& operator=(const TcpSocket&) = delete;
        TcpSocket(TcpSocket&& other) noexcept : sock_(other.sock_) { other.sock_ = INVALID_SOCKET; }
        TcpSocket& operator=(TcpSocket&& other) noexcept {
            if (this != &other) { close(); sock_ = other.sock_; other.sock_ = INVALID_SOCKET; }
            return *this;
        }
        ~TcpSocket() { close(); }

        void close() noexcept {
            if (sock_ != INVALID_SOCKET) { closesocket(sock_); sock_ = INVALID_SOCKET; }
        }

        bool valid() const noexcept { return sock_ != INVALID_SOCKET; }

        // timeout_ms bounds how long a connection ATTEMPT can take -- not
        // just the degenerate "port refused" case (which is normally near-
        // instant on loopback and doesn't need this), but the case a
        // cluster actually cares about: a peer whose listening socket was
        // JUST closed (see animus_cluster.hpp's RaftNode::stop()) can, in
        // practice on Windows, leave a brief window where a fresh connect()
        // to that port is neither accepted nor immediately RST'd, and the
        // OS's default connect timeout for that case is many seconds --
        // long enough to stall an entire Raft election round waiting on a
        // single dead peer. Implemented as non-blocking connect + select()
        // rather than a plain blocking connect(), specifically so a dead
        // peer fails fast and predictably instead of at the mercy of the
        // OS's default TCP retransmission/timeout schedule.
        static TcpSocket connect_to(const std::string& host, uint16_t port, int timeout_ms = 5000) {
            SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (s == INVALID_SOCKET) throw std::runtime_error("socket() failed: " + std::to_string(WSAGetLastError()));
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(port);
            if (InetPtonA(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
                closesocket(s);
                throw std::runtime_error("invalid IPv4 address: " + host);
            }

            u_long non_blocking = 1;
            ioctlsocket(s, FIONBIO, &non_blocking);

            int rc = ::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
            if (rc != 0) {
                int err = WSAGetLastError();
                if (err != WSAEWOULDBLOCK) {
                    closesocket(s);
                    throw std::runtime_error("connect() failed: " + std::to_string(err));
                }
                fd_set write_set, err_set;
                FD_ZERO(&write_set); FD_SET(s, &write_set);
                FD_ZERO(&err_set); FD_SET(s, &err_set);
                timeval tv{};
                tv.tv_sec = timeout_ms / 1000;
                tv.tv_usec = (timeout_ms % 1000) * 1000;
                int ready = select(0, nullptr, &write_set, &err_set, &tv);
                if (ready == 0) {
                    closesocket(s);
                    throw std::runtime_error("connect() timed out after " + std::to_string(timeout_ms) + " ms");
                }
                if (ready == SOCKET_ERROR || FD_ISSET(s, &err_set)) {
                    closesocket(s);
                    throw std::runtime_error("connect() failed: " + std::to_string(WSAGetLastError()));
                }
                int so_error = 0;
                int so_error_len = sizeof(so_error);
                if (getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_error), &so_error_len) != 0 || so_error != 0) {
                    closesocket(s);
                    throw std::runtime_error("connect() failed: " + std::to_string(so_error));
                }
            }

            u_long blocking = 0;
            ioctlsocket(s, FIONBIO, &blocking); // every other TcpSocket method assumes blocking semantics
            return TcpSocket(s);
        }

        static TcpSocket listen_on(uint16_t port) {
            SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (s == INVALID_SOCKET) throw std::runtime_error("socket() failed: " + std::to_string(WSAGetLastError()));
            BOOL reuse = TRUE;
            setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(port);
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
                closesocket(s);
                throw std::runtime_error("bind() failed: " + std::to_string(WSAGetLastError()));
            }
            if (::listen(s, 1) != 0) {
                closesocket(s);
                throw std::runtime_error("listen() failed: " + std::to_string(WSAGetLastError()));
            }
            return TcpSocket(s);
        }

        TcpSocket accept_one() {
            SOCKET c = ::accept(sock_, nullptr, nullptr);
            if (c == INVALID_SOCKET) throw std::runtime_error("accept() failed: " + std::to_string(WSAGetLastError()));
            return TcpSocket(c);
        }

        bool send_all(const uint8_t* data, size_t len) noexcept {
            size_t sent = 0;
            while (sent < len) {
                int n = ::send(sock_, reinterpret_cast<const char*>(data + sent), static_cast<int>(len - sent), 0);
                if (n <= 0) return false;
                sent += static_cast<size_t>(n);
            }
            return true;
        }

        // One raw recv() call -- may return fewer bytes than a caller wants,
        // by design: TLS record boundaries don't align with socket reads,
        // so callers accumulate into their own buffer across calls.
        bool recv_some(std::vector<uint8_t>& out) noexcept {
            char buf[8192];
            int n = ::recv(sock_, buf, sizeof(buf), 0);
            if (n <= 0) return false;
            out.assign(buf, buf + n);
            return true;
        }

    private:
        SOCKET sock_ = INVALID_SOCKET;
    };

    // -----------------------------------------------------------------------
    // Certificate loading and chain verification (CryptoAPI / wincrypt.h)
    // -----------------------------------------------------------------------

    // A stateless functor deleter (rather than a bare function pointer)
    // keeps CertContextPtr default-constructible -- std::unique_ptr's
    // no-argument constructor is disabled for a function-pointer deleter
    // (it has no guaranteed-valid default value), but is fine for an
    // empty class type like this one.
    struct CertContextDeleter {
        void operator()(PCCERT_CONTEXT ctx) const noexcept {
            if (ctx) CertFreeCertificateContext(ctx);
        }
    };
    using CertContextPtr = std::unique_ptr<const CERT_CONTEXT, CertContextDeleter>;
    inline CertContextPtr make_cert_ptr(PCCERT_CONTEXT ctx) { return CertContextPtr(ctx); }

    // Loads a leaf certificate + its private key from a PFX file (see
    // generate_demo_certs.ps1) -- used for the server's own identity and
    // the client's own identity.
    //
    // The key is imported with CRYPT_USER_KEYSET (persisted to the calling
    // user's CNG/CAPI key store), not PKCS12_NO_PERSIST_KEY. That was a
    // deliberate correction, not the obvious choice: PKCS12_NO_PERSIST_KEY
    // (an in-memory-only ephemeral key, never touching disk) was tried
    // first as the safer-looking default, but was empirically found to
    // make AcquireCredentialsHandleW reject the resulting certificate with
    // SEC_E_UNKNOWN_CREDENTIALS on this platform/SDK -- reproduced with a
    // minimal isolated repro outside this header, independent of anything
    // else in animus_transport.hpp. delete_persisted_key() below removes
    // the key again once a SecureChannel is done with it, so a long-running
    // process (or repeated demo runs) doesn't accumulate key material in
    // the user's key store.
    inline CertContextPtr load_pfx_certificate(const std::wstring& pfx_path, const std::wstring& password) {
        std::ifstream file(pfx_path, std::ios::binary);
        if (!file) throw std::runtime_error("cannot open PFX file");
        std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (bytes.empty()) throw std::runtime_error("PFX file is empty");

        CRYPT_DATA_BLOB blob{};
        blob.cbData = static_cast<DWORD>(bytes.size());
        blob.pbData = reinterpret_cast<BYTE*>(bytes.data());

        HCERTSTORE store = PFXImportCertStore(&blob, password.c_str(), CRYPT_EXPORTABLE | CRYPT_USER_KEYSET);
        if (!store) throw std::runtime_error("PFXImportCertStore failed: " + hresult_hex(static_cast<long>(GetLastError())));

        PCCERT_CONTEXT cert = CertFindCertificateInStore(
            store, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0, CERT_FIND_HAS_PRIVATE_KEY, nullptr, nullptr);
        if (!cert) {
            CertCloseStore(store, 0);
            throw std::runtime_error("PFX contains no certificate with an attached private key");
        }
        CertContextPtr result = make_cert_ptr(CertDuplicateCertificateContext(cert));
        CertFreeCertificateContext(cert);
        CertCloseStore(store, 0);
        return result;
    }

    // Deletes the private key a load_pfx_certificate() call persisted to
    // the user's key store (CNG via NCryptDeleteKey, or legacy CAPI via
    // CryptAcquireContextW(..., CRYPT_DELETEKEYSET) as a fallback) --
    // pairs with load_pfx_certificate's CRYPT_USER_KEYSET so a demo/test
    // process doesn't accumulate key material run over run. Best-effort:
    // silently does nothing if the certificate has no private key or the
    // key was already removed.
    inline void delete_persisted_key(PCCERT_CONTEXT cert) noexcept {
        if (!cert) return;
        DWORD keySpec = 0;
        HCRYPTPROV_OR_NCRYPT_KEY_HANDLE key = 0;
        BOOL free_key = FALSE;
        if (!CryptAcquireCertificatePrivateKey(cert, CRYPT_ACQUIRE_SILENT_FLAG, nullptr, &key, &keySpec, &free_key)) {
            return;
        }
        if (keySpec == CERT_NCRYPT_KEY_SPEC) {
            NCryptDeleteKey(static_cast<NCRYPT_KEY_HANDLE>(key), 0); // also frees the handle
            return;
        }
        if (free_key) CryptReleaseContext(static_cast<HCRYPTPROV>(key), 0);

        DWORD size = 0;
        if (!CertGetCertificateContextProperty(cert, CERT_KEY_PROV_INFO_PROP_ID, nullptr, &size)) return;
        std::vector<BYTE> buf(size);
        auto* info = reinterpret_cast<CRYPT_KEY_PROV_INFO*>(buf.data());
        if (!CertGetCertificateContextProperty(cert, CERT_KEY_PROV_INFO_PROP_ID, info, &size)) return;
        HCRYPTPROV prov = 0;
        CryptAcquireContextW(&prov, info->pwszContainerName, info->pwszProvName, info->dwProvType, CRYPT_DELETEKEYSET);
    }

    // Loads a DER-encoded CA certificate (see generate_demo_certs.ps1's
    // ca.cer, exported via Export-Certificate) -- no private key needed,
    // this is only ever used as a trust anchor.
    inline CertContextPtr load_cer_certificate(const std::wstring& cer_path) {
        std::ifstream file(cer_path, std::ios::binary);
        if (!file) throw std::runtime_error("cannot open CER file");
        std::vector<BYTE> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (bytes.empty()) throw std::runtime_error("CER file is empty");
        PCCERT_CONTEXT cert = CertCreateCertificateContext(X509_ASN_ENCODING, bytes.data(), static_cast<DWORD>(bytes.size()));
        if (!cert) throw std::runtime_error("CertCreateCertificateContext failed: " + hresult_hex(static_cast<long>(GetLastError())));
        return make_cert_ptr(cert);
    }

    inline std::wstring get_subject_common_name(PCCERT_CONTEXT cert) {
        DWORD len = CertGetNameStringW(cert, CERT_NAME_ATTR_TYPE, 0,
            const_cast<LPSTR>(szOID_COMMON_NAME), nullptr, 0);
        if (len <= 1) return {};
        std::wstring name(len, L'\0');
        CertGetNameStringW(cert, CERT_NAME_ATTR_TYPE, 0,
            const_cast<LPSTR>(szOID_COMMON_NAME), name.data(), len);
        name.resize(std::wcslen(name.c_str()));
        return name;
    }

    inline bool certificate_has_eku(PCCERT_CONTEXT cert, LPCSTR oid) {
        DWORD size = 0;
        if (!CertGetEnhancedKeyUsage(cert, 0, nullptr, &size) || size == 0) return false;
        std::vector<BYTE> buf(size);
        auto* usage = reinterpret_cast<PCERT_ENHKEY_USAGE>(buf.data());
        if (!CertGetEnhancedKeyUsage(cert, 0, usage, &size)) return false;
        for (DWORD i = 0; i < usage->cUsageIdentifier; ++i) {
            if (std::strcmp(usage->rgpszUsageIdentifier[i], oid) == 0) return true;
        }
        return false;
    }

    // Wraps an in-memory certificate store holding exactly one trusted CA,
    // plus a CERT_CHAIN_ENGINE configured with that store as its EXCLUSIVE
    // root (CERT_CHAIN_ENGINE_CONFIG::hExclusiveRoot) -- chain verification
    // through this engine trusts only certificates issued by our own demo
    // CA, never anything already sitting in the Windows system Root store.
    // This is what makes SCH_CRED_MANUAL_CRED_VALIDATION (used by both the
    // client and server credentials below, so Schannel does not attempt
    // its own default trust check) safe: verify_certificate_chain() below
    // performs the real check that SCH_CRED_MANUAL_CRED_VALIDATION skipped.
    class TrustedRoot {
    public:
        explicit TrustedRoot(PCCERT_CONTEXT ca_cert) {
            store_ = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, 0, nullptr);
            if (!store_) throw std::runtime_error("CertOpenStore(memory) failed");
            if (!CertAddCertificateContextToStore(store_, ca_cert, CERT_STORE_ADD_ALWAYS, nullptr)) {
                CertCloseStore(store_, 0);
                throw std::runtime_error("CertAddCertificateContextToStore failed");
            }
            CERT_CHAIN_ENGINE_CONFIG cfg{};
            cfg.cbSize = sizeof(cfg);
            cfg.hExclusiveRoot = store_;
            if (!CertCreateCertificateChainEngine(&cfg, &engine_)) {
                CertCloseStore(store_, 0);
                throw std::runtime_error("CertCreateCertificateChainEngine failed");
            }
        }
        ~TrustedRoot() {
            if (engine_) CertFreeCertificateChainEngine(engine_);
            if (store_) CertCloseStore(store_, 0);
        }
        TrustedRoot(const TrustedRoot&) = delete;
        TrustedRoot& operator=(const TrustedRoot&) = delete;

        HCERTCHAINENGINE engine() const noexcept { return engine_; }
        // Also used as CertGetCertificateChain's hAdditionalStore: our CA
        // is the leaf's issuer as well as its root (a two-tier demo chain),
        // so chain building needs it available both as a candidate issuer
        // and as the trusted root, not only the latter.
        HCERTSTORE store() const noexcept { return store_; }

    private:
        HCERTSTORE store_ = nullptr;
        HCERTCHAINENGINE engine_ = nullptr;
    };

    // Real chain-of-trust verification against `trust`'s exclusive CA --
    // NOT a stub. Deliberately out of scope for this demo-CA setup:
    // revocation checking (no CRL/OCSP endpoint exists for a private demo
    // CA) and full RFC 5280 policy constraints via
    // CertVerifyCertificateChainPolicy -- a production deployment with a
    // real internal CA should add both. required_eku, if non-null, must
    // also be present on the leaf (e.g. szOID_PKIX_KP_SERVER_AUTH /
    // szOID_PKIX_KP_CLIENT_AUTH) so a client cert can't be replayed as a
    // server cert or vice versa.
    inline bool verify_certificate_chain(PCCERT_CONTEXT cert, const TrustedRoot& trust, LPCSTR required_eku, std::string* error_out) {
        CERT_CHAIN_PARA chain_para{};
        chain_para.cbSize = sizeof(chain_para);

        PCCERT_CHAIN_CONTEXT chain = nullptr;
        BOOL ok = CertGetCertificateChain(trust.engine(), cert, nullptr, trust.store(), &chain_para,
            CERT_CHAIN_CACHE_END_CERT, nullptr, &chain);
        if (!ok || !chain) {
            if (error_out) *error_out = "CertGetCertificateChain failed: " + hresult_hex(static_cast<long>(GetLastError()));
            return false;
        }

        bool trusted = (chain->TrustStatus.dwErrorStatus == CERT_TRUST_NO_ERROR);
        if (!trusted && error_out) {
            *error_out = "certificate chain not trusted, dwErrorStatus=" + hresult_hex(static_cast<long>(chain->TrustStatus.dwErrorStatus));
        }
        CertFreeCertificateChain(chain);
        if (!trusted) return false;

        if (required_eku && !certificate_has_eku(cert, required_eku)) {
            if (error_out) *error_out = std::string("certificate missing required EKU: ") + required_eku;
            return false;
        }
        return true;
    }

    // Maps a verified peer certificate's subject CN to an AccessToken.
    // Only ever consulted AFTER verify_certificate_chain() has succeeded --
    // an unverified certificate's CN is not a trustworthy input.
    class CertificateIdentityMap {
    public:
        void add(const std::wstring& common_name, const security::AccessToken& token) {
            map_[common_name] = token;
        }

        bool resolve(PCCERT_CONTEXT cert, security::AccessToken& out) const {
            std::wstring cn = get_subject_common_name(cert);
            auto it = map_.find(cn);
            if (it == map_.end()) return false;
            out = it->second;
            return true;
        }

    private:
        std::unordered_map<std::wstring, security::AccessToken> map_;
    };

    // -----------------------------------------------------------------------
    // SecureChannel: one Schannel (SSPI) TLS 1.3 session, client or server
    // side, layered directly over a TcpSocket. Owns the CredHandle/CtxtHandle
    // for its whole lifetime; the destructor sends a close_notify and frees
    // both. Mutual authentication is mandatory in both directions: a client
    // must present a cert (ISC_REQ_USE_SUPPLIED_CREDS + a supplied cert), a
    // server requires one (ASC_REQ_MUTUAL_AUTH), and finish_handshake()
    // fails the connection if the peer's certificate cannot be retrieved
    // after the handshake completes.
    // -----------------------------------------------------------------------
    class SecureChannel {
    public:
        explicit SecureChannel(TcpSocket socket) noexcept : socket_(std::move(socket)) {
            SecInvalidateHandle(&cred_handle_);
            SecInvalidateHandle(&ctxt_handle_);
        }

        SecureChannel(const SecureChannel&) = delete;
        SecureChannel& operator=(const SecureChannel&) = delete;

        ~SecureChannel() {
            if (handshake_done_) send_close_notify();
            if (SecIsValidHandle(&ctxt_handle_)) DeleteSecurityContext(&ctxt_handle_);
            if (SecIsValidHandle(&cred_handle_)) FreeCredentialsHandle(&cred_handle_);
        }

        bool handshake_as_client(PCCERT_CONTEXT client_cert, const std::wstring& target_name) {
            is_server_ = false;
            // TLS 1.3's cipher suites are AEAD/HKDF-based and don't map onto
            // the legacy SCHANNEL_CRED::grbitEnabledProtocols allow-list
            // model (attempting to force SP_PROT_TLS1_3_CLIENT that way
            // fails AcquireCredentialsHandle with SEC_E_UNKNOWN_CREDENTIALS
            // / SEC_E_ALGORITHM_MISMATCH on this SDK) -- SCH_CREDENTIALS +
            // TLS_PARAMETERS::grbitDisabledProtocols (a DENY-list) is the
            // documented mechanism for TLS 1.3 with Schannel.
            TLS_PARAMETERS tls_params{};
            tls_params.grbitDisabledProtocols = static_cast<DWORD>(~SP_PROT_TLS1_3);

            PCCERT_CONTEXT certs[1] = { client_cert };
            SCH_CREDENTIALS cred{};
            cred.dwVersion = SCH_CREDENTIALS_VERSION;
            cred.dwCredFormat = SCH_CRED_FORMAT_CERT_CONTEXT;
            cred.cCreds = 1;
            cred.paCred = certs;
            // SCH_CRED_DISABLE_RECONNECTS: also stops the PEER (a server
            // we connect to) from issuing TLS 1.3 post-handshake
            // NewSessionTicket messages it otherwise would -- see the
            // matching comment on the server side below for why that
            // matters to every reader of this connection, not just this
            // client.
            cred.dwFlags = SCH_CRED_MANUAL_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS | SCH_CRED_DISABLE_RECONNECTS;
            cred.cTlsParameters = 1;
            cred.pTlsParameters = &tls_params;

            TimeStamp expiry{};
            SECURITY_STATUS status = AcquireCredentialsHandleW(
                nullptr, const_cast<LPWSTR>(UNISP_NAME_W), SECPKG_CRED_OUTBOUND,
                nullptr, &cred, nullptr, nullptr, &cred_handle_, &expiry);
            if (status != SEC_E_OK) { last_error_ = "AcquireCredentialsHandle(client): " + hresult_hex(status); return false; }

            DWORD context_req = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY |
                ISC_RET_EXTENDED_ERROR | ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM | ISC_REQ_USE_SUPPLIED_CREDS;

            std::vector<uint8_t> in_buf;
            bool have_context = false;
            DWORD out_flags = 0;
            std::wstring target = target_name; // InitializeSecurityContextW takes a non-const pointer

            for (;;) {
                SecBuffer out_buf{}; out_buf.BufferType = SECBUFFER_TOKEN;
                SecBufferDesc out_desc{ SECBUFFER_VERSION, 1, &out_buf };
                SECURITY_STATUS ss;

                if (!have_context) {
                    ss = InitializeSecurityContextW(&cred_handle_, nullptr, target.data(), context_req, 0, 0,
                        nullptr, 0, &ctxt_handle_, &out_desc, &out_flags, nullptr);
                }
                else {
                    SecBuffer in_bufs[2]{};
                    in_bufs[0].BufferType = SECBUFFER_TOKEN;
                    in_bufs[0].pvBuffer = in_buf.data();
                    in_bufs[0].cbBuffer = static_cast<unsigned long>(in_buf.size());
                    in_bufs[1].BufferType = SECBUFFER_EMPTY;
                    SecBufferDesc in_desc{ SECBUFFER_VERSION, 2, in_bufs };

                    ss = InitializeSecurityContextW(&cred_handle_, &ctxt_handle_, target.data(), context_req, 0, 0,
                        &in_desc, 0, nullptr, &out_desc, &out_flags, nullptr);

                    consume_input_buffer(in_buf, in_bufs[1], ss);
                }
                have_context = true;

                if (out_buf.cbBuffer > 0 && out_buf.pvBuffer) {
                    bool sent = socket_.send_all(reinterpret_cast<const uint8_t*>(out_buf.pvBuffer), out_buf.cbBuffer);
                    FreeContextBuffer(out_buf.pvBuffer);
                    if (!sent) { last_error_ = "socket send failed during client handshake"; return false; }
                }

                if (ss == SEC_E_OK) break;
                if (ss == SEC_I_CONTINUE_NEEDED || ss == SEC_E_INCOMPLETE_MESSAGE) {
                    std::vector<uint8_t> chunk;
                    if (!socket_.recv_some(chunk)) { last_error_ = "peer closed connection during client handshake"; return false; }
                    in_buf.insert(in_buf.end(), chunk.begin(), chunk.end());
                    continue;
                }
                last_error_ = "InitializeSecurityContext: " + hresult_hex(ss);
                return false;
            }

            return finish_handshake();
        }

        bool handshake_as_server(PCCERT_CONTEXT server_cert) {
            is_server_ = true;
            // See handshake_as_client()'s comment: SCH_CREDENTIALS +
            // TLS_PARAMETERS::grbitDisabledProtocols, not legacy
            // SCHANNEL_CRED, is what actually negotiates TLS 1.3 here.
            TLS_PARAMETERS tls_params{};
            tls_params.grbitDisabledProtocols = static_cast<DWORD>(~SP_PROT_TLS1_3);

            PCCERT_CONTEXT certs[1] = { server_cert };
            SCH_CREDENTIALS cred{};
            cred.dwVersion = SCH_CREDENTIALS_VERSION;
            cred.dwCredFormat = SCH_CRED_FORMAT_CERT_CONTEXT;
            cred.cCreds = 1;
            cred.paCred = certs;
            // SCH_CRED_DISABLE_RECONNECTS suppresses TLS 1.3 session
            // resumption on this server credential -- in particular, the
            // automatic post-handshake NewSessionTicket message(s) a
            // Schannel TLS 1.3 server would otherwise send right after the
            // handshake completes. Those tickets are encrypted with the
            // application traffic key and arrive indistinguishable from a
            // real data record until DecryptMessage is called on them, at
            // which point it returns SEC_I_RENEGOTIATE instead of handing
            // back application data -- correctly reprocessing that status
            // (rather than just discarding the record) requires re-driving
            // {Initialize,Accept}SecurityContext with exactly the right
            // leftover bytes, which is finicky enough that a real attempt
            // at it here produced a genuine deadlock (SecureChannel is
            // symmetric for Phase 9 -- every connection's dialer both
            // sends a request AND blocks reading the response, so the two
            // ends can end up mutually waiting). Not issuing tickets in
            // the first place avoids the whole class of bug; recv_frame()/
            // decrypt_one_record() below still fail fast with a clear
            // error if SEC_I_RENEGOTIATE ever shows up anyway, rather than
            // silently hanging.
            cred.dwFlags = SCH_CRED_MANUAL_CRED_VALIDATION | SCH_CRED_DISABLE_RECONNECTS;
            cred.cTlsParameters = 1;
            cred.pTlsParameters = &tls_params;

            TimeStamp expiry{};
            SECURITY_STATUS status = AcquireCredentialsHandleW(
                nullptr, const_cast<LPWSTR>(UNISP_NAME_W), SECPKG_CRED_INBOUND,
                nullptr, &cred, nullptr, nullptr, &cred_handle_, &expiry);
            if (status != SEC_E_OK) { last_error_ = "AcquireCredentialsHandle(server): " + hresult_hex(status); return false; }

            DWORD context_req = ASC_REQ_SEQUENCE_DETECT | ASC_REQ_REPLAY_DETECT | ASC_REQ_CONFIDENTIALITY |
                ASC_REQ_EXTENDED_ERROR | ASC_REQ_ALLOCATE_MEMORY | ASC_REQ_STREAM | ASC_REQ_MUTUAL_AUTH;

            std::vector<uint8_t> in_buf;
            bool have_context = false;
            DWORD out_flags = 0;

            for (;;) {
                std::vector<uint8_t> chunk;
                if (!socket_.recv_some(chunk)) { last_error_ = "peer closed connection during server handshake"; return false; }
                in_buf.insert(in_buf.end(), chunk.begin(), chunk.end());

                for (;;) {
                    SecBuffer in_bufs[2]{};
                    in_bufs[0].BufferType = SECBUFFER_TOKEN;
                    in_bufs[0].pvBuffer = in_buf.data();
                    in_bufs[0].cbBuffer = static_cast<unsigned long>(in_buf.size());
                    in_bufs[1].BufferType = SECBUFFER_EMPTY;
                    SecBufferDesc in_desc{ SECBUFFER_VERSION, 2, in_bufs };

                    SecBuffer out_buf{}; out_buf.BufferType = SECBUFFER_TOKEN;
                    SecBufferDesc out_desc{ SECBUFFER_VERSION, 1, &out_buf };

                    SECURITY_STATUS ss = AcceptSecurityContext(&cred_handle_,
                        have_context ? &ctxt_handle_ : nullptr, &in_desc, context_req, 0,
                        &ctxt_handle_, &out_desc, &out_flags, nullptr);
                    have_context = true;

                    bool had_extra = consume_input_buffer(in_buf, in_bufs[1], ss);

                    if (out_buf.cbBuffer > 0 && out_buf.pvBuffer) {
                        bool sent = socket_.send_all(reinterpret_cast<const uint8_t*>(out_buf.pvBuffer), out_buf.cbBuffer);
                        FreeContextBuffer(out_buf.pvBuffer);
                        if (!sent) { last_error_ = "socket send failed during server handshake"; return false; }
                    }

                    if (ss == SEC_E_OK) return finish_handshake();
                    if (ss == SEC_I_CONTINUE_NEEDED) {
                        if (had_extra) continue; // more buffered handshake data already in hand
                        break; // need more bytes from the socket
                    }
                    if (ss == SEC_E_INCOMPLETE_MESSAGE) break; // need more bytes from the socket
                    last_error_ = "AcceptSecurityContext: " + hresult_hex(ss);
                    return false;
                }
            }
        }

        bool send_frame(const WireFrame& frame) noexcept {
            if (!handshake_done_) return false;
            std::vector<uint8_t> io_buf(stream_sizes_.cbHeader + sizeof(WireFrame) + stream_sizes_.cbTrailer);
            std::memcpy(io_buf.data() + stream_sizes_.cbHeader, &frame, sizeof(WireFrame));

            SecBuffer bufs[4]{};
            bufs[0] = { stream_sizes_.cbHeader, SECBUFFER_STREAM_HEADER, io_buf.data() };
            bufs[1] = { static_cast<unsigned long>(sizeof(WireFrame)), SECBUFFER_DATA, io_buf.data() + stream_sizes_.cbHeader };
            bufs[2] = { stream_sizes_.cbTrailer, SECBUFFER_STREAM_TRAILER, io_buf.data() + stream_sizes_.cbHeader + sizeof(WireFrame) };
            bufs[3] = { 0, SECBUFFER_EMPTY, nullptr };
            SecBufferDesc desc{ SECBUFFER_VERSION, 4, bufs };

            SECURITY_STATUS ss = EncryptMessage(&ctxt_handle_, 0, &desc, 0);
            if (ss != SEC_E_OK) { last_error_ = "EncryptMessage: " + hresult_hex(ss); return false; }

            size_t total = bufs[0].cbBuffer + bufs[1].cbBuffer + bufs[2].cbBuffer;
            return socket_.send_all(io_buf.data(), total);
        }

        // -------------------------------------------------------------
        // Generic variable-length message framing, layered on the same
        // EncryptMessage/DecryptMessage record plumbing as send_frame/
        // recv_frame above but not tied to the fixed-size WireFrame --
        // Phase 9's cluster RPCs (RequestVote/AppendEntries) carry a
        // variable number of log entries, so a fixed struct doesn't fit.
        // Wire shape: a 4-byte little-endian length prefix, itself sent
        // as its own TLS record, followed by that many payload bytes
        // (chunked into further records if larger than one TLS record's
        // max message size). recv_message() reassembles across however
        // many DecryptMessage calls that takes, buffering leftover
        // plaintext in plaintext_buf_ for the next call.
        // -------------------------------------------------------------
        bool send_message(const std::vector<uint8_t>& payload) noexcept {
            uint32_t len = static_cast<uint32_t>(payload.size());
            uint8_t hdr[4];
            std::memcpy(hdr, &len, 4);
            if (!send_raw(hdr, 4)) return false;
            if (payload.empty()) return true;
            return send_raw(payload.data(), payload.size());
        }

        bool recv_message(std::vector<uint8_t>& payload) noexcept {
            std::vector<uint8_t> hdr;
            if (!recv_exact(4, hdr)) return false;
            uint32_t len;
            std::memcpy(&len, hdr.data(), 4);
            if (len > (64u * 1024 * 1024)) { last_error_ = "recv_message: implausible length prefix"; return false; }
            return recv_exact(len, payload);
        }

        bool recv_frame(WireFrame& out) noexcept {
            if (!handshake_done_) return false;
            for (;;) {
                // Only pull more bytes off the socket when recv_buf_ can't
                // already satisfy a decrypt -- a single recv_some() often
                // returns several whole TLS records at once (loopback TCP
                // delivers in bursts), and the leftover SECBUFFER_EXTRA
                // tail from the previous recv_frame() call may already be
                // a complete record. Blocking on recv_some() unconditionally
                // here would stall waiting for bytes the peer has no more
                // of to send once it has finished and closed the socket,
                // even though the last frame(s) are already sitting in
                // recv_buf_ undecrypted.
                if (recv_buf_.empty()) {
                    std::vector<uint8_t> chunk;
                    if (!socket_.recv_some(chunk)) { last_error_ = "peer closed connection"; return false; }
                    recv_buf_.insert(recv_buf_.end(), chunk.begin(), chunk.end());
                }

                SecBuffer bufs[4]{};
                bufs[0] = { static_cast<unsigned long>(recv_buf_.size()), SECBUFFER_DATA, recv_buf_.data() };
                bufs[1] = { 0, SECBUFFER_EMPTY, nullptr };
                bufs[2] = { 0, SECBUFFER_EMPTY, nullptr };
                bufs[3] = { 0, SECBUFFER_EMPTY, nullptr };
                SecBufferDesc desc{ SECBUFFER_VERSION, 4, bufs };

                SECURITY_STATUS ss = DecryptMessage(&ctxt_handle_, &desc, 0, nullptr);
                if (ss == SEC_E_INCOMPLETE_MESSAGE) {
                    std::vector<uint8_t> chunk;
                    if (!socket_.recv_some(chunk)) { last_error_ = "peer closed connection (incomplete message)"; return false; }
                    recv_buf_.insert(recv_buf_.end(), chunk.begin(), chunk.end());
                    continue;
                }
                if (ss == SEC_I_CONTEXT_EXPIRED) { last_error_ = "peer sent close_notify"; return false; }
                if (ss == SEC_I_RENEGOTIATE) {
                    // Both handshake_as_client() and handshake_as_server()
                    // set SCH_CRED_DISABLE_RECONNECTS specifically so this
                    // should never happen (see the comment there for the
                    // full story: it stops the peer from ever sending the
                    // TLS 1.3 post-handshake message that would otherwise
                    // trigger this). Fail fast with a clear diagnostic
                    // rather than attempt to reprocess it -- an earlier,
                    // more "helpful" attempt at reprocessing it here
                    // produced a genuine deadlock under Phase 9's
                    // symmetric (both-sides-send-and-receive) connections.
                    last_error_ = "unexpected SEC_I_RENEGOTIATE (a peer sent a post-handshake TLS message; SCH_CRED_DISABLE_RECONNECTS should have prevented this)";
                    return false;
                }
                if (ss != SEC_E_OK) { last_error_ = "DecryptMessage: " + hresult_hex(ss); return false; }

                uint8_t* data_ptr = nullptr; unsigned long data_len = 0;
                uint8_t* extra_ptr = nullptr; unsigned long extra_len = 0;
                for (const SecBuffer& b : bufs) {
                    if (b.BufferType == SECBUFFER_DATA && !data_ptr) { data_ptr = static_cast<uint8_t*>(b.pvBuffer); data_len = b.cbBuffer; }
                    if (b.BufferType == SECBUFFER_EXTRA) { extra_ptr = static_cast<uint8_t*>(b.pvBuffer); extra_len = b.cbBuffer; }
                }
                if (!data_ptr || data_len != sizeof(WireFrame)) { last_error_ = "unexpected decrypted frame size"; return false; }
                std::memcpy(&out, data_ptr, sizeof(WireFrame));

                recv_buf_ = (extra_ptr && extra_len > 0)
                    ? std::vector<uint8_t>(extra_ptr, extra_ptr + extra_len)
                    : std::vector<uint8_t>();
                return true;
            }
        }

        // Valid only after a successful handshake; the channel retains
        // ownership (caller must not CertFreeCertificateContext it).
        PCCERT_CONTEXT peer_certificate() const noexcept { return peer_cert_.get(); }
        DWORD negotiated_protocol() const noexcept { return negotiated_protocol_; }
        const char* negotiated_protocol_name() const noexcept {
            if (negotiated_protocol_ & SP_PROT_TLS1_3) return "TLS 1.3";
            if (negotiated_protocol_ & SP_PROT_TLS1_2) return "TLS 1.2";
            return "unknown";
        }
        const std::string& last_error() const noexcept { return last_error_; }

        // Forcibly aborts this channel's underlying socket from a thread
        // OTHER than the one that owns/uses this channel -- unblocks
        // whatever recv_some()/send_all() call the owning thread may
        // currently be parked in (it observes a socket error and the
        // in-flight send_message/recv_message call returns false), since
        // that thread has no other way to learn the channel is being torn
        // down while blocked on a peer that will never send it anything
        // else. Used by animus_cluster.hpp's RaftNode::stop() to tear down
        // still-open inbound connections' handler threads on shutdown --
        // without this, stopping a node whose peers are still sending it
        // requests would hang forever joining those threads.
        void force_close() noexcept { socket_.close(); }

    private:
        // Encrypts and sends an arbitrary-length buffer as one or more TLS
        // records (each capped at stream_sizes_.cbMaximumMessage plaintext
        // bytes) -- the record-boundary bookkeeping needed to reassemble
        // these on the far side is decrypt_one_record()/recv_exact()'s job,
        // not this function's.
        bool send_raw(const uint8_t* data, size_t len) noexcept {
            if (!handshake_done_) return false;
            size_t max_msg = stream_sizes_.cbMaximumMessage ? stream_sizes_.cbMaximumMessage : len;
            if (max_msg == 0) max_msg = len ? len : 1;
            size_t offset = 0;
            while (offset < len) {
                size_t chunk = (len - offset < max_msg) ? (len - offset) : max_msg;
                std::vector<uint8_t> io_buf(stream_sizes_.cbHeader + chunk + stream_sizes_.cbTrailer);
                std::memcpy(io_buf.data() + stream_sizes_.cbHeader, data + offset, chunk);

                SecBuffer bufs[4]{};
                bufs[0] = { stream_sizes_.cbHeader, SECBUFFER_STREAM_HEADER, io_buf.data() };
                bufs[1] = { static_cast<unsigned long>(chunk), SECBUFFER_DATA, io_buf.data() + stream_sizes_.cbHeader };
                bufs[2] = { stream_sizes_.cbTrailer, SECBUFFER_STREAM_TRAILER, io_buf.data() + stream_sizes_.cbHeader + chunk };
                bufs[3] = { 0, SECBUFFER_EMPTY, nullptr };
                SecBufferDesc desc{ SECBUFFER_VERSION, 4, bufs };

                SECURITY_STATUS ss = EncryptMessage(&ctxt_handle_, 0, &desc, 0);
                if (ss != SEC_E_OK) { last_error_ = "EncryptMessage: " + hresult_hex(ss); return false; }

                size_t total = bufs[0].cbBuffer + bufs[1].cbBuffer + bufs[2].cbBuffer;
                if (!socket_.send_all(io_buf.data(), total)) return false;
                offset += chunk;
            }
            return true;
        }

        // Decrypts exactly one TLS record (blocking on the socket for more
        // bytes as needed) and appends its plaintext to plaintext_buf_.
        bool decrypt_one_record() noexcept {
            for (;;) {
                if (recv_buf_.empty()) {
                    std::vector<uint8_t> chunk;
                    if (!socket_.recv_some(chunk)) { last_error_ = "peer closed connection"; return false; }
                    recv_buf_.insert(recv_buf_.end(), chunk.begin(), chunk.end());
                }

                SecBuffer bufs[4]{};
                bufs[0] = { static_cast<unsigned long>(recv_buf_.size()), SECBUFFER_DATA, recv_buf_.data() };
                bufs[1] = { 0, SECBUFFER_EMPTY, nullptr };
                bufs[2] = { 0, SECBUFFER_EMPTY, nullptr };
                bufs[3] = { 0, SECBUFFER_EMPTY, nullptr };
                SecBufferDesc desc{ SECBUFFER_VERSION, 4, bufs };

                SECURITY_STATUS ss = DecryptMessage(&ctxt_handle_, &desc, 0, nullptr);
                if (ss == SEC_E_INCOMPLETE_MESSAGE) {
                    std::vector<uint8_t> chunk;
                    if (!socket_.recv_some(chunk)) { last_error_ = "peer closed connection (incomplete message)"; return false; }
                    recv_buf_.insert(recv_buf_.end(), chunk.begin(), chunk.end());
                    continue;
                }
                if (ss == SEC_I_CONTEXT_EXPIRED) { last_error_ = "peer sent close_notify"; return false; }
                if (ss == SEC_I_RENEGOTIATE) {
                    // TLS 1.3 has no real in-band renegotiation; Schannel
                    // reuses this status to mean "I decrypted a
                    // non-application-data protocol record" -- in practice
                    // almost always a server's post-handshake
                    // NewSessionTicket message. Phase 8's fixed-direction
                    // demo (server only ever recv_frame()s, client only
                    // ever send_frame()s) never triggered this because the
                    // client -- the side a ticket actually arrives at --
                    // never calls a receive function; Phase 9's cluster
                    // RPCs are bidirectional per connection (the dialing
                    // side sends a request AND receives the response), so
                    // it hit this on essentially every first RPC. An
                    // earlier attempt at reprocessing the leftover bytes
                    // via {Initialize,Accept}SecurityContext here -- rather
                    // than treating this as a hard failure -- produced a
                    // genuine deadlock (each side blocked waiting on the
                    // other). Both handshake_as_client() and
                    // handshake_as_server() now set
                    // SCH_CRED_DISABLE_RECONNECTS, which stops the peer
                    // from ever sending a session ticket, so this code path
                    // should not be reachable in practice -- it fails fast
                    // rather than silently hanging if it somehow is.
                    last_error_ = "unexpected SEC_I_RENEGOTIATE (a peer sent a post-handshake TLS message; SCH_CRED_DISABLE_RECONNECTS should have prevented this)";
                    return false;
                }
                if (ss != SEC_E_OK) { last_error_ = "DecryptMessage: " + hresult_hex(ss); return false; }

                uint8_t* data_ptr = nullptr; unsigned long data_len = 0;
                uint8_t* extra_ptr = nullptr; unsigned long extra_len = 0;
                for (const SecBuffer& b : bufs) {
                    if (b.BufferType == SECBUFFER_DATA && !data_ptr) { data_ptr = static_cast<uint8_t*>(b.pvBuffer); data_len = b.cbBuffer; }
                    if (b.BufferType == SECBUFFER_EXTRA) { extra_ptr = static_cast<uint8_t*>(b.pvBuffer); extra_len = b.cbBuffer; }
                }
                if (data_ptr && data_len > 0) plaintext_buf_.insert(plaintext_buf_.end(), data_ptr, data_ptr + data_len);

                recv_buf_ = (extra_ptr && extra_len > 0)
                    ? std::vector<uint8_t>(extra_ptr, extra_ptr + extra_len)
                    : std::vector<uint8_t>();
                return true;
            }
        }

        // Blocks until at least n bytes of decrypted plaintext are
        // available (decrypting further records as needed), then hands
        // back exactly n bytes and keeps any remainder buffered.
        bool recv_exact(size_t n, std::vector<uint8_t>& out) noexcept {
            while (plaintext_buf_.size() < n) {
                if (!decrypt_one_record()) return false;
            }
            out.assign(plaintext_buf_.begin(), plaintext_buf_.begin() + n);
            plaintext_buf_.erase(plaintext_buf_.begin(), plaintext_buf_.begin() + n);
            return true;
        }

        // After AcceptSecurityContext/InitializeSecurityContext, checks
        // whether the trailing input buffer was retagged SECBUFFER_EXTRA
        // (unconsumed bytes belonging to the *next* handshake message,
        // already in hand) and if so keeps just those bytes; otherwise
        // clears the buffer once its contents have been fully consumed.
        // Returns true iff there is leftover SECBUFFER_EXTRA data.
        static bool consume_input_buffer(std::vector<uint8_t>& in_buf, const SecBuffer& trailing, SECURITY_STATUS ss) {
            if (trailing.BufferType == SECBUFFER_EXTRA && trailing.cbBuffer > 0) {
                std::vector<uint8_t> extra(in_buf.end() - trailing.cbBuffer, in_buf.end());
                in_buf = std::move(extra);
                return true;
            }
            if (ss != SEC_E_INCOMPLETE_MESSAGE) {
                in_buf.clear();
            }
            return false;
        }

        bool finish_handshake() {
            SECURITY_STATUS ss = QueryContextAttributesW(&ctxt_handle_, SECPKG_ATTR_STREAM_SIZES, &stream_sizes_);
            if (ss != SEC_E_OK) { last_error_ = "QueryContextAttributes(STREAM_SIZES): " + hresult_hex(ss); return false; }

            SecPkgContext_ConnectionInfo conn_info{};
            ss = QueryContextAttributesW(&ctxt_handle_, SECPKG_ATTR_CONNECTION_INFO, &conn_info);
            negotiated_protocol_ = (ss == SEC_E_OK) ? conn_info.dwProtocol : 0;

            PCCERT_CONTEXT peer = nullptr;
            ss = QueryContextAttributesW(&ctxt_handle_, SECPKG_ATTR_REMOTE_CERT_CONTEXT, &peer);
            if (ss != SEC_E_OK || !peer) {
                last_error_ = "peer did not present a verifiable certificate (mutual auth not satisfied): " + hresult_hex(ss);
                return false;
            }
            peer_cert_ = make_cert_ptr(peer);
            handshake_done_ = true;
            return true;
        }

        void send_close_notify() noexcept {
            DWORD type = SCHANNEL_SHUTDOWN;
            SecBuffer ctrl_buf{ sizeof(type), SECBUFFER_TOKEN, &type };
            SecBufferDesc ctrl_desc{ SECBUFFER_VERSION, 1, &ctrl_buf };
            ApplyControlToken(&ctxt_handle_, &ctrl_desc);

            SecBuffer out_buf{}; out_buf.BufferType = SECBUFFER_TOKEN;
            SecBufferDesc out_desc{ SECBUFFER_VERSION, 1, &out_buf };
            DWORD out_flags = 0;
            SECURITY_STATUS ss;
            if (is_server_) {
                DWORD context_req = ASC_REQ_SEQUENCE_DETECT | ASC_REQ_REPLAY_DETECT | ASC_REQ_CONFIDENTIALITY |
                    ASC_REQ_EXTENDED_ERROR | ASC_REQ_ALLOCATE_MEMORY | ASC_REQ_STREAM | ASC_REQ_MUTUAL_AUTH;
                ss = AcceptSecurityContext(&cred_handle_, &ctxt_handle_, nullptr, context_req, 0,
                    &ctxt_handle_, &out_desc, &out_flags, nullptr);
            }
            else {
                DWORD context_req = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY |
                    ISC_RET_EXTENDED_ERROR | ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM;
                wchar_t empty_target[] = L"";
                ss = InitializeSecurityContextW(&cred_handle_, &ctxt_handle_, empty_target, context_req, 0, 0,
                    nullptr, 0, nullptr, &out_desc, &out_flags, nullptr);
            }
            if (ss == SEC_E_OK && out_buf.cbBuffer > 0 && out_buf.pvBuffer) {
                socket_.send_all(reinterpret_cast<const uint8_t*>(out_buf.pvBuffer), out_buf.cbBuffer);
                FreeContextBuffer(out_buf.pvBuffer);
            }
        }

        TcpSocket socket_;
        CredHandle cred_handle_{};
        CtxtHandle ctxt_handle_{};
        bool is_server_ = false;
        bool handshake_done_ = false;
        SecPkgContext_StreamSizes stream_sizes_{};
        DWORD negotiated_protocol_ = 0;
        CertContextPtr peer_cert_;
        std::vector<uint8_t> recv_buf_;
        std::vector<uint8_t> plaintext_buf_;
        std::string last_error_;
    };

} // namespace transport
} // namespace animus
