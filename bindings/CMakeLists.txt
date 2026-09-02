# Animus Engine -- nanobind Python extension (bindings/animus_py.cpp).
#
# A standalone CMake project deliberately kept separate from the root
# CMakeLists.txt (which builds animus_native, the C-ABI shim for the
# existing zero-dependency ctypes SDK): this extension pulls in nanobind
# and, at the Python packaging layer, installs *only* the compiled
# _animus_native module into the existing animus/ package directory --
# not a change to animus-engine-sdk's own build, and not a second copy of
# animus/*.py. animus.consumer (animus/consumer.py, shipped by the main
# package) simply raises a clear ImportError until this extension has
# actually been built, rather than requiring a second, differently-named
# package.
#
# Verified combination: `pip install .` (a normal, non-editable install
# of the main package) followed by `pip install ./bindings` copies both
# sets of files into the *same* physical site-packages/animus/ directory
# and works exactly as intended -- confirmed by actually installing both
# and importing animus.consumer alongside animus.bindings, not assumed.
#
# NOT verified to work: `pip install -e .` (editable) followed by
# `pip install ./bindings`. Empirically, that combination left `animus`
# resolving as an empty namespace package pointing at site-packages'
# physical (bindings-only) animus/ directory, silently losing
# animus.bindings/.core/etc. entirely -- setuptools' PEP 660 editable
# finder did not take priority over the real directory the bindings
# wheel had already placed in site-packages. For local development, use
# the direct-CMake loop below instead (copy the built extension straight
# into the repo's own animus/ directory, next to consumer.py) rather
# than layering a separate `pip install ./bindings` on top of an
# editable main-package install.
#
# Build directly with CMake for local iteration (see the bottom of this
# file) or via `pip install ./bindings`, which drives this through
# scikit-build-core
# (see pyproject.toml alongside this file).
cmake_minimum_required(VERSION 3.15)
project(animus_native_ext LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Python 3.8 COMPONENTS Interpreter Development.Module REQUIRED)

# Locate nanobind's CMake package the same way its own documentation
# recommends: ask the interpreter that's actually building this (so a venv
# with nanobind installed is found correctly, rather than guessing a path).
execute_process(
    COMMAND "${Python_EXECUTABLE}" -m nanobind --cmake_dir
    OUTPUT_STRIP_TRAILING_WHITESPACE
    OUTPUT_VARIABLE NB_DIR
    RESULT_VARIABLE NB_DIR_RESULT
)
if(NOT NB_DIR_RESULT EQUAL 0)
    message(FATAL_ERROR
        "Could not locate nanobind's CMake config via '${Python_EXECUTABLE} -m nanobind --cmake_dir'. "
        "Is nanobind installed in that interpreter's environment? (pip install nanobind)")
endif()
list(APPEND CMAKE_PREFIX_PATH "${NB_DIR}")
find_package(nanobind CONFIG REQUIRED)

nanobind_add_module(_animus_native
    NB_STATIC
    animus_py.cpp
)

# animus.hpp is header-only (Phase 7 of AnimusCore_v1/BENCHMARKS.md) --
# this extension #includes it directly and drives animus::Engine's
# primitives in-process, no DLL, no C-ABI boundary, no ctypes.
target_include_directories(_animus_native PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../AnimusCore_v1
)

if(WIN32)
    target_compile_definitions(_animus_native PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

find_package(Threads REQUIRED)
target_link_libraries(_animus_native PRIVATE Threads::Threads)

# Installed as animus/_animus_native(.pyd|.so) -- animus/consumer.py
# imports it as a sibling module (`from . import _animus_native`).
install(TARGETS _animus_native LIBRARY DESTINATION animus)

# ---------------------------------------------------------------------
# Local build without scikit-build-core, for fast iteration:
#
#   pip install nanobind
#   cmake -S bindings -B bindings/build -DPython_EXECUTABLE=<path to your python>
#   cmake --build bindings/build --config Release
#
# The resulting _animus_native(.pyd|.so) lands in bindings/build/ (or
# bindings/build/Release/ on multi-config generators) -- copy it into the
# repo's animus/ directory (next to consumer.py) to import it directly
# against an editable install (`pip install -e .` for the main package),
# or add that build directory to PYTHONPATH for a quick local test. For a
# proper installable wheel that stages just the compiled extension into
# an already-installed animus/ package, use `pip install ./bindings`
# instead -- see pyproject.toml.
# ---------------------------------------------------------------------
