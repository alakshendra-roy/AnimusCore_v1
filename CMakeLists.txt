# Portable, cross-platform companion to AnimusCore_v1.vcxproj (MSVC-only).
#
# Builds AnimusNative -- the same C-ABI shim as AnimusCore_v1.dll
# (AnimusCore_v1/animus_engine.cpp, re-exposing animus.hpp's header-only
# animus::Engine across extern "C") -- but through CMake so it also produces
# libanimus_native.so / libanimus_native.dylib on Linux/macOS, which the
# vcxproj cannot. This target is additive: it does not replace or touch the
# existing AnimusCore_v1.dll build, and animus/bindings.py's native library
# search checks both names.
cmake_minimum_required(VERSION 3.15)
project(AnimusNative LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_library(animus_native SHARED
    ${CMAKE_CURRENT_SOURCE_DIR}/AnimusCore_v1/animus_engine.cpp
)

target_include_directories(animus_native PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/AnimusCore_v1
)

target_compile_definitions(animus_native PRIVATE ANIMUS_EXPORTS)

# Needed for pthread_setaffinity_np (animus_pin_current_thread_to_core's
# Linux implementation, animus_engine.cpp) -- std::thread's own use in
# EngineImpl already implies pthread, but that's an implicit dependency of
# whichever glibc/libstdc++ this builds against, not a guarantee; linking
# it explicitly via the portable CMake mechanism (works whether pthread is
# a separate library or merged into libc) doesn't rely on that holding.
find_package(Threads REQUIRED)
target_link_libraries(animus_native PRIVATE Threads::Threads)

# Needed for shm_open/shm_unlink (SharedMemorySegment's POSIX
# implementation, animus.hpp) on glibc older than 2.34, which shipped them
# in a separate librt rather than libc itself; harmless to link even where
# it's already merged, since glibc still ships librt.so as a compatibility
# stub. Not needed (and not present) on macOS, where shm_open is part of
# libSystem directly.
if(UNIX AND NOT APPLE)
    target_link_libraries(animus_native PRIVATE rt)
endif()

# Needed for animus_verify_license's Windows implementation (animus_engine.cpp):
# bcrypt (RSA signature verification + SHA-256), iphlpapi (MAC address
# enumeration), ws2_32 (winsock2.h/ws2tcpip.h, required by iphlpapi.h's own
# declarations). MSVC picks these up automatically via #pragma comment(lib,
# ...) in the source; MinGW/g++ does not honor that pragma, so this is the
# only thing that actually links them for a CMake+MinGW build on Windows.
if(WIN32)
    target_link_libraries(animus_native PRIVATE bcrypt iphlpapi ws2_32)
endif()

if(NOT MSVC)
    target_compile_options(animus_native PRIVATE -fvisibility=hidden)
    # Statically link the GCC/MinGW runtime (libstdc++/libgcc/winpthread) so
    # the produced binary is redistributable on its own -- without this, a
    # MinGW-built AnimusNative.dll only loads on a machine that also has
    # MSYS2/MinGW's runtime DLLs on PATH, which defeats the point of
    # shipping a standalone C-ABI shared library.
    target_link_options(animus_native PRIVATE -static-libgcc -static-libstdc++)
    if(WIN32)
        target_link_options(animus_native PRIVATE -static -lwinpthread)
    endif()
endif()

if(WIN32)
    # -> AnimusNative.dll. PREFIX must be cleared explicitly: MinGW/GCC
    # defaults to a "lib" prefix on shared libraries even on Windows, where
    # MSVC does not -- without this, a MinGW build produces
    # libAnimusNative.dll instead of AnimusNative.dll.
    set_target_properties(animus_native PROPERTIES OUTPUT_NAME "AnimusNative" PREFIX "")
else()
    # -> libanimus_native.so / libanimus_native.dylib
    set_target_properties(animus_native PROPERTIES OUTPUT_NAME "animus_native")
endif()

# Land the built binary directly under the top-level build directory
# (rather than a per-config subdirectory) so animus/bindings.py's fixed
# search paths (build/, build/Release/) find it without guessing a
# generator-specific layout.
set_target_properties(animus_native PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
foreach(config ${CMAKE_CONFIGURATION_TYPES})
    string(TOUPPER ${config} config_upper)
    set_target_properties(animus_native PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_${config_upper} ${CMAKE_BINARY_DIR}/${config}
        LIBRARY_OUTPUT_DIRECTORY_${config_upper} ${CMAKE_BINARY_DIR}/${config}
    )
endforeach()

# telemetry_benchmark -- standalone C++23 proof-point harness (benchmarks/).
# A per-target language standard (cxx_std_23) rather than the project-wide
# CMAKE_CXX_STANDARD above, so this doesn't force C++23 onto animus_native,
# which stays on the C++17 the exported C-ABI shim is built against.
add_executable(telemetry_benchmark
    ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/telemetry_benchmark.cpp
)
target_compile_features(telemetry_benchmark PRIVATE cxx_std_23)
find_package(Threads REQUIRED)
target_link_libraries(telemetry_benchmark PRIVATE Threads::Threads)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # libstdc++'s <print> ships its Windows/POSIX terminal-writing backend
    # (std::__open_terminal / std::__write_to_terminal) in the separate
    # "experimental" static library until the TS support stabilizes --
    # without this, linking fails with undefined references to those two
    # symbols on any GCC (including MinGW) new enough to have <print> at all.
    target_link_libraries(telemetry_benchmark PRIVATE stdc++exp)
endif()
set_target_properties(telemetry_benchmark PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

# harness_benchmark -- cross-process shared-memory producer harness
# (Milestone 4). C++17 (matching include/animus/shm_ipc.hpp's own
# standard, not telemetry_benchmark's C++23 opt-in above) so this builds
# with the same toolchains as animus_native itself. Unlike
# telemetry_benchmark, this is one half of a two-*process* demo -- see
# benchmarks/consumer.py for the other half, and benchmarks/build.sh for
# the POSIX ASan/UBSan build path this CMake target does not replace.
add_executable(harness_benchmark
    ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/harness_benchmark.cpp
)
target_include_directories(harness_benchmark PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_features(harness_benchmark PRIVATE cxx_std_17)
target_link_libraries(harness_benchmark PRIVATE Threads::Threads)
if(UNIX AND NOT APPLE)
    target_link_libraries(harness_benchmark PRIVATE rt)
endif()
set_target_properties(harness_benchmark PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

# test_dynamic_schema -- Milestone 1 regression test (Dynamic & User-Defined
# Wire Schemas). Same cross-process, C++17, include/-only dependency shape
# as harness_benchmark above; this binary is both the producer and the
# consumer side, selected by argv -- see tests/test_dynamic_schema.cpp's own
# header comment for how to run it as two separate OS processes.
add_executable(test_dynamic_schema
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_dynamic_schema.cpp
)
target_include_directories(test_dynamic_schema PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_features(test_dynamic_schema PRIVATE cxx_std_17)
target_link_libraries(test_dynamic_schema PRIVATE Threads::Threads)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_dynamic_schema PRIVATE rt)
endif()
set_target_properties(test_dynamic_schema PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
