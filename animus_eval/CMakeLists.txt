# Animus Evaluation Kit -- standalone build.
#
# Two required targets (built by default, no Python needed):
#   libanimus     -- shared library, src/engine.cpp + include/animus/engine.hpp
#   replay_bench  -- executable, src/replay_bench.cpp, links libanimus
#
# One optional target (off by default -- see ANIMUS_EVAL_BUILD_PYTHON below):
#   animus_eval_native -- nanobind Python extension, python/animus_py.cpp
#
# Target: Linux x86_64, GCC 12+ / Clang 15+. Builds on other platforms for
# development convenience (see README.md), but Linux is the only supported
# measurement target for reported replay_bench numbers -- -march=native and
# the affinity/clock calls in engine.cpp are meaningful only there.
#
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build -j
#
cmake_minimum_required(VERSION 3.20)
project(AnimusEvalKit LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(ANIMUS_EVAL_BUILD_PYTHON "Build the nanobind Python extension (requires Python 3.8+ and nanobind installed in it)" OFF)

include(CheckIPOSupported)
check_ipo_supported(RESULT ipo_supported OUTPUT ipo_error_message)

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# libanimus -- shared library
# ---------------------------------------------------------------------------
add_library(libanimus SHARED src/engine.cpp)
set_target_properties(libanimus PROPERTIES
    OUTPUT_NAME animus
    POSITION_INDEPENDENT_CODE ON
)
target_include_directories(libanimus PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(libanimus PRIVATE Threads::Threads)
# See engine.hpp's ANIMUS_EVAL_API comment: only libanimus's own translation
# units build with the dllexport side of that macro; every consumer
# (replay_bench, the Python extension, a client) sees dllimport instead.
target_compile_definitions(libanimus PRIVATE ANIMUS_EVAL_BUILDING_DLL)
if(ipo_supported)
    set_target_properties(libanimus PROPERTIES INTERPROCEDURAL_OPTIMIZATION ON)
endif()

# ---------------------------------------------------------------------------
# replay_bench -- standalone evaluation harness
# ---------------------------------------------------------------------------
add_executable(replay_bench src/replay_bench.cpp)
target_link_libraries(replay_bench PRIVATE libanimus Threads::Threads)
if(ipo_supported)
    set_target_properties(replay_bench PROPERTIES INTERPROCEDURAL_OPTIMIZATION ON)
endif()

# Strict production flags apply only to the two targets above -- the hot
# path this kit exists to measure. They are deliberately NOT applied to the
# nanobind extension target further down: nanobind's own generated dispatch
# code requires C++ exceptions (it translates them to Python exceptions
# across the binding boundary) and, on most toolchains, RTTI, so
# -fno-exceptions/-fno-rtti would fail to compile or silently break error
# propagation there, not just warn.
if(MSVC)
    # -march=native / -fno-exceptions have no direct MSVC equivalent; /O2 /GL
    # /DNDEBUG plus CMAKE_INTERPROCEDURAL_OPTIMIZATION's /LTCG above is the
    # closest MSVC analog, included so this kit still builds on a Windows
    # development machine (see README.md's supported-platform note).
    foreach(target libanimus replay_bench)
        # /wd4324: alignas(64) padding on MarketDataEntry/Impl's ring cursors
        # is intentional cache-line isolation, not a layout bug -- the same
        # suppression this repo's other eval kits apply to identical
        # alignas(64) types (see animus-eval-kit/CMakeLists.txt).
        #
        # /wd4251: AnimusEngine's private std::unique_ptr<Impl> pimpl member
        # "needs dll-interface" per MSVC -- textbook-benign for a pimpl: no
        # client code ever names or touches Impl or the unique_ptr directly,
        # only through AnimusEngine's own exported member functions, so the
        # member's own dll-interface is irrelevant to callers.
        target_compile_options(${target} PRIVATE /O2 /GL /DNDEBUG /W4 /WX /wd4324 /wd4251)
    endforeach()
else()
    foreach(target libanimus replay_bench)
        target_compile_options(${target} PRIVATE
            -O3
            -march=native
            -fno-omit-frame-pointer
            -fno-exceptions
            -fno-rtti
            -pthread
            -DNDEBUG
            -Wall
            -Wextra
            -Werror
        )
    endforeach()
endif()

# ---------------------------------------------------------------------------
# animus_eval_native -- optional nanobind Python extension
# ---------------------------------------------------------------------------
if(ANIMUS_EVAL_BUILD_PYTHON)
    find_package(Python 3.8 COMPONENTS Interpreter Development.Module REQUIRED)

    execute_process(
        COMMAND "${Python_EXECUTABLE}" -m nanobind --cmake_dir
        OUTPUT_STRIP_TRAILING_WHITESPACE
        OUTPUT_VARIABLE NB_DIR
        RESULT_VARIABLE NB_DIR_RESULT
    )
    if(NOT NB_DIR_RESULT EQUAL 0)
        message(FATAL_ERROR
            "ANIMUS_EVAL_BUILD_PYTHON is ON but nanobind's CMake config could not be "
            "located via '${Python_EXECUTABLE} -m nanobind --cmake_dir'. "
            "Install it in that interpreter's environment: pip install nanobind")
    endif()
    list(APPEND CMAKE_PREFIX_PATH "${NB_DIR}")
    find_package(nanobind CONFIG REQUIRED)

    nanobind_add_module(animus_eval_native NB_STATIC python/animus_py.cpp)
    target_link_libraries(animus_eval_native PRIVATE libanimus)
    target_include_directories(animus_eval_native PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    if(WIN32)
        target_compile_definitions(animus_eval_native PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    endif()
endif()
