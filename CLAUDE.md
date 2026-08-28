# Animus Core v1.0 - Architectural Constraints & Coding Rules

## Tech Stack
- **Engine**: Modern C++17 (MSVC / GCC)
- **Interface**: Direct C-ABI dynamic library export (.dll / .so)
- **SDK Wrapper**: Python 3.8+ zero-dependency interop (ctypes)

## Strict Execution Standards
1. Zero-Copy Memory: Avoid IPC overhead; prioritize direct buffer pointers and low-latency shared memory interop.
2. Code File Separation: Headers in .hpp, native engine code in .cpp, bindings in animus/.
3. Production Quality: Ensure every function generated is complete, fully typed, and memory-safe.
