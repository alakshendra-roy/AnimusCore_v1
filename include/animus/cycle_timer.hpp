#pragma once
// Multi-architecture serialized cycle counter with dynamic calibration.
//
// tests/eval_harness.cpp used to carry its own x86-only rdtsc_serialized()
// and a CPUID check that does not even compile on AArch64. This header is
// the one place that knows how to read a hardware counter on each target,
// so the harnesses (and any customer code that wants to time a region
// without a syscall) share a single, audited implementation:
//
//   x86-64 / x86   lfence ; rdtsc ; lfence          (invariant TSC)
//   AArch64 GCC    isb ; mrs xN, cntvct_el0 ; isb   (generic timer virtual count)
//   AArch64 MSVC   __isb ; _ReadStatusReg(CNTVCT) ; __isb   (written against <intrin.h>; NOT compiled
//                  in this release's verification -- no ARM64 MSVC toolchain was available)
//   anything else  std::chrono::steady_clock        (flagged: NOT a cycle counter)
//
// What the number means. On x86 the invariant TSC ticks at a fixed reference
// frequency regardless of core P-/C-state, so its ticks are "reference
// cycles", not necessarily core clock cycles under turbo. On AArch64 the
// generic timer's CNTVCT_EL0 ticks at CNTFRQ_EL0 -- typically 1-50 MHz on
// ARMv8.0-8.5 parts and 1 GHz on ARMv8.6+/ARMv9 (the architecture fixes it at
// 1 GHz from ARMv8.6) -- and is NOT the core clock at all. A harness must
// therefore never hard-code "cycles per nanosecond": calibrate() below
// measures the ratio against std::chrono::steady_clock at run time, and
// resolution_ns() reports the smallest interval the counter can resolve, so
// a result on a 24 MHz counter (41.7 ns per tick) is read for what it is.
//
// Why both barriers. rdtsc/mrs by themselves are not ordered against the
// surrounding instructions: the CPU may begin the timed work before the
// counter is sampled (under-reporting the interval) or sample the counter
// before earlier work has finished. lfence (x86) / isb (AArch64) before the
// read stops the first; the same barrier after the read stops later
// instructions from running ahead of it. The AArch64 sequence the project
// specifies is "isb ; mrs" -- the trailing isb below only makes it symmetric
// with the x86 form and costs one more pipeline flush per read.
//
// On x86 the ordering guarantee rests on LFENCE completing all earlier
// instructions before any later one issues. That is architectural on Intel; AMD
// parts have enabled it by default or via an OS-set MSR depending on the
// generation, so confirm it on the exact part you deploy on.
//
// Header-only, noexcept and allocation-free (calibrate() uses only the stack).
// Safe to include next to thread_affinity.hpp.

#include <chrono>
#include <cstddef>
#include <cstdint>

#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
    #define ANIMUS_TIMING_X86 1
    #if defined(_MSC_VER)
        #include <intrin.h>
    #else
        #include <cpuid.h>
        #include <x86intrin.h>
    #endif
#elif defined(__aarch64__)
    #define ANIMUS_TIMING_ARM64 1
#elif defined(_M_ARM64)
    #define ANIMUS_TIMING_ARM64 1
    #define ANIMUS_TIMING_ARM64_MSVC 1
    #include <intrin.h>
#endif

namespace animus {
namespace timing {

    enum class CounterKind : int {
        InvariantTsc = 0,    // x86 time-stamp counter (fixed reference rate when CPUID reports invariance)
        ArmGenericTimer = 1, // AArch64 CNTVCT_EL0 (ticks at CNTFRQ_EL0, not at the core clock)
        SteadyClock = 2,     // fallback: no hardware counter on this target; resolution = steady_clock's
    };

#if defined(ANIMUS_TIMING_X86)
    inline constexpr CounterKind kCounterKind = CounterKind::InvariantTsc;
#elif defined(ANIMUS_TIMING_ARM64)
    inline constexpr CounterKind kCounterKind = CounterKind::ArmGenericTimer;
#else
    inline constexpr CounterKind kCounterKind = CounterKind::SteadyClock;
#endif

    inline constexpr const char* counter_kind_name(CounterKind kind) noexcept {
        return kind == CounterKind::InvariantTsc ? "invariant TSC (lfence; rdtsc; lfence)"
             : kind == CounterKind::ArmGenericTimer ? "AArch64 generic timer CNTVCT_EL0 (isb; mrs; isb)"
             : "std::chrono::steady_clock (no hardware cycle counter on this target)";
    }

    // What one tick of this counter is called in a report: x86 prints
    // "cycles" (invariant-TSC reference cycles, the historical unit of this
    // project's figures); AArch64 and the fallback print "ticks" because a
    // generic-timer tick is not a core cycle.
    inline constexpr const char* tick_unit_name(CounterKind kind) noexcept {
        return kind == CounterKind::InvariantTsc ? "cycles" : "ticks";
    }

    // Serialized read -- brackets a timed region. See the file header for why
    // both barriers are present.
    inline uint64_t read_cycles_serialized() noexcept {
#if defined(ANIMUS_TIMING_X86)
        _mm_lfence();
        const uint64_t t = __rdtsc();
        _mm_lfence();
        return t;
#elif defined(ANIMUS_TIMING_ARM64_MSVC)
        __isb(_ARM64_BARRIER_SY);
        const uint64_t t = static_cast<uint64_t>(_ReadStatusReg(ARM64_CNTVCT));
        __isb(_ARM64_BARRIER_SY);
        return t;
#elif defined(ANIMUS_TIMING_ARM64)
        uint64_t t;
        // "\n\t", not ";": ';' starts a comment in Apple's AArch64 assembler.
        asm volatile("isb\n\tmrs %0, cntvct_el0\n\tisb" : "=r"(t) : : "memory");
        return t;
#else
        return static_cast<uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
#endif
    }

    // Unserialized read: a few cycles cheaper, correct for stamping a value
    // that another thread will difference against its own stamp (transit
    // latency) where the stamp's position within a handful of instructions
    // does not matter. Not for bracketing a region shorter than the
    // reorder window -- use read_cycles_serialized() there.
    inline uint64_t read_cycles() noexcept {
#if defined(ANIMUS_TIMING_X86)
        return __rdtsc();
#elif defined(ANIMUS_TIMING_ARM64_MSVC)
        return static_cast<uint64_t>(_ReadStatusReg(ARM64_CNTVCT));
#elif defined(ANIMUS_TIMING_ARM64)
        uint64_t t;
        asm volatile("mrs %0, cntvct_el0" : "=r"(t) : : "memory");
        return t;
#else
        return static_cast<uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
#endif
    }

    // AArch64: the architectural counter frequency in Hz (CNTFRQ_EL0), used
    // by calibrate() only as a cross-check of the measured rate. 0 elsewhere.
    inline uint64_t arm_counter_frequency_hz() noexcept {
#if defined(ANIMUS_TIMING_ARM64_MSVC)
        return static_cast<uint64_t>(_ReadStatusReg(ARM64_CNTFRQ));
#elif defined(ANIMUS_TIMING_ARM64)
        uint64_t f;
        asm volatile("mrs %0, cntfrq_el0" : "=r"(f));
        return f;
#else
        return 0;
#endif
    }

    // True when the counter is known to tick at a constant rate, which is
    // what makes a raw tick count meaningful to report at all.
    //   x86:     CPUID.80000007H:EDX[8] ("invariant TSC"). Leaf 0x80000007 is
    //            only valid if the maximum extended leaf reaches it.
    //   AArch64: the system counter is architecturally constant-rate.
    //   fallback: steady_clock is monotonic by definition (but see kCounterKind).
    inline bool counter_is_invariant() noexcept {
#if defined(ANIMUS_TIMING_X86)
    #if defined(_MSC_VER)
        int info[4] = { 0, 0, 0, 0 };
        __cpuid(info, static_cast<int>(0x80000000));
        if (static_cast<unsigned>(info[0]) < 0x80000007u) return false;
        __cpuid(info, static_cast<int>(0x80000007));
        return (static_cast<unsigned>(info[3]) & (1u << 8)) != 0;
    #else
        if (__get_cpuid_max(0x80000000u, nullptr) < 0x80000007u) return false;
        unsigned a = 0, b = 0, c = 0, d = 0;
        __cpuid(0x80000007u, a, b, c, d);
        return (d & (1u << 8)) != 0;
    #endif
#else
        return true;
#endif
    }

    // Result of racing the counter against std::chrono::steady_clock.
    struct Calibration {
        bool valid = false;
        double ticks_per_ns = 0.0;     // median over `trials`; == GHz for an x86 TSC
        double min_ticks_per_ns = 0.0; // spread across trials: a wide spread means the host was
        double max_ticks_per_ns = 0.0; // too noisy to trust the conversion to that many digits
        double spread_ppm = 0.0;       // (max - min) / median, in parts per million
        unsigned trials = 0;
        uint64_t window_ns = 0;        // length of each trial's window
        uint64_t arm_cntfrq_hz = 0;    // AArch64 only: CNTFRQ_EL0 for cross-checking ticks_per_ns

        double ns(uint64_t ticks) const noexcept {
            return valid && ticks_per_ns > 0.0 ? static_cast<double>(ticks) / ticks_per_ns : 0.0;
        }
        // Smallest interval one tick can resolve, in nanoseconds.
        double resolution_ns() const noexcept {
            return valid && ticks_per_ns > 0.0 ? 1.0 / ticks_per_ns : 0.0;
        }
    };

    // Measures ticks per nanosecond by sampling the counter inside two
    // steady_clock readings at the start and again at the end of a busy-wait
    // window, then using the midpoints so the cost of the clock read itself
    // cancels. `trials` independent windows are taken and the MEDIAN ratio is
    // reported, with the min/max spread, so one preempted window cannot skew
    // the result and the harness can say how far to trust it. Nothing here is
    // assumed from a spec sheet or a /proc file.
    inline Calibration calibrate(std::chrono::milliseconds window = std::chrono::milliseconds(100),
                                 unsigned trials = 5) noexcept {
        using clock = std::chrono::steady_clock;
        constexpr unsigned kMaxTrials = 15;
        if (trials == 0) trials = 1;
        if (trials > kMaxTrials) trials = kMaxTrials;

        const auto now_ns = []() noexcept {
            return static_cast<int64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                clock::now().time_since_epoch()).count());
        };
        const int64_t window_ns = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(window).count());

        double ratios[kMaxTrials];
        unsigned good = 0;
        for (unsigned i = 0; i < trials; ++i) {
            const int64_t a0 = now_ns();
            const uint64_t c0 = read_cycles_serialized();
            const int64_t b0 = now_ns();
            while (now_ns() - b0 < window_ns) { /* spin: keeps the core awake and un-migrated */ }
            const int64_t a1 = now_ns();
            const uint64_t c1 = read_cycles_serialized();
            const int64_t b1 = now_ns();

            const double t0 = 0.5 * (static_cast<double>(a0) + static_cast<double>(b0));
            const double t1 = 0.5 * (static_cast<double>(a1) + static_cast<double>(b1));
            if (t1 <= t0 || c1 <= c0) continue; // clock stepped or counter wrapped: discard the trial
            ratios[good++] = static_cast<double>(c1 - c0) / (t1 - t0);
        }

        Calibration cal;
        cal.trials = good;
        cal.window_ns = static_cast<uint64_t>(window_ns);
        cal.arm_cntfrq_hz = arm_counter_frequency_hz();
        if (good == 0) return cal;

        // Insertion sort: at most 15 elements, and no <algorithm> dependency.
        for (unsigned i = 1; i < good; ++i) {
            const double v = ratios[i];
            unsigned j = i;
            while (j > 0 && ratios[j - 1] > v) { ratios[j] = ratios[j - 1]; --j; }
            ratios[j] = v;
        }
        cal.min_ticks_per_ns = ratios[0];
        cal.max_ticks_per_ns = ratios[good - 1];
        cal.ticks_per_ns = (good % 2 == 1) ? ratios[good / 2] : 0.5 * (ratios[good / 2 - 1] + ratios[good / 2]);
        cal.spread_ppm = cal.ticks_per_ns > 0.0
            ? (cal.max_ticks_per_ns - cal.min_ticks_per_ns) / cal.ticks_per_ns * 1e6 : 0.0;
        cal.valid = cal.ticks_per_ns > 0.0;
        return cal;
    }

} // namespace timing
} // namespace animus
