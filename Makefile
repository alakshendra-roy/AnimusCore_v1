# Animus Core -- verification suite without CMake (Linux; other POSIX best-effort).
#
#   make             build the tools into build-make/
#   make probe       (Linux) page-fault / backing accounting for each shm create+map strategy
#   make test        quick check: regression test, static allocation audit, small harness + soak runs
#   make verify      the reference run: regression test, audit, eval_harness (10,000,000 timed
#                    iterations per primitive, stream + drained), soak_shm_mpmc (64,000,000 frames,
#                    8 producers / 8 consumers). Writes JSON next to the binaries.
#   make sanitize    AddressSanitizer + UndefinedBehaviorSanitizer builds; regression test, a
#                    10,000,000-iteration harness run and an 8P/8C soak under them
#   make tsan        ThreadSanitizer build of the regression test and a reduced soak
#   make clean
#
# Reference flags: -std=c++17 -O3 -march=native -pthread -Wall -Wextra -Werror (and -lrt on Linux).
# Override on the command line, e.g.  make CXX=clang++   or   make FRAMES=200000000
#
# Windows: use CMake (see CMakeLists.txt, -DANIMUS_STRICT_WARNINGS=ON -DANIMUS_NATIVE_ARCH=ON). MinGW
# needs -static for the two allocator-replacing harnesses; the CMake target adds it.

CXX      ?= g++
PYTHON   ?= python3
BUILD    ?= build-make
FRAMES   ?= 64000000
ITERS    ?= 10000000

UNAME_S  := $(shell uname -s)

CXXSTD   := -std=c++17
WARN     := -Wall -Wextra -Wpedantic -Werror
INC      := -I include
CXXFLAGS := $(CXXSTD) -O3 -march=native -DNDEBUG -pthread $(WARN) $(INC)
SANFLAGS := $(CXXSTD) -O1 -g -fno-omit-frame-pointer -fno-sanitize-recover=all -pthread $(WARN) $(INC)

ifeq ($(UNAME_S),Linux)
  LDLIBS := -lrt
endif

# Layer 2 of the zero-allocation proof: GNU ld redirects malloc-family references to the counting
# wrappers in tests/alloc_guard.hpp. Not available with Apple's ld64.
ifneq ($(UNAME_S),Darwin)
  WRAP_DEF := -DANIMUS_WRAP_MALLOC=1
  WRAP_LD  := -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free,--wrap=posix_memalign,--wrap=aligned_alloc
endif

HEADERS  := $(wildcard include/animus/*.hpp tests/*.hpp)
TOOLS    := $(BUILD)/test_shm_mpmc $(BUILD)/eval_harness $(BUILD)/soak_shm_mpmc $(BUILD)/hotpath_audit_probe.o
ifeq ($(UNAME_S),Linux)
  TOOLS  += $(BUILD)/posix_shm_fault_probe
endif

.PHONY: all test verify audit probe sanitize tsan clean
all: $(TOOLS)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/test_shm_mpmc: tests/test_shm_mpmc.cpp $(HEADERS) | $(BUILD)
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDLIBS)

$(BUILD)/eval_harness: tests/eval_harness.cpp $(HEADERS) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(WRAP_DEF) $< -o $@ $(WRAP_LD) $(LDLIBS)

$(BUILD)/soak_shm_mpmc: tests/soak_shm_mpmc.cpp $(HEADERS) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(WRAP_DEF) $< -o $@ $(WRAP_LD) $(LDLIBS)

$(BUILD)/hotpath_audit_probe.o: tests/hotpath_audit_probe.cpp $(HEADERS) | $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/posix_shm_fault_probe: tests/posix_shm_fault_probe.cpp $(HEADERS) | $(BUILD)
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDLIBS)

# Linux: page-fault and backing accounting for each way of creating/mapping a shm segment (kernel counters, no timing).
probe: $(BUILD)/posix_shm_fault_probe
	$(BUILD)/posix_shm_fault_probe

# Static layer of the zero-allocation proof: no allocator or throw path is reachable from any hot-path symbol.
audit: $(BUILD)/hotpath_audit_probe.o
	$(PYTHON) scripts/audit_hotpath_symbols.py --object $< --json $(BUILD)/hotpath_audit.json

test: $(TOOLS) audit
	$(BUILD)/test_shm_mpmc
	$(BUILD)/eval_harness --iters 200000 --warmup 20000 --floor-samples 100000 --mode both
	$(BUILD)/soak_shm_mpmc --frames 4000000

verify: $(TOOLS) audit
	$(BUILD)/test_shm_mpmc
	$(BUILD)/eval_harness --iters $(ITERS) --warmup 500000 --mode both --json $(BUILD)/eval_harness.json
	$(BUILD)/soak_shm_mpmc --frames $(FRAMES) --producers 8 --consumers 8 --label saturated --json $(BUILD)/soak_saturated.json
	$(BUILD)/soak_shm_mpmc --frames 20000000 --producers 8 --consumers 8 --rate 2000000 --label paced-2M --json $(BUILD)/soak_paced.json

ASAN := $(BUILD)-asan
sanitize: | $(BUILD)
	mkdir -p $(ASAN)
	$(CXX) $(SANFLAGS) -fsanitize=address,undefined tests/test_shm_mpmc.cpp -o $(ASAN)/test_shm_mpmc $(LDLIBS)
	$(CXX) $(SANFLAGS) -fsanitize=address,undefined tests/eval_harness.cpp -o $(ASAN)/eval_harness $(LDLIBS)
	$(CXX) $(SANFLAGS) -fsanitize=address,undefined tests/soak_shm_mpmc.cpp -o $(ASAN)/soak_shm_mpmc $(LDLIBS)
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 $(ASAN)/test_shm_mpmc
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 $(ASAN)/eval_harness --iters $(ITERS) --warmup 500000 --mode both
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 $(ASAN)/soak_shm_mpmc --frames $(FRAMES) --label asan-ubsan

TSAN := $(BUILD)-tsan
tsan: | $(BUILD)
	mkdir -p $(TSAN)
	$(CXX) $(SANFLAGS) -fsanitize=thread tests/test_shm_mpmc.cpp -o $(TSAN)/test_shm_mpmc $(LDLIBS)
	$(CXX) $(SANFLAGS) -fsanitize=thread tests/soak_shm_mpmc.cpp -o $(TSAN)/soak_shm_mpmc $(LDLIBS)
	TSAN_OPTIONS=halt_on_error=1 $(TSAN)/test_shm_mpmc --frames 100000
	TSAN_OPTIONS=halt_on_error=1 $(TSAN)/soak_shm_mpmc --frames 4000000 --label tsan

clean:
	rm -rf $(BUILD) $(BUILD)-asan $(BUILD)-tsan
