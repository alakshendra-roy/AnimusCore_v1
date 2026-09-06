# Animus Evaluation Kit -- Backpressure (zero-loss) benchmark launcher.
#
# Same underlying stripped harness_benchmark.exe as animus_bench_saturation.ps1,
# run in --mode backpressure instead. Unlike overwrite mode, backpressure mode
# blocks (bounded-retry) once the ring fills, so it needs a consumer actually
# draining the ring concurrently to complete -- on Windows, the named
# shared-memory segment does not reliably outlive the producer process the
# way a POSIX /dev/shm node does, so producer and consumer must be started
# together, not sequentially (see docs/EVALUATION_KIT.md §2.3). This script
# starts the producer in the background, runs the bundled Python consumer
# (python/consumer.py) in the foreground, then waits on the producer and
# prints its own report -- reproducing the "10,000,000 / 10,000,000
# delivered, 0 drops" pairing documented in docs/EVALUATION_KIT.md §1.2.
#
# Usage: .\animus_bench_backpressure.ps1

$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$exe = Join-Path $here "harness_benchmark.exe"
$pythonReader = Join-Path (Split-Path -Parent $here) "python\consumer.py"
$segmentName = "animus_eval_backpressure"

$python = Get-Command python -ErrorAction SilentlyContinue
if (-not $python) { $python = Get-Command py -ErrorAction SilentlyContinue }
if (-not $python) {
    Write-Error "No 'python' or 'py' found on PATH -- required to run the consumer half of this benchmark."
    exit 1
}

Write-Host "Starting producer (backpressure mode) in the background..."
$producer = Start-Process -FilePath $exe -ArgumentList @(
    "--name", $segmentName,
    "--events", "10000000",
    "--capacity", "1048576",
    "--mode", "backpressure",
    "--unlink-when-done"
) -NoNewWindow -PassThru

Start-Sleep -Milliseconds 500  # let the producer create the segment before the consumer attaches

Write-Host "Starting consumer (python/consumer.py) in the foreground..."
& $python.Source $pythonReader --name $segmentName --events 10000000 --idle-timeout-s 5.0

Write-Host "Waiting for producer to finish..."
$producer.WaitForExit()
$producer.Refresh()
Write-Host "Producer exit code: $($producer.ExitCode)"
