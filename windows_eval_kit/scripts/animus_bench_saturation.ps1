# Animus Evaluation Kit -- Saturation (decoupled overwrite) benchmark launcher.
#
# Thin wrapper around the stripped harness_benchmark.exe in this same
# directory -- there is one real producer binary in this kit, and mode is
# selected at runtime via --mode, not by shipping two separately-compiled
# executables. This script pins the exact arguments that produced the
# "16.127 M events/sec" saturation figure in docs/EVALUATION_KIT.md §1.1:
# the producer never waits on a consumer, so this is fully self-contained
# and completes on its own with no second process required.
#
# Usage: .\animus_bench_saturation.ps1 [extra harness_benchmark.exe args]

$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$exe = Join-Path $here "harness_benchmark.exe"

& $exe --events 10000000 --capacity 1048576 --mode overwrite @args
