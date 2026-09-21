# animus_sandbox -- minimal single-command build.
#
#   make              builds ./benchmark_harness and ./soak_harness
#   make run          builds (if needed) and runs benchmark_harness
#   make soak         builds (if needed) and runs soak_harness for a
#                      10,000,000-event soak (see run_benchmark.sh for the
#                      full checked/pinned/summarized version of this)
#   make clean        removes both built binaries
#
# CXX/CXXFLAGS can be overridden on the command line, e.g.:
#   make CXX=clang++
CXX      ?= g++
CXXFLAGS := -O3 -march=native -std=c++17 -Wall -Wextra -Wpedantic -pthread
LDFLAGS  := -pthread

.PHONY: all run soak clean

all: benchmark_harness soak_harness

benchmark_harness: benchmark_harness.cpp ring_buffer.hpp sandbox_event.hpp tsc_clock.hpp
	$(CXX) $(CXXFLAGS) benchmark_harness.cpp -o benchmark_harness $(LDFLAGS)

soak_harness: soak_harness.cpp ring_buffer.hpp sandbox_event.hpp tsc_clock.hpp
	$(CXX) $(CXXFLAGS) soak_harness.cpp -o soak_harness $(LDFLAGS)

run: benchmark_harness
	./benchmark_harness

soak: soak_harness
	./soak_harness 60 2 8 8 10000000

clean:
	rm -f benchmark_harness benchmark_harness.exe soak_harness soak_harness.exe
