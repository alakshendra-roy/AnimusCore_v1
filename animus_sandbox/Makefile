# animus_sandbox -- minimal single-command build.
#
#   make            builds ./benchmark_harness
#   make run        builds (if needed) and runs it
#   make clean      removes the built binary
#
# CXX/CXXFLAGS can be overridden on the command line, e.g.:
#   make CXX=clang++
CXX      ?= g++
CXXFLAGS := -O3 -march=native -std=c++17 -Wall -Wextra -Wpedantic -pthread
LDFLAGS  := -pthread

.PHONY: all run clean

all: benchmark_harness

benchmark_harness: benchmark_harness.cpp ring_buffer.hpp sandbox_event.hpp tsc_clock.hpp
	$(CXX) $(CXXFLAGS) benchmark_harness.cpp -o benchmark_harness $(LDFLAGS)

run: benchmark_harness
	./benchmark_harness

clean:
	rm -f benchmark_harness benchmark_harness.exe
