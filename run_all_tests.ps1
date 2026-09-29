Write-Host "==================================================" -ForegroundColor Cyan
Write-Host "     ANIMUSCORE AUTOMATED TEST & BENCHMARK CI     " -ForegroundColor Cyan
Write-Host "==================================================" -ForegroundColor Cyan

$py = ".\venv\Scripts\python.exe"

Write-Host "`n[1/2] Running MPMC High-Contention Burst Test..." -ForegroundColor Yellow
& $py tests\stress_test_mpmc.py
if ($LASTEXITCODE -ne 0) {
    Write-Host "[!] MPMC stress test failed." -ForegroundColor Red
    exit 1
}

Write-Host "`n[2/2] Running Nanobind Zero-Copy Latency & Jitter Harness..." -ForegroundColor Yellow
& $py tests\benchmark_nanobind_bridge.py
if ($LASTEXITCODE -ne 0) {
    Write-Host "[!] Nanobind benchmark failed." -ForegroundColor Red
    exit 1
}

Write-Host "`n==================================================" -ForegroundColor Green
Write-Host "  ALL TESTS PASSED: THROUGHPUT & LATENCIES VERIFIED" -ForegroundColor Green
Write-Host "==================================================" -ForegroundColor Green
