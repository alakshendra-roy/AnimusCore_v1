#include "animus.hpp"
#include <vector>
#include <atomic>
#include <intrin.h>

namespace animus {

    template <typename T>
    class LockFreeRingBuffer {
    private:
        std::vector<T> buffer_;
        const size_t capacity_;
        const size_t mask_;
        alignas(64) std::atomic<size_t> head_{ 0 };
        alignas(64) std::atomic<size_t> tail_{ 0 };

    public:
        explicit LockFreeRingBuffer(size_t capacity)
            : capacity_(capacity), mask_(capacity - 1), buffer_(capacity) {
        }

        inline bool push(const T& item) noexcept {
            const size_t current_tail = tail_.load(std::memory_order_relaxed);
            const size_t current_head = head_.load(std::memory_order_acquire);

            if (current_tail - current_head >= capacity_) {
                return false;
            }

            buffer_[current_tail & mask_] = item;
            tail_.store(current_tail + 1, std::memory_order_release);
            return true;
        }
    };

    class ConcreteEngine final : public Engine {
    private:
        LockFreeRingBuffer<TelemetryPayload> ring_buffer_;
        std::atomic<bool> guard_status_{ true };

    public:
        explicit ConcreteEngine(size_t capacity) : ring_buffer_(capacity) {}

        bool record(uint32_t event_id, uint32_t trace_id, uint64_t value) noexcept override {
            uint64_t tsc = __rdtsc();
            TelemetryPayload payload{ tsc, event_id, trace_id, value };
            return ring_buffer_.push(payload);
        }

        bool is_guard_active() const noexcept override {
            return guard_status_.load(std::memory_order_relaxed);
        }
    };

    std::unique_ptr<Engine> Engine::Create(size_t buffer_capacity) {
        return std::make_unique<ConcreteEngine>(buffer_capacity);
    }

} // namespace animus