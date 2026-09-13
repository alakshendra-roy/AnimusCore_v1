cmake_minimum_required(VERSION 3.15)
project(animus_sandbox LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Threads REQUIRED)

# --- Core benchmark harness (always built) ------------------------------
add_executable(benchmark_harness benchmark_harness.cpp)
target_link_libraries(benchmark_harness PRIVATE Threads::Threads)

if(MSVC)
    # MSVC has no exact equivalent of -march=native; /O2 is its Release
    # default and is applied automatically via CMAKE_BUILD_TYPE above.
    # /wd4324 silences "structure was padded due to alignment specifier"
    # -- that padding is alignas(64)'s intended effect here, not a bug.
    target_compile_options(benchmark_harness PRIVATE /W4 /permissive- /wd4324)
else()
    target_compile_options(benchmark_harness PRIVATE -O3 -march=native -Wall -Wextra -Wpedantic)
endif()

# --- Optional: nanobind zero-copy Python bridge --------------------------
# Only attempted if a Python 3.8+ interpreter with dev headers AND an
# importable `nanobind` package are both found; benchmark_harness above
# always builds regardless of whether this optional step succeeds.
find_package(Python 3.8 COMPONENTS Interpreter Development.Module QUIET)

if(Python_FOUND)
    execute_process(
        COMMAND "${Python_EXECUTABLE}" -c "import nanobind; print(nanobind.cmake_dir())"
        OUTPUT_VARIABLE NB_DIR
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE NB_IMPORT_RESULT
    )
    if(NB_IMPORT_RESULT EQUAL 0 AND NB_DIR)
        list(APPEND CMAKE_PREFIX_PATH "${NB_DIR}")
        find_package(nanobind CONFIG QUIET)
        if(nanobind_FOUND)
            nanobind_add_module(animus_sandbox_bridge STABLE_ABI nanobind_bridge.cpp)
            target_link_libraries(animus_sandbox_bridge PRIVATE Threads::Threads)
            if(NOT MSVC)
                target_compile_options(animus_sandbox_bridge PRIVATE -O3 -march=native)
            endif()
            message(STATUS "animus_sandbox: building nanobind Python bridge module (animus_sandbox_bridge)")
        else()
            message(STATUS "animus_sandbox: nanobind CMake package not found via ${NB_DIR} -- skipping Python bridge module")
        endif()
    else()
        message(STATUS "animus_sandbox: nanobind not importable from ${Python_EXECUTABLE} -- "
                        "skipping Python bridge module (run: pip install nanobind)")
    endif()
else()
    message(STATUS "animus_sandbox: no Python 3.8+ with dev headers found -- skipping Python bridge module")
endif()
