# SPDX-License-Identifier: Apache-2.0
# AnimusCore ring-buffer verification suite -- x86-64 Linux (glibc).
#
#   make check     zero-allocation audit: 29 allocator entry points proven to trip the guard, then
#                  10,000,000 frames through the SPSC ring and 10,000,000 through the MPMC ring
#   make bench     invariant-TSC latency profile on one pinned core, with the profiler's self-checks
#   make pybridge  nanobind zero-copy bridge: build the module, run the validation suite
#                  (nanobind is needed to build and NumPy to run:  python3 -m pip install nanobind numpy)
#   make clean
#
# Compiler: clang++ if installed, otherwise g++; override with CXX=. Flags: -std=c++17 -O3 -march=native
# -Wall -Wextra -Werror -pedantic. Examples:
#   make check bench CXX=clang++-18        make bench CPU=3        make check ITERS=1000000
#   make pybridge PYTHON=.venv/bin/python
# Build products go under ./build; no target writes outside this directory. `check` and `bench` need x86-64
# Linux with glibc (they refuse elsewhere); only the Python bridge is portable. See README.md.

UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)

ifeq ($(origin CXX),default)
  CXX := $(shell command -v clang++ >/dev/null 2>&1 && echo clang++ || echo g++)
endif
PYTHON ?= python3
BUILD  ?= build
ITERS  ?= 10000000
CPU    ?=

CXXFLAGS := -std=c++17 -O3 -march=native -Wall -Wextra -Werror -pedantic -fsized-deallocation -pthread
INC      := -I include
HEADERS  := $(wildcard include/animus/*.hpp)
BRIDGE   := $(BUILD)/bridge

.DEFAULT_GOAL := help
.PHONY: help all check bench pybridge clean _linux_x86_64 _python_deps

help:
	@echo "make check     zero-allocation audit (29 entry points, then $(ITERS) frames per ring)"
	@echo "make bench     invariant-TSC latency profile, pinned (CPU=<n> to choose the core)"
	@echo "make pybridge  build and validate the nanobind zero-copy bridge (needs nanobind, NumPy, CMake)"
	@echo "make clean     remove ./$(BUILD)"
	@echo "compiler: $(CXX)   (override: make check CXX=g++-14)"

all: $(BUILD)/test_no_alloc $(BUILD)/bench_ring_tsc

$(BUILD):
	mkdir -p $(BUILD)

# -rdynamic: names the externally visible functions in a violation's stack trace.
$(BUILD)/test_no_alloc: src/test_no_alloc.cpp $(HEADERS) | $(BUILD)
	$(CXX) $(CXXFLAGS) -rdynamic $(INC) $< -o $@

$(BUILD)/bench_ring_tsc: src/bench_ring_tsc.cpp $(HEADERS) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(INC) $< -o $@

_linux_x86_64:
ifneq ($(UNAME_S)/$(UNAME_M),Linux/x86_64)
	$(error check and bench need x86-64 Linux with glibc, and this is $(UNAME_S)/$(UNAME_M). Only the Python bridge is portable: see README.md)
endif
	@:

check: _linux_x86_64 $(BUILD)/test_no_alloc
	$(BUILD)/test_no_alloc --iters $(ITERS)

bench: _linux_x86_64 $(BUILD)/bench_ring_tsc
	$(BUILD)/bench_ring_tsc $(if $(CPU),--cpu $(CPU))

_python_deps:
	@command -v cmake >/dev/null 2>&1 || { echo "pybridge needs cmake on PATH"; exit 1; }
	@$(PYTHON) -c "import nanobind, numpy" 2>/dev/null || { echo "pybridge needs nanobind (to build) and NumPy (to run) in '$(PYTHON)'. Try: $(PYTHON) -m pip install nanobind numpy, or set PYTHON= to an environment that has them"; exit 1; }

$(BRIDGE)/.built: CMakeLists.txt src/python_bridge.cpp $(HEADERS) | $(BUILD)
	cmake -S . -B $(BRIDGE) -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=$(CXX) -DPython_EXECUTABLE=$(abspath $(shell command -v $(PYTHON)))
	cmake --build $(BRIDGE) --parallel
	touch $@

pybridge: _python_deps $(BRIDGE)/.built
	ANIMUS_BRIDGE_DIR=$(BRIDGE) $(PYTHON) python/test_python_bridge.py

clean:
	rm -rf $(BUILD)
