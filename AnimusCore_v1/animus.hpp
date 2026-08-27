#pragma once
#include <cstdint>
#include <memory>
#include <string>

namespace animus {

    // Strictly aligned to 64 bytes to match standard CPU L1/L2 cache line boundaries
    struct alignas(64) TelemetryPayload {
        uint64_t timestamp_cycles;
        uint32_t event_id;
        uint32_t trace_id;
        uint64_t metric_value;
    };

    class Engine {
    public:
        virtual ~Engine() = default;
        virtual bool record(uint32_t event_id, uint32_t trace_id, uint64_t value) const noexcept = 0;
        virtual bool is_guard_active() const noexcept = 0;
        virtual void start_persistence(const std::string& log_filepath) = 0;
        virtual void stop_persistence() = 0;

        static std::unique_ptr<Engine> Create(size_t buffer_capacity = 65536);
    };

} // namespace animus 
// Append macro definition at top if not already defined:
#ifndef ANIMUS_API
#ifdef ANIMUS_EXPORTS
#define ANIMUS_API __declspec(dllexport)
#else
#define ANIMUS_API __declspec(dllimport)
#endif
#endif

// Add C-ABI Exports at the bottom of animus.hpp (below namespace animus)
extern "C" {
    ANIMUS_API bool animus_init(size_t buffer_capacity);
    ANIMUS_API bool animus_record_event(uint32_t event_id, uint32_t trace_id, uint64_t metric_value);
    ANIMUS_API void animus_start_logging(const char* filepath);
    ANIMUS_API void animus_stop_logging();
}