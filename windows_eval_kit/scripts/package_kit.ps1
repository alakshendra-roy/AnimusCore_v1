# Animus Windows Evaluation Kit -- packaging automation.
#
# Windows counterpart to eval_kit/scripts/package_kit.sh (which targets
# Linux and needs a Linux build environment). Assembles the client-facing
# tarball a Windows systems engineer downloads and runs: the stripped
# AnimusNative.dll + harness_benchmark.exe Release binaries, the public
# ShmRing<T> headers, the reference zero-copy Python consumer, the
# hardware-fingerprint licensing utility, two mode-specific benchmark
# launchers, and the docs the numbers they reproduce are sourced from.
#
# This script assumes a full Windows build environment is available here
# (CMake + a C++17 compiler configured into the ./build directory already,
# per the repo root's own `cmake -S . -B build` instructions) plus GNU
# binutils' `strip` and `tar`/`sha256sum` on PATH (both ship with Git for
# Windows / MSYS2, which this repo's own contributors already have). Run
# from anywhere; every path below is resolved relative to this script's
# own location, not the caller's cwd.
#
# Usage: powershell -ExecutionPolicy Bypass -File windows_eval_kit\scripts\package_kit.ps1

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$KitDir = Split-Path -Parent $ScriptDir
$RepoRoot = Split-Path -Parent $KitDir

$BundleName = "animus_eval_v1.0"
$DistDir = Join-Path $RepoRoot "dist"
$StageDir = Join-Path $DistDir $BundleName
$Tarball = Join-Path $DistDir "$BundleName.tar.gz"

Write-Host "=== Animus Windows Evaluation Kit -- Packaging ==="
Write-Host "Repo root:   $RepoRoot"
Write-Host "Staging to:  $StageDir"
Write-Host ""

foreach ($tool in @("strip", "tar", "sha256sum")) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        Write-Error "required tool '$tool' not found on PATH (ships with Git for Windows / MSYS2)"
        exit 1
    }
}

if (Test-Path $StageDir) { Remove-Item -Recurse -Force $StageDir }
if (Test-Path $Tarball) { Remove-Item -Force $Tarball }
New-Item -ItemType Directory -Force -Path @(
    "$StageDir\bin", "$StageDir\include\animus", "$StageDir\lib",
    "$StageDir\python", "$StageDir\scripts", "$StageDir\docs"
) | Out-Null

# --- 1. Build the Release binaries --------------------------------------
Write-Host "--- Building Release: animus_native, harness_benchmark ---"
$BuildDir = Join-Path $RepoRoot "build"
if (-not (Test-Path $BuildDir)) {
    cmake -S $RepoRoot -B $BuildDir
    if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }
}
cmake --build $BuildDir --config Release --target animus_native harness_benchmark
if ($LASTEXITCODE -ne 0) { throw "cmake build failed" }

$ReleaseDir = Join-Path $BuildDir "Release"
Copy-Item "$ReleaseDir\harness_benchmark.exe" "$StageDir\bin\harness_benchmark.exe"
Copy-Item "$ReleaseDir\AnimusNative.dll" "$StageDir\lib\AnimusNative.dll"
Copy-Item "$ReleaseDir\AnimusNative.lib" "$StageDir\lib\AnimusNative.lib"

# --- 2. Strip symbols ----------------------------------------------------
# Strip the staged copies, never the build/Release originals -- those stay
# intact for local debugging of this same build.
Write-Host "--- Stripping symbols (strip --strip-all) ---"
strip --strip-all "$StageDir\bin\harness_benchmark.exe"
strip --strip-all "$StageDir\lib\AnimusNative.dll"

# --- 3. Benchmark launchers, headers, python reader, fingerprint script -
Copy-Item "$ScriptDir\animus_bench_saturation.ps1" "$StageDir\bin\animus_bench_saturation.ps1"
Copy-Item "$ScriptDir\animus_bench_backpressure.ps1" "$StageDir\bin\animus_bench_backpressure.ps1"
Copy-Item "$RepoRoot\include\animus\*.hpp" "$StageDir\include\animus\"
Copy-Item "$RepoRoot\benchmarks\consumer.py" "$StageDir\python\consumer.py"
Copy-Item "$RepoRoot\Pilot_Kit\get_fingerprint.ps1" "$StageDir\scripts\get_fingerprint.ps1"
Copy-Item "$RepoRoot\BENCHMARK_DATASHEET.md" "$StageDir\docs\BENCHMARK_DATASHEET.md"
Copy-Item "$RepoRoot\docs\EVALUATION_KIT.md" "$StageDir\docs\EVALUATION_KIT.md"
Copy-Item "$KitDir\README.md" "$StageDir\README.md"

Write-Host "--- Bundle contents ---"
Get-ChildItem -Recurse -File $StageDir | ForEach-Object {
    Write-Host "  $($_.FullName.Substring($StageDir.Length + 1))"
}
Write-Host ""

# --- 4. Archive + checksum -----------------------------------------------
# Both tar and sha256sum are run against a relative filename from inside
# $DistDir, not the full Windows path -- GNU coreutils' checksum tools
# prepend a stray leading "\" to their output line when the given filename
# contains a backslash (their escape convention for filenames that need
# it), which a full Windows path always does.
Push-Location $DistDir
try {
    tar -czf "$BundleName.tar.gz" $BundleName
    if ($LASTEXITCODE -ne 0) { throw "tar failed" }
    $Sha256Line = & sha256sum "$BundleName.tar.gz"
} finally {
    Pop-Location
}

$Sha256 = $Sha256Line.Substring(0, 64)

Write-Host "=== Packaging complete ==="
Write-Host "Tarball:   $Tarball"
Write-Host "SHA-256:   $Sha256"
