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

static std::unique_ptr<animus::Engine> g_engine = nullptr;
static std::mutex g_init_mutex;

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

    ANIMUS_API size_t animus_poll_signals(animus::ThreatSignal* out, size_t max_count) {
        animus::Engine* engine = g_engine.get();
        if (!engine || !out) return 0;
        return engine->poll_signals(out, max_count);
    }
}
