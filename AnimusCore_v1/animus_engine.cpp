// DLL build entry point for the Python SDK (see animus/bindings.py).
//
// EngineImpl and Engine::Create() are now defined inline directly in
// animus.hpp (Phase 7: header-only engine), so this translation unit is
// nothing more than a thin C-ABI shim: it instantiates the header-only
// Engine and re-exposes it across extern "C" for ctypes to call. A C++
// caller with a hard latency budget (see animus::ExecutionClient) can skip
// this DLL and the C-ABI/ctypes marshalling cost entirely by including
// animus.hpp directly and driving animus::Engine in-process.
#ifndef ANIMUS_EXPORTS
#define ANIMUS_EXPORTS
#endif
#include "animus.hpp"
#include <memory>
#include <mutex>
#include <string>
#include <thread>

// Platform-specific thread-pinning headers live here, not in animus.hpp --
// keeping windows.h (a large header with a real macro-pollution footprint)
// out of the portable, header-only core is the same reasoning animus.hpp's
// own docstring gives for keeping animus_transport.hpp's Schannel includes
// out of it. Everything above this point in the file needing pinning is
// declared (not defined) in animus.hpp; the platform-specific bodies are
// defined only here.
#if defined(_WIN32)
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h> // animus_verify_license's MAC-address fingerprint component
#include <bcrypt.h>   // animus_verify_license's RSA signature verification + SHA-256
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "ws2_32.lib")
#endif

// The actual pin/priority mechanics (SetThreadAffinityMask/pthread_setaffinity_np,
// _mm_pause, etc.) now live in this standalone header, shared with every
// other native consumer (animus_cluster.hpp's RaftNode threads,
// animus_benchmark_suite.cpp's producer threads) instead of being
// duplicated per translation unit. animus_pin_current_thread_to_core /
// animus_set_thread_high_priority below are just the license-gated C-ABI
// shim around it.
#include "../include/animus/thread_affinity.hpp"
#include "../include/animus/shm_ipc.hpp"
#include "animus_security.hpp"

// Only used by animus_verify_license (Windows-only, see its definition
// below) -- kept out of animus.hpp's own includes for the same "portable
// core header stays free of heavy platform-specific machinery" reasoning
// as the windows.h/iphlpapi.h/bcrypt.h includes just above.
#include <array>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <fstream>
#include <vector>

// GENERATED public key, embedded at build time -- see
// license_tools/generate_license_keypair.ps1. Not present until that
// script has been run at least once; animus_verify_license simply won't
// build without it, which is deliberate (there is no meaningful
// "license verification with no key to verify against").
#include "animus_license_pubkey.hpp"

static std::unique_ptr<animus::Engine> g_engine = nullptr;
static std::mutex g_init_mutex;

static std::unique_ptr<animus::SpscRingBuffer<animus::TelemetryPayload>> g_spsc_ring = nullptr;
static std::mutex g_spsc_init_mutex;

// License entitlement state -- set once by a successful animus_verify_license
// call, read by animus_pin_current_thread_to_core / animus_spsc_init before
// either does anything else. std::atomic rather than a mutex: these are read
// on every gated call (a hot-ish path for pinning in particular), and a
// plain load/store pair is all correctness here needs -- animus_verify_license
// itself is expected to run once at startup, not concurrently with the
// functions it gates.
static std::atomic<bool> g_license_verified{ false };
static std::atomic<uint32_t> g_license_max_cores{ 0 };

#if defined(_WIN32)
namespace {

    // Reads HKLM\SOFTWARE\Microsoft\Cryptography\MachineGuid -- see
    // animus::LicensePayload's docstring (animus.hpp) for why this,
    // not a literal CPU serial number, is the real fingerprint component
    // "CPU GUID" refers to here.
    bool read_machine_guid(std::string& out) {
        HKEY key;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Cryptography", 0,
            KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) {
            return false;
        }
        char buf[64] = {};
        DWORD size = sizeof(buf);
        DWORD type = 0;
        bool ok = RegQueryValueExA(key, "MachineGuid", nullptr, &type, reinterpret_cast<BYTE*>(buf), &size)
            == ERROR_SUCCESS && type == REG_SZ;
        RegCloseKey(key);
        if (ok) out.assign(buf);
        return ok;
    }

    // Picks one real, manufacturer-assigned MAC address deterministically.
    // "First enumerated adapter" is not safe here: a single machine can
    // present several MAC addresses for entirely legitimate reasons (this
    // exact machine, during development, exposed a real Ethernet MAC, a
    // real Wi-Fi MAC, AND two additional Wi-Fi-Direct/hosted-network
    // virtual-role MACs derived from that same radio -- confirmed with a
    // real multi-adapter machine, not assumed). The IEEE "locally
    // administered address" bit (bit 1 of the first octet) is 0 for a
    // genuine burned-in hardware address and 1 for anything
    // derived/virtual/randomized -- Windows sets it deliberately on
    // exactly those derived addresses specifically so they can be told
    // apart this way, which is what this filters on, before picking the
    // lexicographically smallest remaining candidate for a deterministic
    // choice on multi-NIC machines. license_tools/sign_license.ps1
    // implements this identical rule independently (via .NET's
    // NetworkInterface, confirmed to enumerate the same underlying
    // adapter set as GetAdaptersAddresses does here) -- the two were
    // cross-checked to produce byte-identical fingerprints for the same
    // real machine before either was relied on.
    bool read_primary_mac(unsigned char out[6]) {
        ULONG buf_len = 15000;
        std::vector<unsigned char> buf(buf_len);
        ULONG rc = GetAdaptersAddresses(AF_UNSPEC,
            GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
            nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &buf_len);
        if (rc == ERROR_BUFFER_OVERFLOW) {
            buf.resize(buf_len);
            rc = GetAdaptersAddresses(AF_UNSPEC,
                GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &buf_len);
        }
        if (rc != NO_ERROR) return false;

        std::vector<std::array<unsigned char, 6>> candidates;
        for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()); a; a = a->Next) {
            if (a->PhysicalAddressLength != 6 || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            bool all_zero = true;
            for (int i = 0; i < 6; ++i) if (a->PhysicalAddress[i] != 0) { all_zero = false; break; }
            if (all_zero || (a->PhysicalAddress[0] & 0x02)) continue; // locally administered -- skip
            std::array<unsigned char, 6> mac{};
            memcpy(mac.data(), a->PhysicalAddress, 6);
            candidates.push_back(mac);
        }
        if (candidates.empty()) return false;
        std::sort(candidates.begin(), candidates.end());
        memcpy(out, candidates.front().data(), 6);
        return true;
    }

    bool sha256(const unsigned char* data, size_t len, unsigned char out[32]) {
        BCRYPT_ALG_HANDLE alg = nullptr;
        if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return false;
        BCRYPT_HASH_HANDLE hash = nullptr;
        bool ok = BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0
            && BCryptHashData(hash, const_cast<unsigned char*>(data), static_cast<ULONG>(len), 0) == 0
            && BCryptFinishHash(hash, out, 32, 0) == 0;
        if (hash) BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(alg, 0);
        return ok;
    }

    bool compute_machine_fingerprint(unsigned char out[32]) {
        std::string guid;
        unsigned char mac[6];
        if (!read_machine_guid(guid) || !read_primary_mac(mac)) return false;
        std::vector<unsigned char> input(guid.begin(), guid.end());
        input.insert(input.end(), mac, mac + 6);
        return sha256(input.data(), input.size(), out);
    }

    bool rsa_verify(const unsigned char* payload_bytes, size_t payload_len,
        const unsigned char* signature, size_t signature_len) {
        unsigned char hash[32];
        if (!sha256(payload_bytes, payload_len, hash)) return false;

        BCRYPT_ALG_HANDLE rsa_alg = nullptr;
        if (BCryptOpenAlgorithmProvider(&rsa_alg, BCRYPT_RSA_ALGORITHM, nullptr, 0) != 0) return false;
        BCRYPT_KEY_HANDLE key = nullptr;
        NTSTATUS st = BCryptImportKeyPair(rsa_alg, nullptr, BCRYPT_RSAPUBLIC_BLOB, &key,
            const_cast<unsigned char*>(animus_license::kPublicKeyBlob), animus_license::kPublicKeyBlobSize, 0);
        if (st != 0) {
            BCryptCloseAlgorithmProvider(rsa_alg, 0);
            return false;
        }

        BCRYPT_PKCS1_PADDING_INFO padding{};
        padding.pszAlgId = BCRYPT_SHA256_ALGORITHM;
        st = BCryptVerifySignature(key, &padding, hash, sizeof(hash),
            const_cast<unsigned char*>(signature), static_cast<ULONG>(signature_len), BCRYPT_PAD_PKCS1);

        BCryptDestroyKey(key);
        BCryptCloseAlgorithmProvider(rsa_alg, 0);
        return st == 0;
    }

    // Shared implementation behind both animus_verify_license (collapses
    // this to a bool) and animus_check_license_status (returns this
    // directly) -- one code path, not two that could drift apart. Sets the
    // process-wide entitlement state on success, same as the old
    // animus_verify_license body this was factored out of.
    animus::LicenseStatus compute_license_status(const char* license_path) {
        if (!license_path) return animus::LicenseStatus::Missing;

        std::ifstream file(license_path, std::ios::binary);
        if (!file) return animus::LicenseStatus::Missing;
        std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (bytes.size() != animus::kLicenseFileSize) return animus::LicenseStatus::Malformed;

        animus::LicensePayload payload{};
        memcpy(&payload, bytes.data(), sizeof(payload));
        if (payload.magic != animus::kLicenseMagic) return animus::LicenseStatus::Malformed;

        const unsigned char* signature = bytes.data() + sizeof(animus::LicensePayload);
        if (!rsa_verify(bytes.data(), sizeof(animus::LicensePayload), signature, animus::kLicenseSignatureSize)) {
            return animus::LicenseStatus::BadSignature; // tampered payload, or not signed by the key this build embeds
        }

        if (payload.expires_at_unix != 0) {
            auto now = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            if (static_cast<uint64_t>(now) > payload.expires_at_unix) return animus::LicenseStatus::Expired;
        }

        unsigned char my_fingerprint[32];
        if (!compute_machine_fingerprint(my_fingerprint)) return animus::LicenseStatus::UnsupportedPlatform;
        if (memcmp(my_fingerprint, payload.fingerprint_sha256, 32) != 0) {
            return animus::LicenseStatus::WrongMachine; // validly signed, but issued for a different machine
        }

        g_license_max_cores.store(payload.max_cores, std::memory_order_release);
        g_license_verified.store(true, std::memory_order_release);
        return animus::LicenseStatus::Valid;
    }

} // namespace
#endif // _WIN32

namespace {

    // Bundles one TenantRegistry + one SecureTelemetryGateway + one
    // SecureExecutionGateway behind a single C-ABI handle -- Python drives
    // one animus_security_create_context() object, not three separate
    // handles it would otherwise have to keep in sync itself. Not part of
    // animus_security.hpp: this is a C-ABI convenience aggregate, not a
    // reusable C++ abstraction (same reasoning g_engine/g_spsc_ring above
    // are DLL-shim-only, not in the portable headers).
    struct SecurityContext {
        animus::security::TenantRegistry registry;
        animus::security::SecureTelemetryGateway telemetry;
        animus::security::SecureExecutionGateway execution;

        SecurityContext() : telemetry(registry), execution(registry) {}
    };

} // namespace

extern "C" {
    // Cold path: guarded by a mutex since it runs once at startup. The hot
    // record/logging paths below never take a lock.
    ANIMUS_API bool animus_init(size_t buffer_capacity) {
        std::lock_guard<std::mutex> lock(g_init_mutex);
        if (!g_engine) {
            g_engine = animus::Engine::Create(buffer_capacity);
        }
        return g_engine != nullptr;
    }

    ANIMUS_API bool animus_record_event(uint32_t event_id, uint32_t trace_id, uint64_t metric_value) {
        animus::Engine* engine = g_engine.get();
        if (!engine) return false;
        return engine->record(event_id, trace_id, metric_value);
    }

    ANIMUS_API size_t animus_record_events_batch(const animus::RawEvent* events, size_t count) {
        animus::Engine* engine = g_engine.get();
        if (!engine || !events) return 0;
        return engine->record_batch(events, count);
    }

    ANIMUS_API void animus_start_logging(const char* filepath) {
        animus::Engine* engine = g_engine.get();
        if (engine && filepath) {
            engine->start_persistence(std::string(filepath));
        }
    }

    ANIMUS_API void animus_stop_logging() {
        animus::Engine* engine = g_engine.get();
        if (engine) {
            engine->stop_persistence();
        }
    }

    ANIMUS_API bool animus_add_rule(uint32_t rule_id, uint32_t event_id, uint64_t threshold, uint8_t comparator, uint32_t severity) {
        animus::Engine* engine = g_engine.get();
        if (!engine) return false;
        return engine->add_rule(rule_id, event_id, threshold, comparator, severity);
    }

    ANIMUS_API bool animus_add_cep_rule(uint32_t rule_id, uint32_t event_id, uint8_t window_type, uint64_t window_size,
        uint8_t aggregation, uint8_t comparator, uint64_t threshold, uint32_t severity) {
        animus::Engine* engine = g_engine.get();
        if (!engine) return false;
        return engine->add_cep_rule(rule_id, event_id, window_type, window_size, aggregation, comparator, threshold, severity);
    }

    ANIMUS_API size_t animus_poll_signals(animus::ThreatSignal* out, size_t max_count) {
        animus::Engine* engine = g_engine.get();
        if (!engine || !out) return 0;
        return engine->poll_signals(out, max_count);
    }

    // Cold path, same mutex-guarded lazy-init pattern as animus_init above --
    // fully independent singleton, not the same ring as g_engine's. Gated
    // on a verified license (proprietary-edition entitlement, see
    // animus_verify_license) -- fails closed, not open, if no license has
    // been verified in this process yet.
    ANIMUS_API bool animus_spsc_init(size_t capacity) {
        if (!g_license_verified.load(std::memory_order_acquire)) return false;
        std::lock_guard<std::mutex> lock(g_spsc_init_mutex);
        if (!g_spsc_ring) {
            g_spsc_ring = std::make_unique<animus::SpscRingBuffer<animus::TelemetryPayload>>(capacity);
        }
        return g_spsc_ring != nullptr;
    }

    // Hot path: producer-thread-only, per SpscRingBuffer's contract (see
    // animus.hpp). Same batch semantics as animus_record_events_batch --
    // stops at the first push that fails, returns the count actually pushed.
    ANIMUS_API size_t animus_spsc_record_events_batch(const animus::RawEvent* events, size_t count) {
        auto* ring = g_spsc_ring.get();
        if (!ring || !events) return 0;
        size_t pushed = 0;
        for (size_t i = 0; i < count; ++i) {
            animus::TelemetryPayload payload{
                animus::read_cycle_counter(),
                events[i].event_id,
                events[i].trace_id,
                events[i].metric_value
            };
            if (!ring->push(payload)) {
                break; // full; no concurrent consumer will free space within this call
            }
            ++pushed;
        }
        return pushed;
    }

    // Consumer-thread-only, per SpscRingBuffer's contract.
    ANIMUS_API size_t animus_spsc_drain(animus::TelemetryPayload* out, size_t max_count) {
        auto* ring = g_spsc_ring.get();
        if (!ring || !out) return 0;
        size_t count = 0;
        while (count < max_count && ring->pop(out[count])) {
            ++count;
        }
        return count;
    }

    // Gated on a verified license: core_id must be strictly less than the
    // license's entitled max_cores (a license granting max_cores=4 allows
    // pinning to cores 0-3, same "N entitlements, IDs 0..N-1" convention
    // animus_get_cpu_count's own callers already use for core counts).
    // Fails closed if no license has been verified in this process yet --
    // there is no unlicensed default (not even core 0).
    ANIMUS_API bool animus_pin_current_thread_to_core(int core_id) {
        if (core_id < 0) return false;
        if (!g_license_verified.load(std::memory_order_acquire)) return false;
        if (static_cast<uint32_t>(core_id) >= g_license_max_cores.load(std::memory_order_acquire)) return false;
        return animus::sys::pin_current_thread_to_core(static_cast<size_t>(core_id));
    }

    // Same entitlement gate as animus_pin_current_thread_to_core -- realtime/
    // high-priority scheduling is the same class of "take more of the host's
    // scheduler than a default-priority process gets" capability as hard core
    // pinning, so it is licensed identically: fails closed with no verified
    // license, independent of core_id/max_cores (there's no per-core count to
    // check here, just whether this install is entitled at all). Best-effort
    // like animus::sys::set_thread_high_priority() itself -- always returns,
    // never throws, so a caller on a host that denies realtime scheduling
    // (e.g. no CAP_SYS_NICE) still runs, just without the priority boost.
    ANIMUS_API void animus_set_thread_high_priority(void) {
        if (!g_license_verified.load(std::memory_order_acquire)) return;
        animus::sys::set_thread_high_priority();
    }

    ANIMUS_API unsigned animus_get_cpu_count(void) {
        unsigned n = std::thread::hardware_concurrency();
        return n > 0 ? n : 1;
    }

    ANIMUS_API void* animus_shm_create(const char* name, uint64_t capacity) {
        if (!name) return nullptr;
        return animus::SharedTelemetryChannel::create(name, capacity).release();
    }

    ANIMUS_API void* animus_shm_attach(const char* name) {
        if (!name) return nullptr;
        return animus::SharedTelemetryChannel::attach(name).release();
    }

    ANIMUS_API void animus_shm_close(void* channel) {
        delete static_cast<animus::SharedTelemetryChannel*>(channel);
    }

    ANIMUS_API bool animus_shm_unlink(const char* name) {
        if (!name) return false;
        return animus::SharedTelemetryChannel::unlink(name);
    }

    ANIMUS_API uint64_t animus_shm_capacity(void* channel) {
        if (!channel) return 0;
        return static_cast<animus::SharedTelemetryChannel*>(channel)->capacity();
    }

    ANIMUS_API bool animus_shm_push(void* channel, uint32_t event_id, uint32_t trace_id, uint64_t metric_value) {
        if (!channel) return false;
        return static_cast<animus::SharedTelemetryChannel*>(channel)->push(event_id, trace_id, metric_value);
    }

    ANIMUS_API bool animus_shm_pop(void* channel, animus::SharedTelemetryRecord* out) {
        if (!channel || !out) return false;
        return static_cast<animus::SharedTelemetryChannel*>(channel)->pop(*out);
    }

    ANIMUS_API void* animus_feed_create(size_t l2_capacity, size_t trade_capacity) {
        return animus::MarketDataFeed::create(l2_capacity, trade_capacity).release();
    }

    ANIMUS_API void animus_feed_close(void* feed) {
        delete static_cast<animus::MarketDataFeed*>(feed);
    }

    ANIMUS_API size_t animus_feed_l2_capacity(void* feed) {
        if (!feed) return 0;
        return static_cast<animus::MarketDataFeed*>(feed)->l2_capacity();
    }

    ANIMUS_API size_t animus_feed_trade_capacity(void* feed) {
        if (!feed) return 0;
        return static_cast<animus::MarketDataFeed*>(feed)->trade_capacity();
    }

    ANIMUS_API bool animus_feed_push_l2_update(void* feed, uint32_t instrument_id, uint8_t side, uint8_t action,
        uint32_t level, uint64_t price_ticks, uint64_t quantity, uint64_t sequence_number, uint64_t exchange_timestamp_ns) {
        if (!feed) return false;
        return static_cast<animus::MarketDataFeed*>(feed)->push_l2_update(
            instrument_id, side, action, level, price_ticks, quantity, sequence_number, exchange_timestamp_ns);
    }

    ANIMUS_API bool animus_feed_push_trade(void* feed, uint32_t instrument_id, uint64_t trade_id, uint8_t aggressor_side,
        uint64_t price_ticks, uint64_t quantity, uint64_t sequence_number, uint64_t exchange_timestamp_ns) {
        if (!feed) return false;
        return static_cast<animus::MarketDataFeed*>(feed)->push_trade(
            instrument_id, trade_id, aggressor_side, price_ticks, quantity, sequence_number, exchange_timestamp_ns);
    }

    ANIMUS_API size_t animus_feed_poll_l2_updates(void* feed, animus::L2Update* out, size_t max_count) {
        if (!feed || !out) return 0;
        return static_cast<animus::MarketDataFeed*>(feed)->poll_l2_updates(out, max_count);
    }

    ANIMUS_API size_t animus_feed_poll_trades(void* feed, animus::TradeTick* out, size_t max_count) {
        if (!feed || !out) return 0;
        return static_cast<animus::MarketDataFeed*>(feed)->poll_trades(out, max_count);
    }

    ANIMUS_API void* animus_shm_ring_create(const char* name, size_t requested_capacity) {
        if (!name) return nullptr;
        return animus::sys::ipc::ShmRing<animus::RawEvent>::create(name, requested_capacity).release();
    }

    ANIMUS_API void* animus_shm_ring_open(const char* name) {
        if (!name) return nullptr;
        return animus::sys::ipc::ShmRing<animus::RawEvent>::open(name).release();
    }

    ANIMUS_API void animus_shm_ring_close(void* ring) {
        delete static_cast<animus::sys::ipc::ShmRing<animus::RawEvent>*>(ring);
    }

    ANIMUS_API bool animus_shm_ring_unlink(const char* name) {
        if (!name) return false;
        return animus::sys::ipc::ShmRing<animus::RawEvent>::unlink(name);
    }

    ANIMUS_API size_t animus_shm_ring_capacity(void* ring) {
        if (!ring) return 0;
        return static_cast<animus::sys::ipc::ShmRing<animus::RawEvent>*>(ring)->capacity();
    }

    ANIMUS_API bool animus_shm_ring_try_push(void* ring, const animus::RawEvent* event) {
        if (!ring || !event) return false;
        return static_cast<animus::sys::ipc::ShmRing<animus::RawEvent>*>(ring)->try_push(*event);
    }

    ANIMUS_API bool animus_shm_ring_try_pop(void* ring, animus::RawEvent* out) {
        if (!ring || !out) return false;
        return static_cast<animus::sys::ipc::ShmRing<animus::RawEvent>*>(ring)->try_pop(*out);
    }

    ANIMUS_API size_t animus_shm_ring_push_batch(void* ring, const animus::RawEvent* events, size_t count) {
        if (!ring || !events) return 0;
        auto* r = static_cast<animus::sys::ipc::ShmRing<animus::RawEvent>*>(ring);
        size_t pushed = 0;
        for (; pushed < count; ++pushed) {
            if (!r->try_push(events[pushed])) break;
        }
        return pushed;
    }

    ANIMUS_API size_t animus_shm_ring_pop_batch(void* ring, animus::RawEvent* out, size_t max_count) {
        if (!ring || !out) return 0;
        auto* r = static_cast<animus::sys::ipc::ShmRing<animus::RawEvent>*>(ring);
        size_t popped = 0;
        for (; popped < max_count; ++popped) {
            if (!r->try_pop(out[popped])) break;
        }
        return popped;
    }

    ANIMUS_API void* animus_shm_ring_order_create(const char* name, size_t requested_capacity) {
        if (!name) return nullptr;
        return animus::sys::ipc::ShmRing<animus::OrderRequest>::create(name, requested_capacity).release();
    }

    ANIMUS_API void* animus_shm_ring_order_open(const char* name) {
        if (!name) return nullptr;
        return animus::sys::ipc::ShmRing<animus::OrderRequest>::open(name).release();
    }

    ANIMUS_API void animus_shm_ring_order_close(void* ring) {
        delete static_cast<animus::sys::ipc::ShmRing<animus::OrderRequest>*>(ring);
    }

    ANIMUS_API bool animus_shm_ring_order_unlink(const char* name) {
        if (!name) return false;
        return animus::sys::ipc::ShmRing<animus::OrderRequest>::unlink(name);
    }

    ANIMUS_API size_t animus_shm_ring_order_capacity(void* ring) {
        if (!ring) return 0;
        return static_cast<animus::sys::ipc::ShmRing<animus::OrderRequest>*>(ring)->capacity();
    }

    ANIMUS_API bool animus_shm_ring_order_try_push(void* ring, const animus::OrderRequest* order) {
        if (!ring || !order) return false;
        return static_cast<animus::sys::ipc::ShmRing<animus::OrderRequest>*>(ring)->try_push(*order);
    }

    ANIMUS_API bool animus_shm_ring_order_try_pop(void* ring, animus::OrderRequest* out) {
        if (!ring || !out) return false;
        return static_cast<animus::sys::ipc::ShmRing<animus::OrderRequest>*>(ring)->try_pop(*out);
    }

    ANIMUS_API size_t animus_shm_ring_order_push_batch(void* ring, const animus::OrderRequest* orders, size_t count) {
        if (!ring || !orders) return 0;
        auto* r = static_cast<animus::sys::ipc::ShmRing<animus::OrderRequest>*>(ring);
        size_t pushed = 0;
        for (; pushed < count; ++pushed) {
            if (!r->try_push(orders[pushed])) break;
        }
        return pushed;
    }

    ANIMUS_API size_t animus_shm_ring_order_pop_batch(void* ring, animus::OrderRequest* out, size_t max_count) {
        if (!ring || !out) return 0;
        auto* r = static_cast<animus::sys::ipc::ShmRing<animus::OrderRequest>*>(ring);
        size_t popped = 0;
        for (; popped < max_count; ++popped) {
            if (!r->try_pop(out[popped])) break;
        }
        return popped;
    }

    ANIMUS_API bool animus_verify_license(const char* license_path) {
#if defined(_WIN32)
        return compute_license_status(license_path) == animus::LicenseStatus::Valid;
#else
        // No implementation on this platform yet -- see animus.hpp's
        // declaration for why returning false here (never faking success)
        // is the deliberate choice, same as animus_pin_current_thread_to_core.
        (void)license_path;
        return false;
#endif
    }

    ANIMUS_API animus::LicenseStatus animus_check_license_status(const char* license_path) {
#if defined(_WIN32)
        return compute_license_status(license_path);
#else
        (void)license_path;
        return animus::LicenseStatus::UnsupportedPlatform;
#endif
    }

    ANIMUS_API bool animus_is_licensed(void) {
        return g_license_verified.load(std::memory_order_acquire);
    }

    ANIMUS_API uint32_t animus_licensed_max_cores(void) {
        return g_license_verified.load(std::memory_order_acquire) ? g_license_max_cores.load(std::memory_order_acquire) : 0;
    }

    ANIMUS_API void* animus_security_create_context(void) {
        return new SecurityContext();
    }

    ANIMUS_API void animus_security_close_context(void* ctx) {
        delete static_cast<SecurityContext*>(ctx);
    }

    ANIMUS_API bool animus_security_create_tenant(void* ctx, const animus::security::AccessToken* token,
        uint32_t new_tenant_id, size_t buffer_capacity) {
        if (!ctx || !token) return false;
        auto* sc = static_cast<SecurityContext*>(ctx);
        return sc->telemetry.create_tenant(*token, new_tenant_id, buffer_capacity);
    }

    ANIMUS_API bool animus_security_create_execution_tenant(void* ctx, const animus::security::AccessToken* token,
        uint32_t tenant_id) {
        if (!ctx || !token) return false;
        auto* sc = static_cast<SecurityContext*>(ctx);
        return sc->execution.create_execution_tenant(*token, tenant_id);
    }

    ANIMUS_API bool animus_security_submit_order(void* ctx, const animus::security::AccessToken* token,
        const animus::OrderRequest* request, animus::ExecutionReport* out) {
        if (!ctx || !token || !request || !out) return false;
        auto* sc = static_cast<SecurityContext*>(ctx);
        return sc->execution.submit(*token, *request, *out);
    }

    ANIMUS_API size_t animus_security_poll_execution_audit_log(void* ctx, animus::security::AuditEvent* out, size_t max_count) {
        if (!ctx || !out) return 0;
        auto* sc = static_cast<SecurityContext*>(ctx);
        return sc->execution.poll_execution_audit_log(out, max_count);
    }

    ANIMUS_API void animus_security_set_execution_license_required(void* ctx, bool required) {
        if (!ctx) return;
        auto* sc = static_cast<SecurityContext*>(ctx);
        sc->execution.set_execution_license_required(required);
    }
}
