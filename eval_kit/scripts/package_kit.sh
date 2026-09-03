#!/usr/bin/env bash
# Animus Evaluation Kit -- packaging automation.
#
# Assembles the client-facing tarball a systems engineer at a prospective
# desk downloads and runs: a standalone -O3 producer binary, both Python
# wheels it needs (built once here, so the evaluation machine never needs
# a C++ toolchain, cmake, or network access), the verification script, a
# one-command demo runner, and a build manifest. This script itself is a
# packager/release-side tool -- it assumes a full Linux build environment
# (a C++ compiler, cmake, python3 + pip with network access for the
# nanobind/scikit-build-core build dependencies) is available here; the
# whole point of shipping pre-built wheels is that the *evaluator's*
# machine needs none of that. Run from anywhere; every path below is
# resolved relative to this script's own location, not the caller's cwd.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EVAL_KIT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
REPO_ROOT="$(cd "$EVAL_KIT_DIR/.." && pwd)"

BUNDLE_NAME="animus-eval-kit-linux-x86_64"
DIST_DIR="$EVAL_KIT_DIR/dist"
STAGE_DIR="$DIST_DIR/$BUNDLE_NAME"
TARBALL="$DIST_DIR/${BUNDLE_NAME}.tar.gz"

echo "=== Animus Evaluation Kit -- Packaging ==="
echo "Repo root:   $REPO_ROOT"
echo "Staging to:  $STAGE_DIR"
echo

rm -rf "$STAGE_DIR" "$TARBALL"
mkdir -p "$STAGE_DIR/bin" "$STAGE_DIR/wheels" "$STAGE_DIR/scripts"

# --- 1. Compiler + arch-level selection ---------------------------------
# x86-64-v3 (AVX2/BMI2/FMA -- roughly Haswell/2013 and newer) rather than
# -march=native: this binary is meant to run on a machine that is NOT the
# one building it. -march=native bakes in whatever instruction set this
# specific packaging host's CPU happens to support and can SIGILL on a
# different (even newer, differently-configured) target CPU -- exactly
# the kind of "works on my machine" bug an evaluation kit must not ship
# with. v3 is a deliberate, conservative, widely-supported floor for a
# redistributed binary; --native remains available as an explicit
# fallback only when the compiler itself doesn't recognize -march=x86-64-v3
# (older GCC than ~11), not as a default.
CXX="${CXX:-}"
if [[ -z "$CXX" ]]; then
    if command -v clang++ >/dev/null 2>&1; then
        CXX=clang++
    elif command -v g++ >/dev/null 2>&1; then
        CXX=g++
    else
        echo "error: no C++ compiler found (need clang++ or g++ on PATH, or set \$CXX)" >&2
        exit 1
    fi
fi
echo "Compiler: $CXX ($("$CXX" --version | head -1))"

ARCH_CHECK_DIR="$(mktemp -d)"
trap 'rm -rf "$ARCH_CHECK_DIR"' EXIT
echo 'int main(){return 0;}' > "$ARCH_CHECK_DIR/probe.cpp"
if "$CXX" -march=x86-64-v3 -O3 "$ARCH_CHECK_DIR/probe.cpp" -o "$ARCH_CHECK_DIR/probe" 2>/dev/null; then
    ARCH_FLAG="-march=x86-64-v3"
else
    ARCH_FLAG="-march=native"
    echo "warning: $CXX does not recognize -march=x86-64-v3 -- falling back to -march=native." >&2
    echo "         The resulting binary is tied to THIS machine's exact CPU and may SIGILL" >&2
    echo "         on a different one; rebuild on (or matching) your actual distribution" >&2
    echo "         target if this kit will run on different hardware than this build host." >&2
fi
echo "Using: -O3 $ARCH_FLAG -std=c++17"
echo

# --- 2. Build the standalone producer binary ----------------------------
echo "--- Building bin/harness_benchmark ---"
"$CXX" -std=c++17 -O3 "$ARCH_FLAG" -Wall -Wextra -Wpedantic \
    -I"$REPO_ROOT/include" \
    "$REPO_ROOT/benchmarks/harness_benchmark.cpp" \
    -o "$STAGE_DIR/bin/harness_benchmark" \
    -lpthread
chmod +x "$STAGE_DIR/bin/harness_benchmark"
echo "-> $STAGE_DIR/bin/harness_benchmark"
echo

# --- 3. Build the two Python wheels the kit ships -----------------------
# animus_engine_sdk (root pyproject.toml): pure-Python, zero-dependency
# SDK. animus_native_stream (bindings/pyproject.toml): the compiled
# nanobind extension, built via scikit-build-core -- requires cmake and a
# C++ compiler on THIS machine only; the resulting wheel is a prebuilt
# binary the evaluation machine just installs.
if ! command -v cmake >/dev/null 2>&1; then
    echo "error: cmake not found on PATH -- required to build the nanobind extension wheel." >&2
    exit 1
fi
if ! command -v python3 >/dev/null 2>&1; then
    echo "error: python3 not found on PATH." >&2
    exit 1
fi

echo "--- Building wheels ---"
python3 -m pip wheel "$REPO_ROOT" -w "$STAGE_DIR/wheels" --no-deps
python3 -m pip wheel "$REPO_ROOT/bindings" -w "$STAGE_DIR/wheels" --no-deps
echo
ls -la "$STAGE_DIR/wheels"
echo

# --- 4. Assemble the rest of the bundle ---------------------------------
cp "$EVAL_KIT_DIR/README.md" "$STAGE_DIR/README.md"
cp "$EVAL_KIT_DIR/scripts/verify_stream.py" "$STAGE_DIR/scripts/verify_stream.py"
cp "$EVAL_KIT_DIR/scripts/run_demo.sh" "$STAGE_DIR/run_demo.sh"
chmod +x "$STAGE_DIR/run_demo.sh"

PY_VERSION="$(python3 -c 'import sys; print("%d.%d.%d" % sys.version_info[:3])')"
GIT_COMMIT="$(cd "$REPO_ROOT" && git rev-parse --short HEAD 2>/dev/null || echo unknown)"
BUILD_DATE_UTC="$(date -u +"%Y-%m-%dT%H:%M:%SZ")"

cat > "$STAGE_DIR/MANIFEST.txt" <<EOF
Animus Evaluation Kit -- Build Manifest
========================================
Built:                 $BUILD_DATE_UTC
Source commit:         $GIT_COMMIT
Compiler:              $("$CXX" --version | head -1)
Producer build flags:  -O3 $ARCH_FLAG -std=c++17
Python (wheel build):  $PY_VERSION
Wheels:
$(cd "$STAGE_DIR/wheels" && ls -1)

If wheels/animus_native_stream-*.whl fails to install on the evaluation
machine, its compiled-extension platform/ABI tag must match that
machine's python3 --version exactly. Rebuild this kit with that Python
on PATH (see README.md's Troubleshooting section), or install a matching
interpreter on the evaluation machine.
EOF

echo "--- Bundle contents ---"
find "$STAGE_DIR" -type f | sed "s#$STAGE_DIR/#  #"
echo

# --- 5. Compress -----------------------------------------------------
tar -C "$DIST_DIR" -czf "$TARBALL" "$BUNDLE_NAME"
SHA256="$(sha256sum "$TARBALL" | awk '{print $1}')"

echo "=== Packaging complete ==="
echo "Tarball:   $TARBALL"
echo "Size:      $(du -h "$TARBALL" | cut -f1)"
echo "SHA-256:   $SHA256"
echo
echo "Smoke-test before sending it out:"
echo "  tar xzf $(basename "$TARBALL") && cd $BUNDLE_NAME && ./run_demo.sh"
