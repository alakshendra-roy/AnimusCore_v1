#include "animus.hpp"
#include <atomic>
#include <vector>
#include <thread>
#include <fstream>
#include <chrono>

namespace animus {

    class EngineImpl final : public Engine {
    private:
        // Align atomic pointers to separate cache lines (64 bytes) to prevent false sharing
        alignas(64) mutable std::atomic<size_t> head_{ 0 };
        alignas(64) std::atomic<size_t> tail_{ 0 };

        size_t capacity_;
        size_t mask_;
        mutable std::vector<TelemetryPayload> buffer_;

        // Asynchronous Disk Worker Controls
        std::atomic<bool> running_{ false };
        std::thread worker_thread_;
        std::string log_file_path_;

        void process_persistence_queue() {
            std::ofstream log_file(log_file_path_, std::ios::out | std::ios::app | std::ios::binary);
            constexpr size_t BATCH_SIZE = 1024;
            std::vector<TelemetryPayload> batch;
            batch.reserve(BATCH_SIZE);

            while (running_.load(std::memory_order_relaxed) || tail_.load(std::memory_order_relaxed) < head_.load(std::memory_order_relaxed)) {
                size_t current_tail = tail_.load(std::memory_order_relaxed);
                size_t current_head = head_.load(std::memory_order_acquire);

                while (current_tail < current_head && batch.size() < BATCH_SIZE) {
                    batch.push_back(buffer_[current_tail & mask_]);
                    current_tail++;
                }

                if (!batch.empty()) {
                    log_file.write(reinterpret_cast<const char*>(batch.data()), batch.size() * sizeof(TelemetryPayload));
                    tail_.store(current_tail, std::memory_order_release);
                    batch.clear();
                }
                else {
                    std::this_thread::sleep_for(std::chrono::microseconds(50));
                }
            }
            if (log_file.is_open()) {
                log_file.flush();
                log_file.close();
            }
        }

    public:
        explicit EngineImpl(size_t capacity)
            : capacity_(capacity), mask_(capacity - 1), buffer_(capacity) {
        }

        ~EngineImpl() override {
            stop_persistence();
        }

        bool record(uint32_t event_id, uint32_t trace_id, uint64_t value) const noexcept override {
            size_t current_head = head_.load(std::memory_order_relaxed);
            size_t current_tail = tail_.load(std::memory_order_acquire);

            if (current_head - current_tail >= capacity_) {
                return false; // Buffer full
            }

            size_t index = current_head & mask_;
            buffer_[index] = TelemetryPayload{
                0, // Timestamps captured via CPU cycles
                event_id,
                trace_id,
                value
            };

            head_.store(current_head + 1, std::memory_order_release);
            return true;
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