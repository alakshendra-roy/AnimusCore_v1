#pragma once
// Cross-platform CPU thread pinning + cache-line alignment utilities.
//
// Header-only and dependency-free (beyond the platform SDK/libc headers
// pulled in below) so any translation unit in the engine can include this
// next to animus.hpp without a separate .cpp to build or link. Every entry
// point here is noexcept and returns a bool/void status rather than
// throwing: this module exists to be called from hot, latency-sensitive
// threads (market data readers, matching loops, spin-polling consumers),
// and an exception unwinding out of a pinning call is worse for those
// threads than a pin silently not taking effect.
//
// Platform coverage:
//   - Windows (MSVC): SetThreadAffinityMask / SetThreadIdealProcessor,
//     SetThreadPriority / SetPriorityClass.
//   - Linux (GCC/Clang, glibc): pthread_setaffinity_np, sched_setscheduler
//     (SCHED_FIFO) with a nice-value fallback when real-time scheduling
//     is denied (no CAP_SYS_NICE).
//   - Anything else: compiles, but pinning/priority calls are no-ops that
//     report failure -- callers on hot paths already have to check the
//     bool return, so an unsupported platform degrades gracefully instead
//     of failing to build.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <thread>

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
    #define NOMINMAX
    #endif
    #include <windows.h>
#elif defined(__linux__)
    #include <cerrno>
    #include <pthread.h>
    #include <sched.h>
    #include <sys/resource.h>
    #include <unistd.h>
#endif

// x86/x64 SIMD intrinsics header for _mm_pause(). Both MSVC and GCC/Clang
// expose it via <immintrin.h>.
#if defined(_M_IX86) || defined(_M_X64) || defined(__i386__) || defined(__x86_64__)
    #include <immintrin.h>
#endif
// MSVC's ARM/ARM64 __yield() intrinsic lives in <intrin.h>; GCC/Clang on
// ARM instead emit the "yield" instruction via inline asm (below), so no
// extra header is needed there.
#if defined(_MSC_VER) && (defined(_M_ARM) || defined(_M_ARM64))
    #include <intrin.h>
#endif

// Cache line size: 64 bytes covers essentially all x86_64 and most 32/64-bit
// ARM cores; Apple Silicon and some server-class ARM (Neoverse) parts use a
// 128-byte line, so widen the default there to avoid false sharing between
// adjacent aligned fields. Callers needing exact hardware detection for a
// specific ARM part should override this macro at build-config level before
// this header is first included.
#if defined(__aarch64__) || defined(_M_ARM64)
    #ifndef ANIMUS_CACHE_LINE_SIZE
    #define ANIMUS_CACHE_LINE_SIZE 128
    #endif
#else
    #ifndef ANIMUS_CACHE_LINE_SIZE
    #define ANIMUS_CACHE_LINE_SIZE 64
    #endif
#endif

// Compile-time opt-out for the best-effort stderr logging below. Leave
// enabled by default (failures are rare and worth seeing during
// development); flip off in a release build if even the rare fprintf on a
// failure path is unacceptable jitter for a given hot loop.
#ifndef ANIMUS_SYS_ENABLE_LOGGING
#define ANIMUS_SYS_ENABLE_LOGGING 1
#endif

namespace animus {

    // Size, in bytes, of one cache line on the target architecture. Use this
    // (or the macro above, where a constexpr isn't usable, e.g. alignas)
    // to pad hot structs and avoid false sharing across threads.
    inline constexpr size_t cache_line_size = ANIMUS_CACHE_LINE_SIZE;

    // Wraps T so that every instance starts on its own cache line and the
    // struct's size is rounded up to a full line -- placing these in an
    // array guarantees no two elements share a line, which matters for
    // per-thread/per-core counters and SPSC/MPMC queue cursors that are
    // written by different threads.
    template <typename T>
    struct alignas(ANIMUS_CACHE_LINE_SIZE) CacheAligned {
        T value;
    };

    // Pure padding, for manually separating two hot fields that must live in
    // the same struct (e.g. a producer cursor and a consumer cursor) but
    // must not share a cache line.
    struct alignas(ANIMUS_CACHE_LINE_SIZE) CacheLinePad {
        char reserved[ANIMUS_CACHE_LINE_SIZE];
    };

    // Portable "pause"/"yield" instruction for busy-spin polling loops. This
    // hints to the CPU that the current thread is in a spin-wait, which on
    // x86 avoids a memory-order violation stall on exiting the loop and
    // reduces power/resource contention with the other logical core sharing
    // the physical core (SMT); on ARM the equivalent is the YIELD
    // instruction. It is NOT a substitute for a real wait/backoff strategy
    // on contended locks -- it only reduces the cost of spinning, it doesn't
    // avoid it.
    inline void cpu_relax() noexcept {
#if defined(_M_IX86) || defined(_M_X64) || defined(__i386__) || defined(__x86_64__)
        _mm_pause();
#elif defined(_MSC_VER) && (defined(_M_ARM) || defined(_M_ARM64))
        __yield();
#elif defined(__aarch64__) || defined(__arm__)
        asm volatile("yield" ::: "memory");
#else
        // Unknown architecture: still correct to fall through to a thread
        // yield, just without the cheap in-place CPU hint.
        std::this_thread::yield();
#endif
    }

    namespace sys {

        namespace detail {

            // Centralized, allocation-free best-effort diagnostic sink. Never
            // called from a path where its own cost matters (only on
            // failure, which for affinity/priority setup happens once at
            // thread startup, not in a steady-state hot loop) -- see
            // ANIMUS_SYS_ENABLE_LOGGING to disable entirely regardless.
            inline void log_failure(const char* function, const char* reason, long code) noexcept {
#if ANIMUS_SYS_ENABLE_LOGGING
                std::fprintf(stderr, "[animus::sys] %s failed: %s (code=%ld)\n", function, reason, code);
#else
                (void)function;
                (void)reason;
                (void)code;
#endif
            }

        } // namespace detail

        // Pins the thread identified by `handle` to logical core `core_id`.
        //
        // `handle` must be a valid std::thread::native_handle_type for the
        // running platform (HANDLE on Windows, pthread_t on Linux) -- e.g.
        // obtained from std::thread::native_handle(), GetCurrentThread(), or
        // pthread_self(). Returns false (and logs a best-effort diagnostic)
        // on any failure; never throws.
        //
        // Windows note: the affinity mask is a single machine word, so only
        // core_id < 64 is addressable without also targeting a specific
        // processor group -- this function intentionally does not span
        // groups, since Animus Core's target hosts are single-group.
        inline bool pin_thread_to_core(std::thread::native_handle_type handle, size_t core_id) noexcept {
#if defined(_WIN32)
            if (core_id >= 64) {
                detail::log_failure("pin_thread_to_core", "core_id out of range for a single processor group", static_cast<long>(core_id));
                return false;
            }
            const DWORD_PTR mask = (static_cast<DWORD_PTR>(1) << core_id);
            const DWORD_PTR previous_mask = ::SetThreadAffinityMask(handle, mask);
            if (previous_mask == 0) {
                detail::log_failure("pin_thread_to_core", "SetThreadAffinityMask", static_cast<long>(::GetLastError()));
                return false;
            }
            // Ideal processor is only a scheduler hint on top of the hard
            // affinity mask already set above; a failure here does not
            // undo the pin, so it is not treated as an overall failure.
            ::SetThreadIdealProcessor(handle, static_cast<DWORD>(core_id));
            return true;
#elif defined(__linux__)
            if (core_id >= static_cast<size_t>(CPU_SETSIZE)) {
                detail::log_failure("pin_thread_to_core", "core_id exceeds CPU_SETSIZE", static_cast<long>(core_id));
                return false;
            }
            cpu_set_t cpu_set;
            CPU_ZERO(&cpu_set);
            CPU_SET(static_cast<int>(core_id), &cpu_set);
            const int rc = ::pthread_setaffinity_np(handle, sizeof(cpu_set_t), &cpu_set);
            if (rc != 0) {
                detail::log_failure("pin_thread_to_core", "pthread_setaffinity_np", static_cast<long>(rc));
                return false;
            }
            return true;
#else
            (void)handle;
            (void)core_id;
            detail::log_failure("pin_thread_to_core", "unsupported platform", 0);
            return false;
#endif
        }

        // Convenience wrapper: pins the calling thread to `core_id`.
        inline bool pin_current_thread_to_core(size_t core_id) noexcept {
#if defined(_WIN32)
            return pin_thread_to_core(::GetCurrentThread(), core_id);
#elif defined(__linux__)
            return pin_thread_to_core(::pthread_self(), core_id);
#else
            (void)core_id;
            detail::log_failure("pin_current_thread_to_core", "unsupported platform", 0);
            return false;
#endif
        }

        // Raises the calling thread (and, on Windows, its process priority
        // class) to the highest realtime/time-critical scheduling tier the
        // OS will grant without elevated privileges, falling back a step
        // when it won't. Best-effort by design: a latency-sensitive thread
        // should still function correctly (just with worse tail latency) if
        // the host denies real-time scheduling, so failures here are logged
        // rather than surfaced as an error to the caller.
        inline void set_thread_high_priority() noexcept {
#if defined(_WIN32)
            // REALTIME_PRIORITY_CLASS requires SeIncreaseBasePriorityPrivilege
            // on most configurations; fall back to HIGH_PRIORITY_CLASS
            // (no special privilege required) when it's refused.
            if (!::SetPriorityClass(::GetCurrentProcess(), REALTIME_PRIORITY_CLASS)) {
                detail::log_failure("set_thread_high_priority", "SetPriorityClass(REALTIME) falling back to HIGH", static_cast<long>(::GetLastError()));
                ::SetPriorityClass(::GetCurrentProcess(), HIGH_PRIORITY_CLASS);
            }
            if (!::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL)) {
                detail::log_failure("set_thread_high_priority", "SetThreadPriority", static_cast<long>(::GetLastError()));
            }
#elif defined(__linux__)
            sched_param sp{};
            sp.sched_priority = ::sched_get_priority_max(SCHED_FIFO);
            if (::sched_setscheduler(0, SCHED_FIFO, &sp) != 0) {
                // Most likely missing CAP_SYS_NICE; a deeper nice value is
                // still a meaningful (if much weaker) scheduling boost and
                // requires no special capability under the default limits.
                detail::log_failure("set_thread_high_priority", "sched_setscheduler(SCHED_FIFO) falling back to nice(-20)", static_cast<long>(errno));
                ::setpriority(PRIO_PROCESS, 0, -20);
            }
#else
            detail::log_failure("set_thread_high_priority", "unsupported platform", 0);
#endif
        }

    } // namespace sys

} // namespace animus
