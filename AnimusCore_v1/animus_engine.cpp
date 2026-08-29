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
#include <windows.h>
#elif defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

static std::unique_ptr<animus::Engine> g_engine = nullptr;
static std::mutex g_init_mutex;

static std::unique_ptr<animus::SpscRingBuffer<animus::TelemetryPayload>> g_spsc_ring = nullptr;
static std::mutex g_spsc_init_mutex;

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
    // fully independent singleton, not the same ring as g_engine's.
    ANIMUS_API bool animus_spsc_init(size_t capacity) {
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

    ANIMUS_API bool animus_pin_current_thread_to_core(int core_id) {
        if (core_id < 0) return false;
#if defined(_WIN32)
        // SetThreadAffinityMask's mask is machine-word width (64 bits on
        // x64); a core_id at or beyond that can never be expressed.
        if (core_id >= 64) return false;
        DWORD_PTR mask = (DWORD_PTR)1 << core_id;
        return SetThreadAffinityMask(GetCurrentThread(), mask) != 0;
#elif defined(__linux__)
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(core_id, &cpuset);
        return pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) == 0;
#else
        // No portable hard-pinning API on this platform (e.g. macOS's
        // thread_policy_set/THREAD_AFFINITY_POLICY is an affinity *hint*
        // the scheduler is free to ignore, not a real pin -- claiming
        // success here would be dishonest about what actually happened).
        (void)core_id;
        return false;
#endif
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
}
