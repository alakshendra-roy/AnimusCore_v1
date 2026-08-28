#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <atomic>
#include <vector>
#include <chrono>

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#pragma intrinsic(__rdtsc)
#endif

namespace animus {

    // Strictly aligned to 64 bytes to match standard CPU L1/L2 cache line boundaries
    struct alignas(64) TelemetryPayload {
        uint64_t timestamp_cycles;
        uint32_t event_id;
        uint32_t trace_id;
        uint64_t metric_value;
    };

    // Low-overhead cycle counter for hot-path timestamping. Falls back to a
    // monotonic clock on platforms without an invariant TSC intrinsic.
    inline uint64_t read_cycle_counter() noexcept {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
        return __rdtsc();
#elif defined(__x86_64__) || defined(__i386__)
        return __builtin_ia32_rdtsc();
#else
        return static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
    }

    // Bounded lock-free MPMC ring buffer (Vyukov algorithm). Backing storage is
    // allocated once at construction; push()/pop() perform zero heap allocations
    // and never block, making them safe to call from multiple concurrent
    // producer threads (e.g. concurrent ctypes callers) and a single consumer.
    template <typename T>
    class LockFreeRingBuffer {
    public:
        explicit LockFreeRingBuffer(size_t requested_capacity)
            : capacity_(round_up_pow2(requested_capacity)),
            mask_(capacity_ - 1),
            cells_(capacity_) {
            for (size_t i = 0; i < capacity_; ++i) {
                cells_[i].sequence.store(i, std::memory_order_relaxed);
            }
        }

        LockFreeRingBuffer(const LockFreeRingBuffer&) = delete;
        LockFreeRingBuffer& operator=(const LockFreeRingBuffer&) = delete;

        bool push(const T& value) noexcept {
            Cell* cell;
            size_t pos = enqueue_pos_.load(std::memory_order_relaxed);
            for (;;) {
                cell = &cells_[pos & mask_];
                size_t seq = cell->sequence.load(std::memory_order_acquire);
                intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);
                if (diff == 0) {
                    if (enqueue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                        break;
                    }
                }
                else if (diff < 0) {
                    return false; // buffer full
                }
                else {
                    pos = enqueue_pos_.load(std::memory_order_relaxed);
                }
            }
            cell->data = value;
            cell->sequence.store(pos + 1, std::memory_order_release);
            return true;
        }

        bool pop(T& out) noexcept {
            Cell* cell;
            size_t pos = dequeue_pos_.load(std::memory_order_relaxed);
            for (;;) {
                cell = &cells_[pos & mask_];
                size_t seq = cell->sequence.load(std::memory_order_acquire);
                intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);
                if (diff == 0) {
                    if (dequeue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                        break;
                    }
                }
                else if (diff < 0) {
                    return false; // buffer empty
                }
                else {
                    pos = dequeue_pos_.load(std::memory_order_relaxed);
                }
            }
            out = cell->data;
            cell->sequence.store(pos + mask_ + 1, std::memory_order_release);
            return true;
        }

        size_t capacity() const noexcept { return capacity_; }

    private:
        struct Cell {
            std::atomic<size_t> sequence;
            T data;
        };

        static size_t round_up_pow2(size_t v) noexcept {
            size_t p = 1;
            while (p < v) p <<= 1;
            return p;
        }

        size_t capacity_;
        size_t mask_;
        std::vector<Cell> cells_;

        alignas(64) std::atomic<size_t> enqueue_pos_{ 0 };
        alignas(64) std::atomic<size_t> dequeue_pos_{ 0 };
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