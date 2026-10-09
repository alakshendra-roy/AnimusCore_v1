#pragma once
// Counting global allocator for the zero-allocation proofs in the verification
// harnesses (tests/eval_harness.cpp, tests/soak_shm_mpmc.cpp).
//
// Include this from EXACTLY ONE translation unit of an executable: it defines
// the replaceable global allocation functions, and the standard allows a
// program only one definition of each. Both harnesses are single-TU programs.
//
// Two layers of evidence, because "no allocation" can be violated in two ways:
//
//   1. operator new / operator new[] / delete / delete[] -- every form,
//      including the nothrow, sized, and over-aligned overloads -- are
//      replaced and counted. This catches anything C++ code allocates (a
//      std::vector growing, a std::function capturing, a make_unique).
//
//   2. malloc / calloc / realloc / posix_memalign / aligned_alloc / free, when
//      the build passes -DANIMUS_WRAP_MALLOC=1 and the GNU linker flags in
//      ANIMUS_WRAP_MALLOC_LDFLAGS (CMake and the Makefile do this on GCC for
//      Linux and MinGW). `ld --wrap=malloc` redirects every reference to
//      malloc from the objects being linked -- the ring headers and the
//      harness, which is the code under test -- to the wrappers below. It does
//      not reach calls made from inside libc.so/libstdc++.so themselves
//      (those resolve at load time), which is exactly why layer 1 exists: the
//      C++ runtime's own allocations all enter through operator new, which is
//      replaced here. MSVC has no equivalent link-time wrap, so there only
//      layer 1 is active and malloc_instrumented() reports false rather than
//      claiming a count it did not take.
//
// arm() turns the counters into an assertion: while armed, ANY allocation or
// deallocation from ANY thread prints the offending call and abort()s. The
// harnesses arm it for exactly the timed window and disarm it afterwards, so
// a regression that puts the allocator on the hot path fails loudly at the
// call site instead of showing up as a nonzero delta in a table.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>

#if defined(_WIN32)
    #include <malloc.h> // _aligned_malloc / _aligned_free
#endif

// GCC flags the replaced operator new/delete pair below (malloc/free under the
// hood) as mismatched once std::vector inlines them; known false positive for
// exactly this counting-allocator idiom (same suppression as test_audit_ring.cpp).
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif

namespace animus_test {

struct AllocCounters {
    uint64_t new_calls = 0;      // operator new / new[] (all overloads)
    uint64_t delete_calls = 0;   // operator delete / delete[] (non-null pointers only)
    uint64_t malloc_calls = 0;   // malloc/calloc/realloc/posix_memalign/aligned_alloc (when instrumented)
    uint64_t free_calls = 0;     // free (non-null pointers only; when instrumented)

    uint64_t total() const { return new_calls + delete_calls + malloc_calls + free_calls; }
};

inline std::atomic<uint64_t> g_new_calls{ 0 };
inline std::atomic<uint64_t> g_delete_calls{ 0 };
inline std::atomic<uint64_t> g_malloc_calls{ 0 };
inline std::atomic<uint64_t> g_free_calls{ 0 };
inline std::atomic<bool> g_armed{ false };

constexpr bool malloc_instrumented() noexcept {
#if defined(ANIMUS_WRAP_MALLOC)
    return true;
#else
    return false;
#endif
}

inline AllocCounters snapshot() noexcept {
    AllocCounters c;
    c.new_calls = g_new_calls.load(std::memory_order_relaxed);
    c.delete_calls = g_delete_calls.load(std::memory_order_relaxed);
    c.malloc_calls = g_malloc_calls.load(std::memory_order_relaxed);
    c.free_calls = g_free_calls.load(std::memory_order_relaxed);
    return c;
}

inline AllocCounters delta(const AllocCounters& before, const AllocCounters& after) noexcept {
    AllocCounters d;
    d.new_calls = after.new_calls - before.new_calls;
    d.delete_calls = after.delete_calls - before.delete_calls;
    d.malloc_calls = after.malloc_calls - before.malloc_calls;
    d.free_calls = after.free_calls - before.free_calls;
    return d;
}

inline void arm() noexcept { g_armed.store(true, std::memory_order_seq_cst); }
inline void disarm() noexcept { g_armed.store(false, std::memory_order_seq_cst); }

// Optimization barrier: the compiler must assume `v` is read and modified from here on.
//  * Negative controls: [expr.new] lets a compiler drop a new/delete pair whose storage is never
//    observed, and clang does at -O2. Passing the pointer of a deliberate allocation through this
//    keeps it, so a counter check that fails really means "the counter is dead".
//  * Timed regions: forces a value to be materialised in memory before the first timestamp.
#if defined(_MSC_VER)
inline void* volatile g_escape_sink = nullptr;
template <typename T>
inline void escape(T& v) noexcept { g_escape_sink = &v; }
#else
template <typename T>
inline void escape(T& v) noexcept { asm volatile("" : : "g"(&v) : "memory"); }
#endif

// Called on every allocation/deallocation. Cheap when disarmed: one relaxed load.
inline void note(std::atomic<uint64_t>& counter, const char* what) noexcept {
    counter.fetch_add(1, std::memory_order_relaxed);
    if (g_armed.load(std::memory_order_relaxed)) {
        std::fprintf(stderr,
            "\nFATAL: heap activity (%s) inside the armed zero-allocation window -- "
            "the hot path must not touch the allocator. Aborting.\n", what);
        std::fflush(stderr);
        std::abort();
    }
}

} // namespace animus_test

// ---------------------------------------------------------------------------
// Replaceable global allocation functions.
// ---------------------------------------------------------------------------
namespace animus_test {
inline void* alloc_plain(std::size_t n) {
    note(g_new_calls, "operator new");
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
inline void* alloc_plain_nothrow(std::size_t n) noexcept {
    note(g_new_calls, "operator new(nothrow)");
    return std::malloc(n ? n : 1);
}
inline void* alloc_aligned(std::size_t n, std::size_t align) {
    note(g_new_calls, "operator new(align_val_t)");
    if (align < sizeof(void*)) align = sizeof(void*);
    void* p = nullptr;
#if defined(_WIN32)
    p = _aligned_malloc(n ? n : 1, align);
#else
    if (posix_memalign(&p, align, n ? n : 1) != 0) p = nullptr;
#endif
    if (!p) throw std::bad_alloc();
    return p;
}
inline void free_plain(void* p) noexcept {
    if (!p) return;
    note(g_delete_calls, "operator delete");
    std::free(p);
}
inline void free_aligned(void* p) noexcept {
    if (!p) return;
    note(g_delete_calls, "operator delete(align_val_t)");
#if defined(_WIN32)
    _aligned_free(p);
#else
    std::free(p);
#endif
}
} // namespace animus_test

void* operator new(std::size_t n) { return animus_test::alloc_plain(n); }
void* operator new[](std::size_t n) { return animus_test::alloc_plain(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { return animus_test::alloc_plain_nothrow(n); }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { return animus_test::alloc_plain_nothrow(n); }
void* operator new(std::size_t n, std::align_val_t a) { return animus_test::alloc_aligned(n, static_cast<std::size_t>(a)); }
void* operator new[](std::size_t n, std::align_val_t a) { return animus_test::alloc_aligned(n, static_cast<std::size_t>(a)); }

void operator delete(void* p) noexcept { animus_test::free_plain(p); }
void operator delete[](void* p) noexcept { animus_test::free_plain(p); }
void operator delete(void* p, std::size_t) noexcept { animus_test::free_plain(p); }
void operator delete[](void* p, std::size_t) noexcept { animus_test::free_plain(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { animus_test::free_plain(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { animus_test::free_plain(p); }
void operator delete(void* p, std::align_val_t) noexcept { animus_test::free_aligned(p); }
void operator delete[](void* p, std::align_val_t) noexcept { animus_test::free_aligned(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { animus_test::free_aligned(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { animus_test::free_aligned(p); }

// ---------------------------------------------------------------------------
// Layer 2: malloc-family wrappers (GNU ld --wrap). Only compiled when the build
// asked for them; the matching -Wl,--wrap=... flags must be on the link line,
// otherwise these symbols are simply never referenced.
// ---------------------------------------------------------------------------
#if defined(ANIMUS_WRAP_MALLOC)
extern "C" {
void* __real_malloc(std::size_t);
void* __real_calloc(std::size_t, std::size_t);
void* __real_realloc(void*, std::size_t);
void __real_free(void*);
#if !defined(_WIN32)
int __real_posix_memalign(void**, std::size_t, std::size_t);
void* __real_aligned_alloc(std::size_t, std::size_t);
#else
void* __real__aligned_malloc(std::size_t, std::size_t);
void __real__aligned_free(void*);
#endif

void* __wrap_malloc(std::size_t n) {
    animus_test::note(animus_test::g_malloc_calls, "malloc");
    return __real_malloc(n);
}
void* __wrap_calloc(std::size_t a, std::size_t b) {
    animus_test::note(animus_test::g_malloc_calls, "calloc");
    return __real_calloc(a, b);
}
void* __wrap_realloc(void* p, std::size_t n) {
    animus_test::note(animus_test::g_malloc_calls, "realloc");
    return __real_realloc(p, n);
}
#if !defined(_WIN32)
int __wrap_posix_memalign(void** pp, std::size_t align, std::size_t n) {
    animus_test::note(animus_test::g_malloc_calls, "posix_memalign");
    return __real_posix_memalign(pp, align, n);
}
void* __wrap_aligned_alloc(std::size_t align, std::size_t n) {
    animus_test::note(animus_test::g_malloc_calls, "aligned_alloc");
    return __real_aligned_alloc(align, n);
}
#else
void* __wrap__aligned_malloc(std::size_t n, std::size_t align) {
    animus_test::note(animus_test::g_malloc_calls, "_aligned_malloc");
    return __real__aligned_malloc(n, align);
}
void __wrap__aligned_free(void* p) {
    if (p) animus_test::note(animus_test::g_free_calls, "_aligned_free");
    __real__aligned_free(p);
}
#endif
void __wrap_free(void* p) {
    if (p) animus_test::note(animus_test::g_free_calls, "free");
    __real_free(p);
}
} // extern "C"
#endif
