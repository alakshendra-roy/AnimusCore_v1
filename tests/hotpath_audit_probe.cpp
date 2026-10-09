// Probe translation unit for scripts/audit_hotpath_symbols.py.
//
// The ring primitives are header-only templates that the optimizer inlines
// into the caller, so there is no function in a normal build whose machine
// code *is* "the hot path". This file gives each hot-path operation one
// out-of-line, C-linkage symbol -- animus_probe_<primitive>_<operation> -- by
// calling it through a pointer the compiler cannot see through. The audit
// script compiles this file with the same optimization flags as the benchmark
// (-O3 -march=native), disassembles the object, and walks the call graph from
// every animus_probe_* symbol, failing if any path reaches an allocator
// (malloc/calloc/realloc/free, operator new/delete, aligned/posix allocation)
// or a C++ exception-throw entry.
//
// Negative control: animus_negctl_* symbols allocate on purpose. The audit must
// FLAG them; if it does not, the audit is blind and its "clean" verdict on the
// real probes would be worthless.
//
// This file is never linked into a program; it only needs to compile to an
// object. Build: g++ -std=c++17 -O3 -march=native -Iinclude -c tests/hotpath_audit_probe.cpp
#include "../include/animus/broadcast_ring.hpp"
#include "../include/animus/execution_event.hpp"
#include "../include/animus/shm_ipc.hpp"
#include "../include/animus/spsc_queue.hpp"

#include <cstddef>
#include <new>

using animus::ExecutionEvent;
using animus::sys::ipc::BroadcastRing;
using animus::sys::ipc::ShmRing;
using animus::sys::ipc::ShmRingMpmc;
using animus::sys::ipc::SpmcRing;
using animus::sys::ipc::SpscQueue;

#if defined(_MSC_VER)
    #define ANIMUS_PROBE extern "C" __declspec(noinline)
#else
    #define ANIMUS_PROBE extern "C" __attribute__((noinline, used))
#endif

// ---- ShmRingMpmc ---------------------------------------------------------
ANIMUS_PROBE bool animus_probe_mpmc_try_push(ShmRingMpmc<ExecutionEvent>* r, const ExecutionEvent* e) { return r->try_push(*e); }
ANIMUS_PROBE bool animus_probe_mpmc_try_pop(ShmRingMpmc<ExecutionEvent>* r, ExecutionEvent* out) { return r->try_pop(*out); }

// ---- ShmRing (SPSC, legacy) ----------------------------------------------
ANIMUS_PROBE bool animus_probe_shm_ring_try_push(ShmRing<ExecutionEvent>* r, const ExecutionEvent* e) { return r->try_push(*e); }
ANIMUS_PROBE bool animus_probe_shm_ring_try_pop(ShmRing<ExecutionEvent>* r, ExecutionEvent* out) { return r->try_pop(*out); }
ANIMUS_PROBE void animus_probe_shm_ring_push_overwrite(ShmRing<ExecutionEvent>* r, const ExecutionEvent* e) { r->push_overwrite(*e); }

// ---- SpmcRing (broadcast, legacy) ----------------------------------------
ANIMUS_PROBE void animus_probe_spmc_broadcast(SpmcRing<ExecutionEvent>* r, const ExecutionEvent* e) { r->broadcast(*e); }
ANIMUS_PROBE size_t animus_probe_spmc_poll(SpmcRing<ExecutionEvent>* r, ExecutionEvent* out, size_t n) { return r->poll(out, n); }

// ---- SpscQueue / BroadcastRing (current dual-primitive transport) --------
ANIMUS_PROBE bool animus_probe_spsc_queue_try_push(SpscQueue<ExecutionEvent>* q, const ExecutionEvent* e) { return q->try_push(*e); }
ANIMUS_PROBE bool animus_probe_spsc_queue_try_pop(SpscQueue<ExecutionEvent>* q, ExecutionEvent* out) { return q->try_pop(*out); }
ANIMUS_PROBE void animus_probe_broadcast_publish(BroadcastRing<ExecutionEvent>* r, const ExecutionEvent* e) { r->publish(*e); }
ANIMUS_PROBE size_t animus_probe_broadcast_poll(BroadcastRing<ExecutionEvent>* r, ExecutionEvent* out, size_t n) { return r->poll(out, n); }

// ---- negative controls: these MUST be flagged ----------------------------
ANIMUS_PROBE void* animus_negctl_operator_new(size_t n) { return ::operator new(n); }
ANIMUS_PROBE void animus_negctl_operator_delete(void* p) { ::operator delete(p); }
