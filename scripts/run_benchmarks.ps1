<#
.SYNOPSIS
    Animus Engine -- deterministic one-command benchmark reproduction harness
    (native Windows / PowerShell).

.DESCRIPTION
    Builds benchmarks/harness_benchmark.cpp in Release configuration via the
    root CMakeLists.txt's `harness_benchmark` target (the same binary
    BENCHMARKS.md's Phase 29 verification exercises -- a real
    animus::sys::ipc::ShmRing<ExecutionEvent> producer, see
    include/animus/shm_ipc.hpp), runs the standard throughput/latency pass,
    and samples that ring's telemetry header while the producer is still
    writing to it -- once via scripts/animus_stat.py's live-dashboard row
    (--once) and once via its OpenMetrics/Prometheus exposition
    (--prometheus) -- before tearing the segment down. See ARCHITECTURE.md
    sections 1, 2, and 4 for what these numbers mean.

    Sampling happens WHILE the producer runs, not after it exits: unlike
    POSIX (where a /dev/shm node outlives the process until explicitly
    unlinked), a Windows named file mapping is destroyed the instant the
    creating process's last handle closes (ARCHITECTURE.md section 1.1) --
    there is no post-exit segment to read here. The producer is therefore
    started as a background process and scripts/run_benchmark_telemetry.py
    (a single Python process shared with run_benchmarks.sh) polls for its
    segment and then holds its own handle open for the whole sampling
    window, so the segment survives regardless of exactly when the
    producer itself finishes -- see that script's own header comment for
    why a poll-then-let-go approach is not enough on Windows.

.PARAMETER Events
    Synthetic events to push through the ring. Default: 10,000,000
    (matches BENCHMARKS.md's Phase 29 methodology).

.PARAMETER Capacity
    Ring capacity in slots, rounded up to a power of two by ShmRing<T>
    itself. Default: 1,048,576.

.PARAMETER BuildDir
    CMake build directory to configure/build into. Default:
    <repo root>\build\bench-release.

.EXAMPLE
    powershell -File scripts/run_benchmarks.ps1
#>
[CmdletBinding()]
param(
    [int]$Events = 10000000,
    [int]$Capacity = 1048576,
    [string]$BuildDir = ""
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot = (Resolve-Path (Join-Path $ScriptDir "..")).Path
if (-not $BuildDir) { $BuildDir = Join-Path $RepoRoot "build\bench-release" }
$RingName = "animus_bench_$PID"

$OsArch = "x86"
if ([System.Environment]::Is64BitOperatingSystem) { $OsArch = "x64" }

Write-Host "=============================================================="
Write-Host " Animus Engine -- Benchmark Reproduction Harness"
Write-Host "=============================================================="
Write-Host "Platform:    Windows $([System.Environment]::OSVersion.Version) ($OsArch)"
Write-Host "Repo root:   $RepoRoot"
Write-Host "Build dir:   $BuildDir"
Write-Host "Events:      $Events"
Write-Host "Capacity:    $Capacity slots"
Write-Host "Ring name:   $RingName"
Write-Host ""

if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    Write-Error "cmake not found on PATH -- required to build harness_benchmark"
    exit 1
}

# --- 1. Verify/trigger the release build ----------------------------------
# Always re-invoke configure+build: both are incremental (Ninja/MSBuild
# each skip recompiling anything unchanged), so this is the "verify or
# trigger" step the underlying build system already implements correctly --
# no hand-rolled staleness check needed on top of it.
$GeneratorArgs = @()
if (Get-Command ninja -ErrorAction SilentlyContinue) {
    $GeneratorArgs = @("-G", "Ninja")
}

Write-Host "--- Configuring (CMake, Release) ---"
& cmake -S "$RepoRoot" -B "$BuildDir" -DCMAKE_BUILD_TYPE=Release @GeneratorArgs
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed (exit $LASTEXITCODE)" }

Write-Host ""
Write-Host "--- Building harness_benchmark (Release) ---"
& cmake --build "$BuildDir" --target harness_benchmark --config Release
if ($LASTEXITCODE -ne 0) { throw "cmake build failed (exit $LASTEXITCODE)" }

$Bin = Join-Path $BuildDir "harness_benchmark.exe"
if (-not (Test-Path $Bin)) { $Bin = Join-Path $BuildDir "Release\harness_benchmark.exe" }
if (-not (Test-Path $Bin)) {
    Write-Error "harness_benchmark.exe not found after build (looked in $BuildDir and $BuildDir\Release)"
    exit 1
}
Write-Host "-> $Bin"
Write-Host ""

# --- 2. Launch the standard throughput/latency pass (background) --------
Write-Host "=============================================================="
Write-Host " Throughput / Latency Pass (decoupled overwrite, $Events events)"
Write-Host "=============================================================="
$JsonOut = Join-Path $BuildDir "harness_benchmark_result.json"
$LogFile = Join-Path $BuildDir "harness_benchmark_stdout.log"
$ErrFile = Join-Path $BuildDir "harness_benchmark_stderr.log"

$proc = Start-Process -FilePath $Bin -ArgumentList @(
    "--name", $RingName,
    "--events", $Events,
    "--capacity", $Capacity,
    "--mode", "overwrite",
    "--json", $JsonOut
) -NoNewWindow -PassThru -RedirectStandardOutput $LogFile -RedirectStandardError $ErrFile

# --- 3. Poll for the live segment, sample telemetry, and unlink it -------
# Delegates to run_benchmark_telemetry.py (a single, long-lived Python
# process shared with run_benchmarks.sh) rather than polling here and then
# spawning a separate animus_stat.py process afterward: on Windows a named
# file mapping is destroyed the instant its last handle closes, so a gap
# between "the poll process closed its handle" and "the next process opens
# one" is exactly where a fast run (well under a second at the default
# event count) can lose the segment before it's ever sampled. See that
# script's own header comment for the full explanation.
$Py = $null
foreach ($cand in @("python", "python3", "py")) {
    if (Get-Command $cand -ErrorAction SilentlyContinue) { $Py = $cand; break }
}

if ($Py) {
    Write-Host ""
    Write-Host "=============================================================="
    Write-Host " Telemetry Snapshot (scripts/animus_stat.py, ring='$RingName')"
    Write-Host "=============================================================="
    & $Py (Join-Path $ScriptDir "run_benchmark_telemetry.py") $RingName (Join-Path $ScriptDir "animus_stat.py")
    Write-Host ""
} else {
    Write-Warning "no python/python3/py found on PATH -- skipping telemetry snapshot (scripts/animus_stat.py)"
}

# --- 4. Wait for the producer to finish, then show its own report --------
Wait-Process -Id $proc.Id
Write-Host "=============================================================="
Write-Host " Producer Output"
Write-Host "=============================================================="
Get-Content $LogFile
$stderrContent = Get-Content $ErrFile -ErrorAction SilentlyContinue
if ($stderrContent) {
    Write-Host ""
    Write-Host "--- stderr ---"
    $stderrContent | Write-Host
}
Write-Host ""

Write-Host "=============================================================="
Write-Host " Done. Full JSON report: $JsonOut"
Write-Host "=============================================================="
