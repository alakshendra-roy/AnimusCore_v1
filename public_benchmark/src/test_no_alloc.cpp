// SPDX-License-Identifier: Apache-2.0
// test_no_alloc.cpp -- dynamic zero-allocation audit of the AnimusCore hot path.
//
// What a clean run establishes. During a window in which every worker thread has g_enforce_zero_alloc
// set, no thread in the process calls ANY heap entry point -- operator new/new[]/delete/delete[] in all
// C++17 forms, or malloc/calloc/realloc/free/posix_memalign/aligned_alloc/memalign/valloc/pvalloc --
// while 10,000,000 telemetry records go through each ring: animus::SpscRingBuffer (1 producer, 1
// consumer) and animus::LockFreeRingBuffer (the MPMC ring; 2 producers, 2 consumers), both from
// animus/ring_buffers.hpp, and animus::sys::ipc::SpscQueue (the header-based SPSC queue that lives in caller-
// supplied, cache-line-aligned memory; 1 producer, 1 consumer; animus/spsc_queue.hpp).
// Any attempt inside the window aborts at the offending call with a stack trace. Independently, the
// process-wide count of heap calls must be unchanged across the window. A third phase runs
// bench_tsc.hpp's Profiler::run() under the same guard, so the profiler is proven allocation-free too.
//
// How. The malloc family is replaced by direct symbol interposition: the definitions below live in the
// executable, so the dynamic linker binds EVERY caller in the process to them, including the calls
// libc and libstdc++ make internally (strdup, fopen, pthread_create, std::string...). They forward to
// glibc's own __libc_* entry points, so allocation behaves exactly as before. Link-time wrapping
// (`ld --wrap=malloc`) is the usual alternative; it redirects only references in the objects being
// linked and cannot see calls made from inside libc.so or libstdc++.so, which is why it is not used here.
//
// Why a clean run means something. A guard that never fires would also report "clean", so before the
// hot path runs the harness proves it can see: for each of the 29 entry points it checks that the
// call is recorded exactly once, under its own name, when enforcement is on (soft mode), that nothing
// is recorded when it is off, and, in a forked child, that the real abort path fires (SIGABRT plus the
// diagnostic naming that entry point). It then checks libc/libstdc++-internal callers are caught,
// that enforcement is per thread, that scopes nest, and that the replacement allocators still honour
// alignment, zero-fill, realloc and EINVAL semantics.
//
// What it does not establish. It is a dynamic check of the paths exercised in this run, not a proof
// over all inputs; a static audit of the compiled hot-path symbols (no allocator or throw path reachable
// from push/pop) is the complementary check. It sees heap API calls, not direct mmap/brk system calls
// or a private arena. The ring storage (a std::vector inside each ring) is allocated before the
// window, as designed.
//
// assert() is deliberately not used: release builds define NDEBUG, which would compile the guard
// out. Violations call abort() unconditionally.
//
// A violation prints the entry point, the byte count and a stack trace, then aborts. Frames print as
// module(+offset): resolve one with `addr2line -f -C -e ./test_no_alloc <offset>` (add -g to the build
// for file:line). -rdynamic also names the externally visible functions directly.
//
// Build and run (Linux/glibc, dynamic linking; do not combine with ASan/TSan/MSan, which interpose
// the allocator themselves; UBSan is fine). From the package root:
//     clang++ -std=c++17 -O3 -march=native -Wall -Wextra -Werror -pedantic -fsized-deallocation -pthread -rdynamic -I include src/test_no_alloc.cpp -o test_no_alloc
//     ./test_no_alloc                  # 10,000,000 iterations per ring; exit 0 = all checks passed
//     ./test_no_alloc --iters 1000000  # shorter
// or: make check   (CXX=g++ also builds clean)
#include "animus/aligned_block.hpp"
#include "animus/bench_tsc.hpp"
#include "animus/ring_buffers.hpp"
#include "animus/spsc_queue.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <thread>
#include <vector>

#include <execinfo.h>
#include <gnu/libc-version.h>
#include <immintrin.h>
#include <malloc.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#if !defined(__linux__) || !defined(__GLIBC__)
    #error "test_no_alloc.cpp interposes glibc's allocator (__libc_malloc and friends): Linux + glibc only"
#endif
#ifndef __has_feature
    #define __has_feature(x) 0
#endif
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__) || \
    __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || __has_feature(memory_sanitizer)
    #error "test_no_alloc.cpp replaces malloc itself, as do ASan/TSan/MSan: build it without those (UBSan is fine)"
#endif

#define ANIMUS_EXPORT __attribute__((visibility("default")))
#define ANIMUS_TLS __attribute__((tls_model("initial-exec")))

// ===========================================================================
// 1. The guard
// ===========================================================================

// THE SWITCH. While true on a thread, any heap call from that thread is a violation. Thread-local, so
// setup and warmup on any thread allocate freely and only threads that opt in are policed.
// Constant-initialized and trivially destructible, so reading it from inside malloc() needs no lazy TLS
// setup (which would itself allocate); initial-exec keeps that true even if this is built with -fPIC.
thread_local bool g_enforce_zero_alloc ANIMUS_TLS = false;

extern "C" {
// glibc's real allocator. Exported by libc.so.6 but not declared in any header.
void* __libc_malloc(std::size_t) noexcept;
void* __libc_calloc(std::size_t, std::size_t) noexcept;
void* __libc_realloc(void*, std::size_t) noexcept;
void* __libc_memalign(std::size_t, std::size_t) noexcept;
void* __libc_valloc(std::size_t) noexcept;
void* __libc_pvalloc(std::size_t) noexcept;
void __libc_free(void*) noexcept;
}

namespace guard {

constexpr std::size_t kLogCap = 8;
constexpr std::size_t kRecent = 32;
struct Violation {
    const char* entry;
    std::size_t bytes;
};

// Every heap entry point, any thread, enforcement on or off. A hot window must not move it.
std::atomic<std::uint64_t> g_calls{ 0 };
// Names of the most recent kRecent calls, slot = call number % kRecent: when the window check fails,
// this says WHICH call it was, even on a thread that never opted in.
std::atomic<const char*> g_recent[kRecent];

// Test-only: record a violation instead of aborting, so the matrix can inspect it.
thread_local bool soft_mode ANIMUS_TLS = false;
thread_local std::uint32_t violations ANIMUS_TLS = 0;
thread_local Violation vlog[kLogCap] ANIMUS_TLS;

// Formats into a stack buffer and writes with write(2): no stdio, no allocation.
class Msg {
public:
    void add(const char* s) noexcept {
        while (*s != '\0' && n_ + 1 < sizeof buf_) buf_[n_++] = *s++;
    }
    void add(std::uint64_t v) noexcept {
        char digits[24];
        int k = 0;
        do { digits[k++] = static_cast<char>('0' + v % 10); v /= 10; } while (v != 0);
        while (k > 0) {
            const char c = digits[--k];
            if (n_ + 1 < sizeof buf_) buf_[n_++] = c;
        }
    }
    void flush(int fd) const noexcept {
        std::size_t off = 0;
        while (off < n_) {
            const ssize_t w = ::write(fd, buf_ + off, n_ - off);
            if (w <= 0) break;
            off += static_cast<std::size_t>(w);
        }
    }
private:
    char buf_[384];
    std::size_t n_ = 0;
};

[[noreturn, gnu::noinline, gnu::cold]] void fatal(const char* entry, std::size_t bytes) noexcept {
    g_enforce_zero_alloc = false; // the handler may allocate (backtrace does): never let it recurse
    Msg m;
    m.add("\nanimus-no-alloc: ZERO-ALLOC VIOLATION: heap call '");
    m.add(entry);
    m.add("' (");
    m.add(static_cast<std::uint64_t>(bytes));
    m.add(" bytes) on thread ");
    m.add(static_cast<std::uint64_t>(::syscall(SYS_gettid)));
    m.add(" while g_enforce_zero_alloc was set. Call stack:\n");
    m.flush(STDERR_FILENO);
    void* frames[32];
    const int depth = ::backtrace(frames, 32);
    ::backtrace_symbols_fd(frames, depth, STDERR_FILENO);
    ::abort();
}

// Called by every interposed entry point before it forwards to glibc.
inline void note(const char* entry, std::size_t bytes) noexcept {
    const std::uint64_t call = g_calls.fetch_add(1, std::memory_order_relaxed);
    g_recent[call % kRecent].store(entry, std::memory_order_relaxed);
    if (!g_enforce_zero_alloc) return;
    if (soft_mode) {
        if (violations < kLogCap) vlog[violations] = Violation{ entry, bytes };
        ++violations;
        return;
    }
    fatal(entry, bytes);
}

void* new_throwing(std::size_t n, const char* who) {
    note(who, n);
    for (;;) {
        if (void* p = __libc_malloc(n != 0 ? n : 1)) return p;
        const std::new_handler h = std::get_new_handler();
        if (h == nullptr) throw std::bad_alloc();
        h();
    }
}
void* new_aligned_throwing(std::size_t n, std::align_val_t a, const char* who) {
    note(who, n);
    for (;;) {
        if (void* p = __libc_memalign(static_cast<std::size_t>(a), n != 0 ? n : 1)) return p;
        const std::new_handler h = std::get_new_handler();
        if (h == nullptr) throw std::bad_alloc();
        h();
    }
}
void* new_nothrow(std::size_t n, const char* who) noexcept {
    note(who, n);
    for (;;) {
        if (void* p = __libc_malloc(n != 0 ? n : 1)) return p;
        const std::new_handler h = std::get_new_handler();
        if (h == nullptr) return nullptr;
        try { h(); } catch (...) { return nullptr; }
    }
}
void* new_aligned_nothrow(std::size_t n, std::align_val_t a, const char* who) noexcept {
    note(who, n);
    for (;;) {
        if (void* p = __libc_memalign(static_cast<std::size_t>(a), n != 0 ? n : 1)) return p;
        const std::new_handler h = std::get_new_handler();
        if (h == nullptr) return nullptr;
        try { h(); } catch (...) { return nullptr; }
    }
}
// Deleting nullptr is a no-op for every form and is not a heap event.
void release(void* p, const char* who) noexcept {
    if (p == nullptr) return;
    note(who, 0);
    __libc_free(p);
}

} // namespace guard

// ---- C allocator family ---------------------------------------------------------------------------
extern "C" {

ANIMUS_EXPORT void* malloc(std::size_t n) noexcept {
    guard::note("malloc", n);
    return __libc_malloc(n);
}
ANIMUS_EXPORT void free(void* p) noexcept {
    if (p != nullptr) guard::note("free", 0);
    __libc_free(p);
}
ANIMUS_EXPORT void* calloc(std::size_t count, std::size_t size) noexcept {
    std::size_t total = 0;
    guard::note("calloc", __builtin_mul_overflow(count, size, &total) ? SIZE_MAX : total);
    return __libc_calloc(count, size);
}
ANIMUS_EXPORT void* realloc(void* p, std::size_t n) noexcept {
    guard::note("realloc", n);
    return __libc_realloc(p, n);
}
ANIMUS_EXPORT int posix_memalign(void** out, std::size_t alignment, std::size_t n) noexcept {
    guard::note("posix_memalign", n);
    // POSIX: a power of two that is a multiple of sizeof(void*). This also rejects 0.
    if (alignment < sizeof(void*) || (alignment & (alignment - 1)) != 0) return EINVAL;
    const int saved_errno = errno;
    void* p = __libc_memalign(alignment, n);
    errno = saved_errno; // posix_memalign reports through its return value and leaves errno alone
    if (p == nullptr) return ENOMEM;
    *out = p;
    return 0;
}
ANIMUS_EXPORT void* aligned_alloc(std::size_t alignment, std::size_t n) noexcept {
    guard::note("aligned_alloc", n);
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) { errno = EINVAL; return nullptr; } // as glibc does
    return __libc_memalign(alignment, n);
}
ANIMUS_EXPORT void* memalign(std::size_t alignment, std::size_t n) noexcept {
    guard::note("memalign", n);
    return __libc_memalign(alignment, n);
}
ANIMUS_EXPORT void* valloc(std::size_t n) noexcept {
    guard::note("valloc", n);
    return __libc_valloc(n);
}
ANIMUS_EXPORT void* pvalloc(std::size_t n) noexcept {
    guard::note("pvalloc", n);
    return __libc_pvalloc(n);
}

} // extern "C"

// ---- C++17 replaceable allocation functions: all 8 allocation forms and all 12 deallocation forms --
void* operator new(std::size_t n) { return guard::new_throwing(n, "operator new"); }
void* operator new[](std::size_t n) { return guard::new_throwing(n, "operator new[]"); }
void* operator new(std::size_t n, std::align_val_t a) { return guard::new_aligned_throwing(n, a, "operator new(align_val_t)"); }
void* operator new[](std::size_t n, std::align_val_t a) { return guard::new_aligned_throwing(n, a, "operator new[](align_val_t)"); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { return guard::new_nothrow(n, "operator new(nothrow)"); }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { return guard::new_nothrow(n, "operator new[](nothrow)"); }
void* operator new(std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept { return guard::new_aligned_nothrow(n, a, "operator new(align_val_t, nothrow)"); }
void* operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept { return guard::new_aligned_nothrow(n, a, "operator new[](align_val_t, nothrow)"); }

void operator delete(void* p) noexcept { guard::release(p, "operator delete"); }
void operator delete[](void* p) noexcept { guard::release(p, "operator delete[]"); }
void operator delete(void* p, std::size_t) noexcept { guard::release(p, "operator delete(size)"); }
void operator delete[](void* p, std::size_t) noexcept { guard::release(p, "operator delete[](size)"); }
void operator delete(void* p, std::align_val_t) noexcept { guard::release(p, "operator delete(align_val_t)"); }
void operator delete[](void* p, std::align_val_t) noexcept { guard::release(p, "operator delete[](align_val_t)"); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { guard::release(p, "operator delete(size, align_val_t)"); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { guard::release(p, "operator delete[](size, align_val_t)"); }
void operator delete(void* p, const std::nothrow_t&) noexcept { guard::release(p, "operator delete(nothrow)"); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { guard::release(p, "operator delete[](nothrow)"); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { guard::release(p, "operator delete(align_val_t, nothrow)"); }
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept { guard::release(p, "operator delete[](align_val_t, nothrow)"); }

// ---- Scopes ---------------------------------------------------------------------------------------
namespace {

inline void compiler_barrier() noexcept { asm volatile("" : : : "memory"); }

// The compiler must treat `p` as read and escaped: a deliberate allocation whose pointer is never
// observed may legally be removed ([expr.new]; clang does so at -O2), and malloc/free pairs likewise.
inline void escape(void* p) noexcept { asm volatile("" : : "g"(p) : "memory"); }

// A null the optimizer cannot see through. realloc(nullptr, n) is rewritten to malloc(n) at compile
// time, so a probe that passes a literal nullptr never reaches the realloc entry point at all.
void* volatile g_opaque_null = nullptr;

// Sets the switch for the lifetime of the scope and restores the previous state, so scopes nest. The
// barriers keep the compiler from moving the hot loop's calls across the store.
class EnforcementScope {
public:
    explicit EnforcementScope(bool on) noexcept : previous_(g_enforce_zero_alloc) {
        g_enforce_zero_alloc = on;
        compiler_barrier();
    }
    ~EnforcementScope() {
        compiler_barrier();
        g_enforce_zero_alloc = previous_;
    }
    EnforcementScope(const EnforcementScope&) = delete;
    EnforcementScope& operator=(const EnforcementScope&) = delete;
private:
    bool previous_;
};

// Enter the hot path: any heap call on this thread until the scope ends is a violation.
struct ZeroAllocScope : EnforcementScope { ZeroAllocScope() noexcept : EnforcementScope(true) {} };
// Inside a hot path, allow heap use for a deliberate stretch (diagnostics, a report).
struct AllocAllowedScope : EnforcementScope { AllocAllowedScope() noexcept : EnforcementScope(false) {} };

int g_failures = 0;
void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

// ===========================================================================
// 2. Negative controls: prove the guard can see before trusting it when it sees nothing
// ===========================================================================

using AcquireFn = void* (*)();
using ReleaseFn = void (*)(void*);

struct Probe {
    const char* entry;   // the exact name the guard reports for this entry point
    bool is_release;     // false: `entry` allocates; true: `entry` deallocates
    AcquireFn acquire;   // allocation rows: the call under test. release rows: an unguarded allocator that yields a block
    ReleaseFn release;   // release rows: the call under test. allocation rows: unguarded cleanup of the block
};

constexpr std::size_t kBytes = 64;
constexpr std::align_val_t kAlign{ 64 };

void rel_free(void* p) { ::free(p); }
void rel_delete(void* p) { ::operator delete(p); }
void rel_delete_arr(void* p) { ::operator delete[](p); }
void rel_delete_sized(void* p) { ::operator delete(p, kBytes); }
void rel_delete_arr_sized(void* p) { ::operator delete[](p, kBytes); }
void rel_delete_al(void* p) { ::operator delete(p, kAlign); }
void rel_delete_arr_al(void* p) { ::operator delete[](p, kAlign); }
void rel_delete_sized_al(void* p) { ::operator delete(p, kBytes, kAlign); }
void rel_delete_arr_sized_al(void* p) { ::operator delete[](p, kBytes, kAlign); }
void rel_delete_nt(void* p) { ::operator delete(p, std::nothrow); }
void rel_delete_arr_nt(void* p) { ::operator delete[](p, std::nothrow); }
void rel_delete_al_nt(void* p) { ::operator delete(p, kAlign, std::nothrow); }
void rel_delete_arr_al_nt(void* p) { ::operator delete[](p, kAlign, std::nothrow); }

void* acq_malloc() { void* p = ::malloc(kBytes); escape(p); return p; }
void* acq_calloc() { void* p = ::calloc(8, 8); escape(p); return p; }
void* acq_realloc() { void* p = ::realloc(g_opaque_null, kBytes); escape(p); return p; }
void* acq_posix_memalign() { void* p = nullptr; if (::posix_memalign(&p, 64, kBytes) != 0) return nullptr; escape(p); return p; }
void* acq_aligned_alloc() { void* p = ::aligned_alloc(64, kBytes); escape(p); return p; }
void* acq_memalign() { void* p = ::memalign(64, kBytes); escape(p); return p; }
void* acq_valloc() { void* p = ::valloc(kBytes); escape(p); return p; }
void* acq_pvalloc() { void* p = ::pvalloc(kBytes); escape(p); return p; }
void* acq_new() { void* p = ::operator new(kBytes); escape(p); return p; }
void* acq_new_arr() { void* p = ::operator new[](kBytes); escape(p); return p; }
void* acq_new_nt() { void* p = ::operator new(kBytes, std::nothrow); escape(p); return p; }
void* acq_new_arr_nt() { void* p = ::operator new[](kBytes, std::nothrow); escape(p); return p; }
void* acq_new_al() { void* p = ::operator new(kBytes, kAlign); escape(p); return p; }
void* acq_new_arr_al() { void* p = ::operator new[](kBytes, kAlign); escape(p); return p; }
void* acq_new_al_nt() { void* p = ::operator new(kBytes, kAlign, std::nothrow); escape(p); return p; }
void* acq_new_arr_al_nt() { void* p = ::operator new[](kBytes, kAlign, std::nothrow); escape(p); return p; }

// 16 allocation entry points + 13 deallocation entry points = 29. Every one is exercised.
const Probe kProbes[] = {
    { "malloc",                               false, acq_malloc,         rel_free },
    { "calloc",                               false, acq_calloc,         rel_free },
    { "realloc",                              false, acq_realloc,        rel_free },
    { "posix_memalign",                       false, acq_posix_memalign, rel_free },
    { "aligned_alloc",                        false, acq_aligned_alloc,  rel_free },
    { "memalign",                             false, acq_memalign,       rel_free },
    { "valloc",                               false, acq_valloc,         rel_free },
    { "pvalloc",                              false, acq_pvalloc,        rel_free },
    { "operator new",                         false, acq_new,            rel_delete },
    { "operator new[]",                       false, acq_new_arr,        rel_delete_arr },
    { "operator new(nothrow)",                false, acq_new_nt,         rel_delete_nt },
    { "operator new[](nothrow)",              false, acq_new_arr_nt,     rel_delete_arr_nt },
    { "operator new(align_val_t)",            false, acq_new_al,         rel_delete_al },
    { "operator new[](align_val_t)",          false, acq_new_arr_al,     rel_delete_arr_al },
    { "operator new(align_val_t, nothrow)",   false, acq_new_al_nt,      rel_delete_al_nt },
    { "operator new[](align_val_t, nothrow)", false, acq_new_arr_al_nt,  rel_delete_arr_al_nt },

    { "free",                                 true,  acq_malloc,         rel_free },
    { "operator delete",                      true,  acq_new,            rel_delete },
    { "operator delete[]",                    true,  acq_new_arr,        rel_delete_arr },
    { "operator delete(size)",                true,  acq_new,            rel_delete_sized },
    { "operator delete[](size)",              true,  acq_new_arr,        rel_delete_arr_sized },
    { "operator delete(align_val_t)",         true,  acq_new_al,         rel_delete_al },
    { "operator delete[](align_val_t)",       true,  acq_new_arr_al,     rel_delete_arr_al },
    { "operator delete(size, align_val_t)",   true,  acq_new_al,         rel_delete_sized_al },
    { "operator delete[](size, align_val_t)", true,  acq_new_arr_al,     rel_delete_arr_sized_al },
    { "operator delete(nothrow)",             true,  acq_new_nt,         rel_delete_nt },
    { "operator delete[](nothrow)",           true,  acq_new_arr_nt,     rel_delete_arr_nt },
    { "operator delete(align_val_t, nothrow)",   true, acq_new_al_nt,    rel_delete_al_nt },
    { "operator delete[](align_val_t, nothrow)", true, acq_new_arr_al_nt, rel_delete_arr_al_nt },
};
constexpr std::size_t kProbeCount = sizeof kProbes / sizeof kProbes[0];
static_assert(kProbeCount == 29, "8 C allocators + free + 8 operator new forms + 12 operator delete forms");

struct SoftResult {
    std::uint32_t count = 0;
    bool first_named_ok = false;
    bool got_block = false;
};

// Runs one probe in-process with violations recorded rather than fatal.
SoftResult run_soft(const Probe& pr, bool enforce) {
    SoftResult r;
    guard::soft_mode = true;
    guard::violations = 0;
    void* p = nullptr;
    if (pr.is_release) {
        p = pr.acquire(); // enforcement is off here
        r.got_block = p != nullptr;
        escape(p);
        EnforcementScope scope(enforce);
        pr.release(p);
    } else {
        {
            EnforcementScope scope(enforce);
            p = pr.acquire();
        }
        r.got_block = p != nullptr;
        escape(p);
        pr.release(p); // enforcement is off again
    }
    guard::soft_mode = false;
    r.count = guard::violations;
    r.first_named_ok = guard::violations >= 1 && std::strcmp(guard::vlog[0].entry, pr.entry) == 0;
    return r;
}

struct FatalResult {
    bool sigabrt = false;
    bool named = false;
    int signal = 0;
    int exit_code = -1;
};

// Runs one probe in a forked child with the real (fatal) guard and reports how the child died and
// what it wrote to stderr. The child is single-threaded when forked.
FatalResult run_fatal(const Probe& pr) {
    FatalResult r;
    std::fflush(nullptr);
    int fds[2];
    if (::pipe(fds) != 0) return r;
    const pid_t pid = ::fork();
    if (pid < 0) { ::close(fds[0]); ::close(fds[1]); return r; }
    if (pid == 0) {
        ::close(fds[0]);
        ::dup2(fds[1], STDERR_FILENO);
        ::close(fds[1]);
        const rlimit no_core{ 0, 0 };
        ::setrlimit(RLIMIT_CORE, &no_core); // an expected abort must not leave a core file behind
        ::alarm(30);                        // a hung handler becomes SIGALRM, not a hung test
        void* p = nullptr;
        if (pr.is_release) {
            p = pr.acquire();
            escape(p);
            ZeroAllocScope scope;
            pr.release(p);
        } else {
            ZeroAllocScope scope;
            p = pr.acquire();
            escape(p);
        }
        ::_exit(97); // reached only if the guard did not fire
    }
    ::close(fds[1]);
    char out[8192];
    std::size_t len = 0;
    for (;;) {
        char scratch[512];
        char* dst = len + 1 < sizeof out ? out + len : scratch;
        const std::size_t room = len + 1 < sizeof out ? sizeof out - 1 - len : sizeof scratch;
        const ssize_t k = ::read(fds[0], dst, room);
        if (k > 0) { if (dst == out + len) len += static_cast<std::size_t>(k); continue; }
        if (k < 0 && errno == EINTR) continue;
        break; // EOF: the child is gone
    }
    out[len] = '\0';
    ::close(fds[0]);
    int status = 0;
    ::waitpid(pid, &status, 0);
    r.sigabrt = WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
    r.signal = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
    r.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    const std::string needle = std::string("ZERO-ALLOC VIOLATION: heap call '") + pr.entry + "' (";
    r.named = std::strstr(out, needle.c_str()) != nullptr;
    return r;
}

void test_matrix_soft() {
    std::printf("negative controls, in-process (every entry point, violations recorded)\n");
    int on_ok = 0, off_ok = 0;
    std::string bad;
    for (const Probe& pr : kProbes) {
        const SoftResult on = run_soft(pr, true);
        const SoftResult off = run_soft(pr, false);
        const bool a = on.got_block && on.count == 1 && on.first_named_ok;
        const bool b = off.got_block && off.count == 0;
        on_ok += a;
        off_ok += b;
        if (!a || !b) {
            bad += std::string("\n         ") + pr.entry + ": enforcing -> " + std::to_string(on.count) +
                   " record(s)" + (on.first_named_ok ? "" : ", wrong or missing name") +
                   "; not enforcing -> " + std::to_string(off.count);
        }
    }
    const std::string total = std::to_string(kProbeCount);
    check(on_ok == static_cast<int>(kProbeCount),
          ("all " + total + " entry points: enforcing records exactly one violation, under its own name").c_str());
    check(off_ok == static_cast<int>(kProbeCount),
          ("all " + total + " entry points: not enforcing records nothing (setup and warmup stay free)").c_str());
    if (!bad.empty()) std::printf("         details:%s\n", bad.c_str());
}

void test_matrix_fatal() {
    std::printf("negative controls, forked child (the real abort path, every entry point)\n");
    int ok = 0;
    std::string bad;
    for (const Probe& pr : kProbes) {
        const FatalResult f = run_fatal(pr);
        if (f.sigabrt && f.named) { ++ok; continue; }
        bad += std::string("\n         ") + pr.entry + ": sigabrt=" + (f.sigabrt ? "yes" : "no") +
               " named=" + (f.named ? "yes" : "no") + " signal=" + std::to_string(f.signal) +
               " exit=" + std::to_string(f.exit_code);
    }
    check(ok == static_cast<int>(kProbeCount),
          ("all " + std::to_string(kProbeCount) + " entry points: SIGABRT with a diagnostic naming the call").c_str());
    if (!bad.empty()) std::printf("         details:%s\n", bad.c_str());

    // The same abort, but raised on a worker thread that opted in: the window mechanics, end to end.
    std::fflush(nullptr);
    int fds[2];
    bool threaded_ok = false;
    if (::pipe(fds) == 0) {
        const pid_t pid = ::fork();
        if (pid == 0) {
            ::close(fds[0]);
            ::dup2(fds[1], STDERR_FILENO);
            ::close(fds[1]);
            const rlimit no_core{ 0, 0 };
            ::setrlimit(RLIMIT_CORE, &no_core);
            ::alarm(30);
            std::thread worker([] {
                ZeroAllocScope scope;
                void* p = ::operator new(8);
                escape(p);
            });
            worker.join();
            ::_exit(97);
        }
        ::close(fds[1]);
        char sink[256];
        while (::read(fds[0], sink, sizeof sink) > 0) {}
        ::close(fds[0]);
        int status = 0;
        ::waitpid(pid, &status, 0);
        threaded_ok = pid > 0 && WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
    }
    check(threaded_ok, "an allocation on an opted-in worker thread aborts the whole process");
}

struct ReachCase {
    const char* what;
    void (*run)();
};

const ReachCase kReach[] = {
    { "libc: strdup()", [] { char* s = ::strdup("animus"); escape(s); ::free(s); } },
    { "libc: fopen()/fclose()", [] { std::FILE* f = std::fopen("/dev/null", "r"); escape(f); if (f != nullptr) std::fclose(f); } },
    { "libstdc++: std::string growth", [] { std::string s(256, 'x'); escape(&s); } },
    { "libstdc++: std::vector", [] { std::vector<int> v(512); escape(&v); } },
    { "libc: pthread_create via std::thread", [] { std::thread t([] {}); t.join(); } },
};

void test_reach() {
    std::printf("reach: allocations made INSIDE libc / libstdc++ are seen (what ld --wrap cannot see)\n");
    for (const ReachCase& rc : kReach) {
        guard::soft_mode = true;
        guard::violations = 0;
        {
            ZeroAllocScope scope;
            rc.run();
        }
        const std::uint32_t seen = guard::violations;
        const char* first = seen > 0 ? guard::vlog[0].entry : "-";
        guard::violations = 0;
        rc.run(); // enforcement off
        const std::uint32_t quiet = guard::violations;
        guard::soft_mode = false;
        const std::string what = std::string(rc.what) + " -> " + std::to_string(seen) + " violation(s), first: " + first;
        check(seen >= 1 && quiet == 0, what.c_str());
    }
}

void test_counter_live() {
    std::printf("the process-wide counter used by the hot-window check\n");
    const std::uint64_t before = guard::g_calls.load();
    void* p = ::malloc(32);
    escape(p);
    ::free(p);
    check(guard::g_calls.load() >= before + 2, "advances when the heap is used outside any window (a counter that cannot move proves nothing)");
}

struct ThreadProbe {
    std::atomic<int> phase{ 0 };
    std::uint32_t other_thread_not_enforcing = 99;
    std::uint32_t other_thread_enforcing = 99;
};

void test_thread_locality_and_scopes() {
    std::printf("enforcement is per thread, and scopes nest\n");
    ThreadProbe probe;
    std::thread other([&probe] {
        guard::soft_mode = true;
        while (probe.phase.load(std::memory_order_acquire) < 1) _mm_pause();
        // The main thread is enforcing right now; this one is not.
        guard::violations = 0;
        void* a = ::operator new(8);
        escape(a);
        ::operator delete(a);
        probe.other_thread_not_enforcing = guard::violations;
        {
            ZeroAllocScope scope; // this thread opts in on its own
            void* b = ::operator new(8);
            escape(b);
            probe.other_thread_enforcing = guard::violations;
            AllocAllowedScope allow; // release `b` without a second record
            ::operator delete(b);
        }
        probe.phase.store(2, std::memory_order_release);
    });
    guard::soft_mode = true;
    guard::violations = 0;
    {
        ZeroAllocScope scope;
        probe.phase.store(1, std::memory_order_release);
        while (probe.phase.load(std::memory_order_acquire) < 2) _mm_pause(); // atomics only: no heap calls here
    }
    const std::uint32_t main_seen = guard::violations;
    guard::soft_mode = false;
    other.join();
    check(main_seen == 0, "a thread that has not opted in does not trip the enforcing thread, and vice versa");
    check(probe.other_thread_not_enforcing == 0, "a thread is free to allocate while another thread is enforcing");
    check(probe.other_thread_enforcing == 1, "a thread that opts in on its own is policed on its own");

    check(!g_enforce_zero_alloc, "the switch is off by default");
    {
        ZeroAllocScope outer;
        const bool on1 = g_enforce_zero_alloc;
        { ZeroAllocScope inner; }
        const bool on2 = g_enforce_zero_alloc;
        guard::soft_mode = true;
        guard::violations = 0;
        {
            AllocAllowedScope allow;
            void* p = ::malloc(16);
            escape(p);
            ::free(p);
        }
        const std::uint32_t allowed = guard::violations;
        const bool on3 = g_enforce_zero_alloc;
        guard::soft_mode = false;
        check(on1 && on2 && on3 && allowed == 0, "scopes nest: leaving an inner scope restores the outer one, AllocAllowedScope permits heap use inside");
    }
    check(!g_enforce_zero_alloc, "leaving the outermost scope turns enforcement off");
}

void test_passthrough_semantics() {
    std::printf("pass-through: the replacement allocators behave as the originals did\n");
    const std::size_t aligns[] = { 16, 32, 64, 128, 256, 4096 };
    const std::size_t page = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
    bool pm = true, aa = true, ma = true, nw = true;
    for (const std::size_t a : aligns) {
        void* p = nullptr;
        pm = pm && ::posix_memalign(&p, a, 100) == 0 && p != nullptr && reinterpret_cast<std::uintptr_t>(p) % a == 0;
        ::free(p);
        void* q = ::aligned_alloc(a, 100);
        aa = aa && q != nullptr && reinterpret_cast<std::uintptr_t>(q) % a == 0;
        ::free(q);
        void* m = ::memalign(a, 100);
        ma = ma && m != nullptr && reinterpret_cast<std::uintptr_t>(m) % a == 0;
        ::free(m);
        void* n = ::operator new(100, static_cast<std::align_val_t>(a));
        nw = nw && reinterpret_cast<std::uintptr_t>(n) % a == 0;
        ::operator delete(n, static_cast<std::align_val_t>(a));
    }
    check(pm && aa && ma && nw, "posix_memalign / aligned_alloc / memalign / aligned operator new honour 16..4096-byte alignment");

    void* v = ::valloc(100);
    void* pv = ::pvalloc(100);
    check(v != nullptr && reinterpret_cast<std::uintptr_t>(v) % page == 0 && pv != nullptr &&
              reinterpret_cast<std::uintptr_t>(pv) % page == 0 && ::malloc_usable_size(pv) >= page,
          "valloc is page-aligned; pvalloc is page-aligned and rounds the size up to a page");
    ::free(v);
    ::free(pv);

    volatile std::size_t not_pow2 = 24, too_small = 4, zero = 0;
    void* const sentinel = reinterpret_cast<void*>(std::uintptr_t{ 0x1 });
    void* out = sentinel;
    const int e1 = ::posix_memalign(&out, not_pow2, 64);
    const int e2 = ::posix_memalign(&out, too_small, 64);
    const int e3 = ::posix_memalign(&out, zero, 64);
    check(e1 == EINVAL && e2 == EINVAL && e3 == EINVAL && out == sentinel,
          "posix_memalign rejects a non-power-of-two, sub-pointer-size or zero alignment with EINVAL and leaves *memptr alone");
    errno = 0;
    void* bad = ::aligned_alloc(not_pow2, 48);
    check(bad == nullptr && errno == EINVAL, "aligned_alloc rejects a non-power-of-two alignment (NULL, EINVAL)");

    errno = 12345;
    void* ok_blk = nullptr;
    const int rc = ::posix_memalign(&ok_blk, 64, 100);
    check(rc == 0 && errno == 12345, "a successful posix_memalign leaves errno untouched");
    ::free(ok_blk);

    unsigned char* z = static_cast<unsigned char*>(::calloc(64, 8));
    bool zeroed = z != nullptr;
    for (std::size_t i = 0; zeroed && i < 512; ++i) zeroed = z[i] == 0;
    ::free(z);
    volatile std::size_t huge = SIZE_MAX / 2 + 1;
    errno = 0;
    void* ovf = ::calloc(huge, 2);
    check(zeroed && ovf == nullptr && errno == ENOMEM, "calloc zero-fills, and a count*size overflow returns NULL (ENOMEM)");

    unsigned char* r = static_cast<unsigned char*>(::malloc(32));
    for (unsigned i = 0; i < 32; ++i) r[i] = static_cast<unsigned char>(i * 7 + 1);
    r = static_cast<unsigned char*>(::realloc(r, 8192));
    bool kept = r != nullptr;
    for (unsigned i = 0; kept && i < 32; ++i) kept = r[i] == static_cast<unsigned char>(i * 7 + 1);
    ::free(r);
    check(kept, "realloc preserves the old contents when it grows a block");
}

// ===========================================================================
// 3. The hot path under enforcement
// ===========================================================================

constexpr std::size_t kRingCapacity = 1u << 14;
constexpr unsigned kMaxProducers = 4;
constexpr unsigned kMaxConsumers = 4;

inline std::uint64_t mix(std::uint64_t x) noexcept { return (x * 0x9E3779B97F4A7C15ull) ^ (x >> 29); }

// Spin briefly, then yield: stays live on a small CI box where producer and consumer share a CPU.
inline void relax(unsigned& spins) noexcept {
    if (++spins < 128) { _mm_pause(); return; }
    spins = 0;
    ::sched_yield();
}

// Per-worker results, one cache line apiece.
struct alignas(64) WorkerResult {
    std::uint64_t items = 0;
    std::uint64_t bad = 0;                              // integrity or ordering violations
    std::uint64_t count[kMaxProducers] = {};
    std::uint64_t sum[kMaxProducers] = {};
};

// The window. main() opens it when every worker is parked and closes it when every worker has left its
// hot region; workers then wait at a second gate so that thread teardown (std::thread deletes its
// state on the worker, libc frees the stack and TLS) happens AFTER the counter is read, not inside it.
struct Window {
    std::atomic<unsigned> parked{ 0 };
    std::atomic<bool> go{ false };
    std::atomic<unsigned> finished{ 0 };
    std::atomic<bool> release{ false };
    void park_and_wait() noexcept {
        parked.fetch_add(1, std::memory_order_acq_rel);
        unsigned spins = 0;
        while (!go.load(std::memory_order_acquire)) relax(spins);
    }
    void done_and_wait() noexcept {
        finished.fetch_add(1, std::memory_order_acq_rel);
        unsigned spins = 0;
        while (!release.load(std::memory_order_acquire)) relax(spins);
    }
};

struct HotReport {
    const char* id = "";   // short name of the ring under test
    const char* name = "";
    std::uint64_t items = 0;
    double seconds = 0.0;
    std::uint64_t calls_before = 0;
    std::uint64_t calls_in_window = ~std::uint64_t{ 0 }; // process-wide heap calls across the window; must be 0
    bool integrity = false;
};

// Opens the window, waits for it to close, fills in the timing and the process-wide heap-call delta,
// then lets the workers go. Nothing between the two counter reads allocates: this thread only spins on atomics.
void run_window(Window& win, unsigned workers, HotReport& rep) {
    unsigned spins = 0;
    while (win.parked.load(std::memory_order_acquire) < workers) relax(spins);
    rep.calls_before = guard::g_calls.load(std::memory_order_acquire);
    const auto t0 = std::chrono::steady_clock::now();
    win.go.store(true, std::memory_order_release);
    spins = 0;
    while (win.finished.load(std::memory_order_acquire) < workers) relax(spins);
    const auto t1 = std::chrono::steady_clock::now();
    const std::uint64_t after = guard::g_calls.load(std::memory_order_acquire);
    rep.seconds = std::chrono::duration<double>(t1 - t0).count();
    rep.calls_in_window = after - rep.calls_before;
    win.release.store(true, std::memory_order_release);
}

animus::TelemetryPayload make_payload(std::uint32_t producer, std::uint64_t i) noexcept {
    animus::TelemetryPayload p{};
    p.timestamp_cycles = (static_cast<std::uint64_t>(producer) << 48) | i;
    p.event_id = static_cast<std::uint32_t>(i);
    p.trace_id = producer;
    p.metric_value = mix(p.timestamp_cycles);
    return p;
}

using Payload = animus::TelemetryPayload;

// A report for a ring that could not be set up: fails both checks in report_hot() instead of hiding the gap.
HotReport setup_failed(const char* id, const char* name, const char* why) {
    HotReport rep;
    rep.id = id;
    rep.name = name;
    std::printf("         setup failed: %s\n", why);
    return rep; // calls_in_window stays at its sentinel, integrity false
}

// One producer pushes `iters` records and one consumer pops and checks each. `push` / `pop` adapt the ring
// under test; both run inside the enforced window, so they must not allocate.
template <typename PushFn, typename PopFn>
HotReport spsc_hot(const char* id, const char* name, std::uint64_t iters, PushFn push, PopFn pop) {
    HotReport rep;
    rep.id = id;
    rep.name = name;
    Window win;
    WorkerResult consumed;
    std::thread consumer([&] {
        win.park_and_wait();
        {
            ZeroAllocScope scope;
            Payload p{};
            std::uint64_t next = 0, bad = 0, sum = 0;
            unsigned spins = 0;
            while (next < iters) {
                if (!pop(p)) { relax(spins); continue; }
                const Payload want = make_payload(0, next);
                if (p.timestamp_cycles != want.timestamp_cycles || p.event_id != want.event_id ||
                    p.trace_id != want.trace_id || p.metric_value != want.metric_value) ++bad;
                sum += p.event_id;
                ++next;
            }
            consumed.items = next;
            consumed.bad = bad;
            consumed.sum[0] = sum;
        }
        win.done_and_wait();
    });
    std::thread producer([&] {
        win.park_and_wait();
        {
            ZeroAllocScope scope;
            unsigned spins = 0;
            for (std::uint64_t i = 0; i < iters; ++i) {
                const Payload p = make_payload(0, i);
                while (!push(p)) relax(spins);
            }
        }
        win.done_and_wait();
    });
    run_window(win, 2, rep);
    producer.join();
    consumer.join();
    rep.items = consumed.items;
    std::uint64_t expect_sum = 0;
    for (std::uint64_t i = 0; i < iters; ++i) expect_sum += static_cast<std::uint32_t>(i);
    rep.integrity = consumed.items == iters && consumed.bad == 0 && consumed.sum[0] == expect_sum;
    return rep;
}

// `producers` threads push `iters` records between them and `consumers` threads pop them, each consumer
// checking every record and per-producer order. Exact per-producer counts and index sums are verified at the end.
template <typename PushFn, typename PopFn>
HotReport mpmc_hot(const char* id, const char* name, std::uint64_t iters, unsigned producers, unsigned consumers,
                   PushFn push, PopFn pop) {
    HotReport rep;
    rep.id = id;
    rep.name = name;
    Window win;
    std::atomic<std::uint64_t> consumed_total{ 0 };
    WorkerResult results[kMaxConsumers]; // one per consumer; each holds per-producer counts
    std::vector<std::thread> threads;
    threads.reserve(producers + consumers);
    for (unsigned c = 0; c < consumers; ++c) {
        threads.emplace_back([&, c] {
            win.park_and_wait();
            {
                ZeroAllocScope scope;
                WorkerResult& mine = results[c];
                bool seen[kMaxProducers] = {};
                std::uint64_t last[kMaxProducers] = {};
                Payload p{};
                unsigned spins = 0;
                while (consumed_total.load(std::memory_order_relaxed) < iters) {
                    if (!pop(p)) { relax(spins); continue; }
                    consumed_total.fetch_add(1, std::memory_order_relaxed);
                    const std::uint32_t prod = p.trace_id;
                    if (prod >= producers) { ++mine.bad; continue; }
                    const std::uint64_t i = p.timestamp_cycles & ((std::uint64_t{ 1 } << 48) - 1);
                    const Payload want = make_payload(prod, i);
                    if (p.timestamp_cycles != want.timestamp_cycles || p.event_id != want.event_id ||
                        p.metric_value != want.metric_value) ++mine.bad;
                    if (seen[prod] && i <= last[prod]) ++mine.bad; // per-producer order, per consumer
                    seen[prod] = true;
                    last[prod] = i;
                    ++mine.count[prod];
                    mine.sum[prod] += i;
                    ++mine.items;
                }
            }
            win.done_and_wait();
        });
    }
    for (unsigned pr = 0; pr < producers; ++pr) {
        threads.emplace_back([&, pr] {
            const std::uint64_t share = iters / producers + (pr < iters % producers ? 1 : 0);
            win.park_and_wait();
            {
                ZeroAllocScope scope;
                unsigned spins = 0;
                for (std::uint64_t i = 0; i < share; ++i) {
                    const Payload p = make_payload(pr, i);
                    while (!push(p)) relax(spins);
                }
            }
            win.done_and_wait();
        });
    }
    run_window(win, producers + consumers, rep);
    for (std::thread& t : threads) t.join();

    std::uint64_t total = 0, bad = 0;
    bool exact = true;
    for (unsigned pr = 0; pr < producers; ++pr) {
        const std::uint64_t share = iters / producers + (pr < iters % producers ? 1 : 0);
        std::uint64_t count = 0, sum = 0;
        for (unsigned c = 0; c < consumers; ++c) { count += results[c].count[pr]; sum += results[c].sum[pr]; }
        exact = exact && count == share && sum == share * (share - 1) / 2; // exact count and index sum per producer
    }
    for (unsigned c = 0; c < consumers; ++c) { total += results[c].items; bad += results[c].bad; }
    rep.items = total;
    rep.integrity = exact && bad == 0 && total == iters;
    return rep;
}

HotReport run_spsc(std::uint64_t iters) {
    animus::SpscRingBuffer<Payload> ring(kRingCapacity); // storage allocated here, before the window
    return spsc_hot("SpscRingBuffer", "SPSC  animus::SpscRingBuffer<TelemetryPayload>, 1 producer / 1 consumer", iters,
                    [&](const Payload& p) { return ring.push(p); }, [&](Payload& p) { return ring.pop(p); });
}

HotReport run_mpmc(std::uint64_t iters, unsigned producers, unsigned consumers) {
    animus::LockFreeRingBuffer<Payload> ring(kRingCapacity);
    return mpmc_hot("LockFreeRingBuffer", "MPMC  animus::LockFreeRingBuffer<TelemetryPayload>, 2 producers / 2 consumers",
                    iters, producers, consumers, [&](const Payload& p) { return ring.push(p); },
                    [&](Payload& p) { return ring.pop(p); });
}

// The header-based SPSC queue, audited in addition to the in-process SpscRingBuffer. Producer and consumer each
// hold their own view of one block.
HotReport run_spsc_queue(std::uint64_t iters) {
    using Queue = animus::sys::ipc::SpscQueue<Payload>;
    static const char* const kId = "SpscQueue";
    static const char* const kName = "SPSC  animus::sys::ipc::SpscQueue<TelemetryPayload>, 1 producer / 1 consumer";
    animus::bench::AlignedBlock block(Queue::required_bytes(kRingCapacity));
    Queue producer_view, consumer_view;
    if (!block || !Queue::init(block.data(), block.size(), kRingCapacity, producer_view) ||
        !Queue::attach(block.data(), block.size(), consumer_view)) {
        return setup_failed(kId, kName, "could not allocate or initialise the SpscQueue block");
    }
    return spsc_hot(kId, kName, iters, [&](const Payload& p) { return producer_view.try_push(p); },
                    [&](Payload& p) { return consumer_view.try_pop(p); });
}

// bench_tsc.hpp's timed loop, run under the guard. Setup (pinning, buffer, calibration) is outside.
HotReport run_profiler(std::uint64_t samples, animus::bench::LatencyReport& out) {
    HotReport rep;
    rep.id = "ProfilerLoop";
    rep.name = "TSC   animus::bench::Profiler::run(), one thread pinned";
    animus::SpscRingBuffer<animus::TelemetryPayload> ring(1024);
    animus::bench::ProfilerConfig cfg;
    cfg.samples = static_cast<std::size_t>(samples);
    cfg.warmup = 20000;
    cfg.require_invariant_tsc = false; // this phase is about allocation, not timing quality
    cfg.calibration_windows = 3;
    cfg.calibration_window_ns = 20000000ull;
    cfg.overhead_samples = 20000;
    animus::bench::Profiler prof(cfg);
    if (!prof.ok()) {
        std::printf("         profiler setup failed: %s\n", prof.error());
        return rep;
    }
    std::uint64_t seq = 0, bad = 0;
    animus::TelemetryPayload in{}, got{};
    rep.calls_before = guard::g_calls.load(std::memory_order_acquire);
    const auto t0 = std::chrono::steady_clock::now();
    {
        ZeroAllocScope scope;
        prof.run([&] {
            in.timestamp_cycles = ++seq;
            ring.push(in);
            ring.pop(got);
            bad += got.timestamp_cycles != seq;
        });
    }
    const auto t1 = std::chrono::steady_clock::now();
    const std::uint64_t after = guard::g_calls.load(std::memory_order_acquire);
    rep.seconds = std::chrono::duration<double>(t1 - t0).count();
    out = prof.finish("push+pop on one thread (informational)");
    rep.calls_in_window = after - rep.calls_before;
    rep.items = out.n;
    rep.integrity = bad == 0 && out.n == samples && seq == samples + cfg.warmup;
    return rep;
}

void report_hot(const HotReport& r, std::uint64_t expected_items) {
    std::printf("%s\n", r.name);
    std::printf("         %llu items in %.3f s, process-wide heap calls inside the window: %llu\n",
                static_cast<unsigned long long>(r.items), r.seconds, static_cast<unsigned long long>(r.calls_in_window));
    check(r.calls_in_window == 0, "zero heap calls anywhere in the process while the window was open");
    if (r.calls_in_window != 0) {
        // Which calls? The log holds the last kRecent, indexed by call number.
        const std::uint64_t shown = r.calls_in_window < guard::kRecent ? r.calls_in_window : guard::kRecent;
        for (std::uint64_t k = 0; k < shown; ++k) {
            const std::uint64_t call = r.calls_before + k;
            const char* name = guard::g_recent[call % guard::kRecent].load(std::memory_order_relaxed);
            std::printf("         heap call #%llu in the window: %s\n", static_cast<unsigned long long>(k + 1), name != nullptr ? name : "?");
        }
    }
    check(r.integrity && r.items == expected_items, "every record arrived intact (and, for MPMC, with exact per-producer counts and index sums, in per-producer order)");
}

} // namespace

int main(int argc, char** argv) {
    std::uint64_t iters = 10000000;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--iters") == 0 && i + 1 < argc) {
            iters = std::strtoull(argv[++i], nullptr, 10);
        } else {
            std::fprintf(stderr, "usage: %s [--iters N]   (default 10000000 per ring)\n", argv[0]);
            return 2;
        }
    }
    if (iters < 16) { std::fprintf(stderr, "--iters must be at least 16\n"); return 2; }

    std::printf("AnimusCore zero-allocation audit: iterations per ring %llu, compiler %s, glibc %s, %u hardware threads\n\n",
                static_cast<unsigned long long>(iters), __VERSION__, ::gnu_get_libc_version(), std::thread::hardware_concurrency());

    // Load libgcc's unwinder now, so the violation handler's backtrace() never has to dlopen under duress.
    void* warm[4];
    (void)::backtrace(warm, 4);

    // The fork tests need a single-threaded process, so they run before any thread exists.
    test_counter_live();
    test_passthrough_semantics();
    test_matrix_soft();
    test_matrix_fatal();
    test_reach();
    test_thread_locality_and_scopes();


    std::printf("\nhot path, enforcement on for every worker thread\n");
    {
        const HotReport spsc = run_spsc(iters);
        report_hot(spsc, iters);
        const HotReport spsc_queue = run_spsc_queue(iters);
        report_hot(spsc_queue, iters);
        const HotReport mpmc = run_mpmc(iters, 2, 2);
        report_hot(mpmc, iters);
        animus::bench::LatencyReport tsc;
        const std::uint64_t samples = iters < 1000000 ? iters : 1000000;
        const HotReport prof = run_profiler(samples, tsc);
        report_hot(prof, samples);
        if (tsc.n != 0) {
            std::printf("         (informational, TSC ticks, timer cost %llu): push+pop p50 %llu p99 %llu net ticks = %.1f / %.1f ns on CPU %d\n",
                        static_cast<unsigned long long>(tsc.overhead_ticks),
                        static_cast<unsigned long long>(tsc.net(tsc.p50)), static_cast<unsigned long long>(tsc.net(tsc.p99)),
                        tsc.ns(tsc.net(tsc.p50)), tsc.ns(tsc.net(tsc.p99)), tsc.cpu);
        }
    }

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASSED" : "FAILED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
