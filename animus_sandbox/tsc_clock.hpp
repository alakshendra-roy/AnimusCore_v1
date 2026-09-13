#pragma once
// Invariant TSC utilities: hardware cycle counting with zero system calls.
//
// __rdtscp (not plain __rdtsc) is used here because it is a partially
// serializing instruction -- the CPU cannot begin executing it until all
// prior instructions have retired, which plain rdtsc alone does not
// guarantee. That gives every read below a clean "nothing before me is
// still in flight" boundary with no separate fence instruction needed.
//
// Invariant TSC (CPUID 0x80000007:EDX bit 8) means the counter ticks at a
// fixed rate regardless of core P-state/C-state transitions, so elapsed
// cycles convert to elapsed time via one calibration constant measured
// once at startup -- without it, turbo/power-saving transitions would
// silently corrupt any assumed cycles-to-ns conversion.
#include <chrono>
#include <cstdint>

#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
    #define SANDBOX_ARCH_X86 1
#else
    #define SANDBOX_ARCH_X86 0
#endif

#if SANDBOX_ARCH_X86
    #if defined(_MSC_VER)
        #include <intrin.h>
    #else
        #include <cpuid.h>
        #include <x86intrin.h>
    #endif
#endif

namespace sandbox {

#if SANDBOX_ARCH_X86

inline bool has_invariant_tsc() noexcept {
    unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
#if defined(_MSC_VER)
    int regs[4] = {0, 0, 0, 0};
    __cpuid(regs, static_cast<int>(0x80000007));
    eax = static_cast<unsigned>(regs[0]);
    ebx = static_cast<unsigned>(regs[1]);
    ecx = static_cast<unsigned>(regs[2]);
    edx = static_cast<unsigned>(regs[3]);
#else
    __get_cpuid(0x80000007u, &eax, &ebx, &ecx, &edx);
#endif
    (void)eax; (void)ebx; (void)ecx;
    return (edx & (1u << 8)) != 0;
}

// Serializing read. No system call, no vDSO hop -- a plain hardware
// register read, which is the entire point of using the TSC on the hot
// path instead of e.g. clock_gettime().
inline std::uint64_t read_tsc() noexcept {
    unsigned int aux;
    return __rdtscp(&aux);
}

#else // non-x86: no TSC register to read -- fall back to a monotonic clock.

inline bool has_invariant_tsc() noexcept { return false; }

inline std::uint64_t read_tsc() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

#endif

// Measures TSC frequency by racing the TSC against std::chrono::steady_clock
// over a fixed wall-clock window rather than trusting an advertised base
// clock -- turbo and power-saving states are common sources of a silently
// wrong assumed cycles/ns figure.
inline double calibrate_cycles_per_ns(std::chrono::milliseconds window = std::chrono::milliseconds(200)) noexcept {
#if !SANDBOX_ARCH_X86
    (void)window;
    return 1.0;
#else
    const auto wall_start = std::chrono::steady_clock::now();
    const std::uint64_t tsc_start = read_tsc();
    while (std::chrono::steady_clock::now() - wall_start < window) {
        // Busy-wait: a sleep-based window risks an OS-scheduled core
        // migration mid-measurement, which would corrupt the TSC delta on
        // systems without a cross-core-synchronized TSC.
    }
    const std::uint64_t tsc_end = read_tsc();
    const auto wall_end = std::chrono::steady_clock::now();
    const double elapsed_ns = std::chrono::duration<double, std::nano>(wall_end - wall_start).count();
    const double elapsed_cycles = static_cast<double>(tsc_end - tsc_start);
    return elapsed_cycles / elapsed_ns;
#endif
}

} // namespace sandbox
