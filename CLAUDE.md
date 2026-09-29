# Animus Core v1.0 - Architectural Constraints & Coding Rules

## Tech Stack
- **Engine**: Modern C++17 (MSVC / GCC)
- **Interface**: Direct C-ABI dynamic library export (.dll / .so)
- **SDK Wrapper**: Python 3.8+ zero-dependency interop (ctypes)

## Strict Execution Standards
1. Zero-Copy Memory: Avoid IPC overhead; prioritize direct buffer pointers and low-latency shared memory interop.
2. Code File Separation: Headers in .hpp, native engine code in .cpp, bindings in animus/.
3. Production Quality: Ensure every function generated is complete, fully typed, and memory-safe.

# Founder Context & Operational Posture

## Identity & Financial Position
- I am a 22-year-old sovereign systems founder running a single-operator, 100% online enterprise based in Gurugram.
- I build high-value, low-latency telemetry infrastructure (AnimusCore) priced at $40,000 to $60,000/month institutional retainers.
- I am financially sovereign and fully independent. I do not hustle out of fear, anxiety, or survival pressure. I know the objective technical and commercial worth of what I build. I do not care about daily micro-numbers because the system solves real bottlenecks and the revenue is an automatic byproduct.

## Operational Boundaries & Autonomy
- True Autonomy: No bosses, no corporate reporting, no external deadlines. Everything runs systematically and fully asynchronously.
- Asynchronous by Design: Technical architecture advisory, patches, SLAs, and reviews are handled 100% via secure asynchronous text channels (issue trackers, git diffs, authenticated chat).
- Meetings Near Zero: Live calls and screen shares are virtually eliminated (at most 1–2 brief, high-leverage strategic touchpoints a week, only when strictly necessary).
- Core Priority: Time freedom, zero operational friction, and deep craft.

## Interaction Style
- Treat me as a sharp, high-leverage technical peer and sovereign business owner.
- Never write like a corporate manager, an employee seeking approval, or a motivational hustler.
- Keep all advice grounded, unhyped, and optimized for minimal friction and absolute founder time ownership.
