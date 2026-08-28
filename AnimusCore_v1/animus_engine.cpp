#include "animus.hpp"
#include <thread>
#include <fstream>
#include <chrono>

namespace animus {

    class EngineImpl final : public Engine {
    private:
        // Lock-free MPMC ring buffer: safe for concurrent producer threads
        // (e.g. multiple Python threads calling into the C-ABI concurrently)
        // paired with the single background persistence consumer below.
        mutable LockFreeRingBuffer<TelemetryPayload> ring_;

        // Asynchronous Disk Worker Controls
        std::atomic<bool> running_{ false };
        std::thread worker_thread_;
        std::string log_file_path_;

        // Spool loop: pop() and push() are both lock-free/non-blocking, so this
        // thread never contends with producer threads for a lock. If the sink
        // file can't be opened (bad path, permissions), events are left in the
        // ring buffer rather than silently discarded -- the buffer will simply
        // fill and record() will start returning false until the sink recovers
        // or shutdown is requested.
        void process_persistence_queue() {
            constexpr size_t BATCH_SIZE = 1024;
            constexpr auto OPEN_RETRY_INTERVAL = std::chrono::milliseconds(200);
            std::vector<TelemetryPayload> batch;
            batch.reserve(BATCH_SIZE);
            TelemetryPayload item;

            std::ofstream log_file;
            auto try_open = [&]() {
                log_file.open(log_file_path_, std::ios::out | std::ios::app | std::ios::binary);
                return log_file.is_open();
            };

            bool sink_ready = try_open();
            auto last_open_attempt = std::chrono::steady_clock::now();

            for (;;) {
                if (!sink_ready) {
                    if (!running_.load(std::memory_order_acquire)) {
                        break; // stop requested before a sink was ever available
                    }
                    auto now = std::chrono::steady_clock::now();
                    if (now - last_open_attempt >= OPEN_RETRY_INTERVAL) {
                        sink_ready = try_open();
                        last_open_attempt = now;
                    }
                    std::this_thread::sleep_for(std::chrono::microseconds(50));
                    continue;
                }

                while (batch.size() < BATCH_SIZE && ring_.pop(item)) {
                    batch.push_back(item);
                }

                if (!batch.empty()) {
                    log_file.write(reinterpret_cast<const char*>(batch.data()), batch.size() * sizeof(TelemetryPayload));
                    log_file.flush(); // bound data loss to the in-flight batch on a hard crash
                    if (!log_file.good()) {
                        sink_ready = false; // sink failed mid-run (e.g. disk full); fall back to retrying
                        log_file.close();
                    }
                    batch.clear();
                    continue;
                }

                if (!running_.load(std::memory_order_acquire)) {
                    break; // stop requested and ring buffer fully drained
                }

                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
            if (log_file.is_open()) {
                log_file.flush();
                log_file.close();
            }
        }

    public:
        explicit EngineImpl(size_t capacity)
            : ring_(capacity) {
        }

        ~EngineImpl() override {
            stop_persistence();
        }

        bool record(uint32_t event_id, uint32_t trace_id, uint64_t value) const noexcept override {
            TelemetryPayload payload{
                read_cycle_counter(),
                event_id,
                trace_id,
                value
            };
            return ring_.push(payload);
        }

        bool is_guard_active() const noexcept override {
            return true;
        }

        void start_persistence(const std::string& log_filepath) override {
            if (running_.load()) return;
            log_file_path_ = log_filepath;
            running_.store(true);
            worker_thread_ = std::thread(&EngineImpl::process_persistence_queue, this);
        }

        void stop_persistence() override {
            if (!running_.load()) return;
            running_.store(false);
            if (worker_thread_.joinable()) {
                worker_thread_.join();
            }
        }
    };

    std::unique_ptr<Engine> Engine::Create(size_t buffer_capacity) {
        return std::make_unique<EngineImpl>(buffer_capacity);
    }

} // namespace animus

#ifndef ANIMUS_EXPORTS
#define ANIMUS_EXPORTS  // Ensures functions are marked as dllexport in this translation unit
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
}

