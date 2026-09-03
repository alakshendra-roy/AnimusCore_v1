#pragma once
// Fault-tolerant shared-memory lifecycle: signal-safe teardown plus
// cross-process liveness checks (Milestone 2).
//
// Two independent problems, both about the boundary between "a process
// attached to a ShmRing segment" and "the OS object that segment lives in":
//
//   1. Clean detach vs. destruction. A process that owns (created) a
//      segment must be able to react to SIGINT/SIGTERM by unmapping its own
//      view and exiting -- without unlinking the underlying /dev/shm node
//      out from under a still-running peer on the other end. SIGSEGV is
//      different in kind: by the time it fires, the process's own memory
//      state may be corrupt, so the only safe reaction is the smallest
//      possible one (flip a flag, no allocation, no I/O) before falling
//      through to the default handler so the OS still produces a core dump.
//      SignalGuard below implements exactly that split: SIGINT/SIGTERM set
//      a flag a normal control-flow loop polls and reacts to by detaching
//      in the ordinary way; SIGSEGV/SIGABRT set a different flag and then
//      re-raise with the default disposition restored, rather than trying
//      to unmap or unlink anything from inside the handler itself.
//
//   2. Zombie-state prevention. A consumer killed with `kill -9` gets no
//      handler at all -- SignalGuard cannot help there, because nothing
//      run by the dying process is involved. What the *surviving* side
//      needs instead is a way to notice the peer is gone without that
//      detection itself depending on any wait that could hang: pid
//      liveness (is_process_alive below), which the OS answers immediately
//      and unconditionally even for a process killed with SIGKILL. This
//      module supplies is_process_alive()/current_process_id(); ShmRing
//      (shm_ipc.hpp) is what actually publishes pid/heartbeat fields into
//      the shared header and exposes is_producer_alive()/is_consumer_alive()
//      built on top of them.
//
// Header-only, dependency-free (beyond platform headers), noexcept
// throughout -- same rationale as thread_affinity.hpp: every entry point
// here can be called from a signal handler or a hot control loop, and an
// exception unwinding out of either is worse than the call reporting
// failure.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <csignal>

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
    #define NOMINMAX
    #endif
    #include <windows.h>
#else
    #include <sys/types.h>
    #include <unistd.h>
    #include <errno.h>
#endif

namespace animus {
namespace sys {
namespace lifecycle {

    // Current process's OS process id, as a plain uint64_t so it fits the
    // same atomic field type ShmRing's Header uses for every other counter
    // (avoids mixing pid_t/DWORD into that header's otherwise-uniform
    // uint64_t layout).
    inline uint64_t current_process_id() noexcept {
#if defined(_WIN32)
        return static_cast<uint64_t>(::GetCurrentProcessId());
#else
        return static_cast<uint64_t>(::getpid());
#endif
    }

    // True if `pid` refers to a currently-running process. Answers
    // immediately and correctly even for a process that was killed with
    // SIGKILL (kill -9) and therefore never ran any cleanup of its own --
    // this is the primitive that makes zombie-state detection possible
    // without depending on the dead process having done anything.
    //
    // POSIX: kill(pid, 0) sends no signal, just performs the permission/
    // existence check; ESRCH means "no such process". Windows: OpenProcess
    // fails outright once the process object is gone, and (a narrow window
    // where a pid could be reused by an unrelated new process is inherent
    // to pid-based liveness on any OS, not specific to this check) a
    // successfully-opened handle's exit code is also checked so a process
    // that has already exited but whose handle is still technically open
    // elsewhere is correctly reported as not alive.
    inline bool is_process_alive(uint64_t pid) noexcept {
        if (pid == 0) return false;
#if defined(_WIN32)
        HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
        if (!h) return false;
        DWORD exit_code = 0;
        const bool got_exit_code = ::GetExitCodeProcess(h, &exit_code) != 0;
        ::CloseHandle(h);
        return got_exit_code && exit_code == STILL_ACTIVE;
#else
        return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno != ESRCH;
#endif
    }

    namespace detail {

        // Async-signal-safe global state a handler is allowed to touch: a
        // lock-free, sig_atomic_t-backed atomic (guaranteed safe to write
        // from a signal handler, unlike almost anything else in the
        // standard library -- no malloc, no iostream, no mutex). One
        // instance per process, since POSIX signal handlers are inherently
        // process-global; a per-object SignalGuard below is a thin RAII
        // wrapper around registering/restoring the OS-level handler, not a
        // way to have multiple independent signal states.
        inline std::atomic<bool> g_shutdown_requested{false};
        inline std::atomic<bool> g_fault_detected{false};

#if !defined(_WIN32)
        inline struct sigaction g_prev_sigint{};
        inline struct sigaction g_prev_sigterm{};
        inline struct sigaction g_prev_sigsegv{};
        inline struct sigaction g_prev_sigabrt{};

        // SIGINT/SIGTERM handler: the entire body is one relaxed atomic
        // store. Nothing else is async-signal-safe to do here -- the
        // actual detach (unmap the segment, and only the owning side
        // unlink it) happens in ordinary control flow once
        // shutdown_requested() is observed true, per this header's own
        // doc comment above.
        inline void handle_graceful(int) noexcept {
            g_shutdown_requested.store(true, std::memory_order_relaxed);
        }

        // SIGSEGV/SIGABRT handler: also just a flag store, but the flag is
        // "fault_detected", not "shutdown_requested" -- a caller that polls
        // fault_detected() (realistically: nothing running on this thread
        // ever will again once SIGSEGV has fired, but a signal-monitor
        // thread elsewhere in the same process, or a supervisor process
        // watching the header's crash flag, can) knows this exit is a
        // crash, not a clean shutdown, and must not treat the segment as
        // safe to unlink -- a crashed producer's last-written slot may be
        // torn. After the store, restores the previous (default) handler
        // and re-raises so the OS still terminates the process the normal
        // way for that signal (core dump for SIGSEGV/SIGABRT), rather than
        // this handler silently swallowing a fault that should be fatal.
        inline void handle_fault(int sig) noexcept {
            g_fault_detected.store(true, std::memory_order_relaxed);
            struct sigaction* prev = (sig == SIGSEGV) ? &g_prev_sigsegv : &g_prev_sigabrt;
            ::sigaction(sig, prev, nullptr);
            ::raise(sig);
        }
#else
        // Windows: Ctrl+C/Break/Close/Logoff/Shutdown console events map to
        // the same "please shut down cleanly" intent as SIGINT/SIGTERM on
        // POSIX. There is no direct equivalent of installing a SIGSEGV
        // handler via signal()/sigaction() -- access violations are
        // structured exceptions, not POSIX signals -- so this path relies
        // on Windows' own top-level unhandled-exception/WER crash dump
        // instead of trying to intercept it; SignalGuard::fault_detected()
        // is therefore always false on this platform. Note the returned
        // TRUE here does NOT prevent Windows from also terminating the
        // process for these events after the handler runs -- like the
        // POSIX SIGSEGV handler above, this stores a flag and does not
        // attempt to be the actual shutdown/detach logic.
        inline BOOL WINAPI handle_console_event(DWORD ctrl_type) noexcept {
            switch (ctrl_type) {
                case CTRL_C_EVENT:
                case CTRL_BREAK_EVENT:
                case CTRL_CLOSE_EVENT:
                case CTRL_LOGOFF_EVENT:
                case CTRL_SHUTDOWN_EVENT:
                    g_shutdown_requested.store(true, std::memory_order_relaxed);
                    return TRUE;
                default:
                    return FALSE;
            }
        }
#endif

    } // namespace detail

    // RAII installer for process-wide graceful-shutdown (and, on POSIX,
    // best-effort fault) signal handling. Construct exactly one of these
    // near the top of main() in any process that owns or attaches to a
    // ShmRing segment; destroying it restores whatever handler was
    // installed before construction.
    //
    // Usage in a producer/consumer main loop:
    //
    //   animus::sys::lifecycle::SignalGuard guard;
    //   while (!guard.shutdown_requested() && !guard.fault_detected()) {
    //       ring->push_overwrite(next_event());
    //   }
    //   // ordinary detach: ring.reset() unmaps this process's view. Only
    //   // the process that called ShmRing::create() should additionally
    //   // call ShmRing::unlink(name) here, and only after this process's
    //   // own detach -- never from inside a signal handler.
    //
    // Because POSIX signal disposition is process-global, only one
    // SignalGuard should be alive at a time per process; nesting two just
    // means the second's destructor restores the first's handler, not the
    // original pre-process one, which is a real but rarely-hit sharp edge
    // worth stating rather than hiding.
    class SignalGuard {
    public:
        SignalGuard() noexcept {
#if defined(_WIN32)
            ::SetConsoleCtrlHandler(detail::handle_console_event, TRUE);
            installed_ = true;
#else
            struct sigaction sa_graceful{};
            sa_graceful.sa_handler = detail::handle_graceful;
            ::sigemptyset(&sa_graceful.sa_mask);
            sa_graceful.sa_flags = 0; // no SA_RESTART: a spin-poll loop should see EINTR-equivalent wakeups promptly, not have them swallowed
            ::sigaction(SIGINT, &sa_graceful, &detail::g_prev_sigint);
            ::sigaction(SIGTERM, &sa_graceful, &detail::g_prev_sigterm);

            struct sigaction sa_fault{};
            sa_fault.sa_handler = detail::handle_fault;
            ::sigemptyset(&sa_fault.sa_mask);
            sa_fault.sa_flags = 0;
            ::sigaction(SIGSEGV, &sa_fault, &detail::g_prev_sigsegv);
            ::sigaction(SIGABRT, &sa_fault, &detail::g_prev_sigabrt);
            installed_ = true;
#endif
            detail::g_shutdown_requested.store(false, std::memory_order_relaxed);
            detail::g_fault_detected.store(false, std::memory_order_relaxed);
        }

        ~SignalGuard() {
            if (!installed_) return;
#if defined(_WIN32)
            ::SetConsoleCtrlHandler(detail::handle_console_event, FALSE);
#else
            ::sigaction(SIGINT, &detail::g_prev_sigint, nullptr);
            ::sigaction(SIGTERM, &detail::g_prev_sigterm, nullptr);
            ::sigaction(SIGSEGV, &detail::g_prev_sigsegv, nullptr);
            ::sigaction(SIGABRT, &detail::g_prev_sigabrt, nullptr);
#endif
        }

        SignalGuard(const SignalGuard&) = delete;
        SignalGuard& operator=(const SignalGuard&) = delete;

        // True once SIGINT/SIGTERM (or, on Windows, a Ctrl+C/Break/Close/
        // Logoff/Shutdown console event) has been observed. Poll this from
        // ordinary control flow -- a hot spin loop can check it every N
        // iterations rather than every single one, if even the relaxed
        // atomic load's cost matters on that specific path.
        static bool shutdown_requested() noexcept {
            return detail::g_shutdown_requested.load(std::memory_order_relaxed);
        }

        // True once SIGSEGV or SIGABRT has been observed (POSIX only --
        // always false on Windows; see the class doc comment above for
        // why). By the time this is observably true from another thread,
        // the faulting thread itself is already back inside the default
        // handler and about to terminate the process -- this exists for a
        // separate monitor thread or a supervisor process to notice
        // *before* that termination completes, not for the faulting
        // thread's own control flow to react to.
        static bool fault_detected() noexcept {
            return detail::g_fault_detected.load(std::memory_order_relaxed);
        }

    private:
        bool installed_ = false;
    };

} // namespace lifecycle
} // namespace sys
} // namespace animus
