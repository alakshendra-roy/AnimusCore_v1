param(
    [int]$BudgetMinutes = 68,
    [int]$RunDurationSeconds = 20,
    [int]$ProducerThreads = 8,
    [int]$PinBaseCore = 4,
    [int]$CooldownSeconds = 3,
    [string]$ArtifactDirOverride = "",
    [switch]$SkipCommit
)

$ErrorActionPreference = "Stop"
$repoRoot   = "C:\Users\Alaks\source\repos\AnimusCore_v1"
$artifactDir = if ($ArtifactDirOverride -ne "") { $ArtifactDirOverride } else { Join-Path $repoRoot "artifacts\client_kit_soak_results" }
$rawLogDir   = Join-Path $artifactDir "raw_logs"
$exePath     = Join-Path $repoRoot "poc_eval\poc_eval_harness.exe"
$csvPath     = Join-Path $artifactDir "soak_summary.csv"
$runnerLog   = Join-Path $artifactDir "soak_runner.log"
$reportPath  = Join-Path $artifactDir "CLIENT_EVALUATION_SOAK_REPORT.md"

New-Item -ItemType Directory -Force -Path $rawLogDir | Out-Null

function Log($msg) {
    $line = "[{0:yyyy-MM-dd HH:mm:ss}] {1}" -f (Get-Date), $msg
    Add-Content -Path $runnerLog -Value $line
}

$csvHeader = "run_index,timestamp_utc,duration_s,producer_threads,pin_base_core," +
    "cpu,logical_cores,tsc_ghz," +
    "spsc_events_pushed,spsc_sequence_intact," +
    "spsc_push_p50_ns,spsc_push_p90_ns,spsc_push_p99_ns,spsc_push_p999_ns,spsc_push_max_ns," +
    "spsc_tick_p50_ns,spsc_tick_p90_ns,spsc_tick_p99_ns,spsc_tick_p999_ns,spsc_tick_max_ns," +
    "mpmc_pushes_per_sec,mpmc_push_p50_ns,mpmc_push_p90_ns,mpmc_push_p99_ns,mpmc_push_p999_ns,mpmc_push_max_ns," +
    "heap_allocations_total,ingest_target_pass,tick_target_pass,overall_pass,exit_code"

if (-not (Test-Path $csvPath)) {
    Set-Content -Path $csvPath -Value $csvHeader
}

function Parse-Run([string]$text, [int]$exitCode) {
    function Grab($pattern) {
        $m = [regex]::Match($text, $pattern)
        if ($m.Success) { return $m.Groups[1].Value.Trim() }
        return ""
    }

    $cpu       = Grab "CPU\s*:\s*(.+)"
    $cores     = Grab "Logical cores\s*:\s*(\d+)"
    $tscGhz    = Grab "TSC frequency\s*:\s*([\d\.]+)\s*GHz"
    $spscPush  = Grab "events pushed\s*:\s*(\d+)"
    $seqIntact = Grab "sequence intact\s*:\s*(yes|NO)"
    $mpmcPps   = Grab "MPMC pushes/sec\s*:\s*(\d+)"
    $heapAllocTotal = Grab "Heap allocations \(both phases\)\s*:\s*(\d+)"
    $ingestPass = Grab "P50 SPSC ingest latency\s*:.*\[(PASS|FAIL)\]"
    $tickPass   = Grab "P50 tick-to-telemetry latency\s*:.*\[(PASS|FAIL)\]"

    # Section-scoped percentile blocks, in file order: SPSC push, SPSC tick, MPMC push
    $blocks = [regex]::Matches($text, "(?ms)^  P50\s+([\d\.]+) ns\n  P90\s+([\d\.]+) ns\n  P99\s+([\d\.]+) ns\n  P99\.9\s+([\d\.]+) ns\n  Max\s+([\d\.]+) ns")

    $spscPushStats = @("","","","","")
    $spscTickStats = @("","","","","")
    $mpmcPushStats = @("","","","","")
    if ($blocks.Count -ge 1) { $spscPushStats = 1..5 | ForEach-Object { $blocks[0].Groups[$_].Value } }
    if ($blocks.Count -ge 2) { $spscTickStats = 1..5 | ForEach-Object { $blocks[1].Groups[$_].Value } }
    if ($blocks.Count -ge 3) { $mpmcPushStats = 1..5 | ForEach-Object { $blocks[2].Groups[$_].Value } }

    [PSCustomObject]@{
        cpu = $cpu; cores = $cores; tscGhz = $tscGhz
        spscPush = $spscPush; seqIntact = $seqIntact; mpmcPps = $mpmcPps
        heapAllocTotal = $heapAllocTotal
        ingestPass = $ingestPass; tickPass = $tickPass
        spscPushStats = $spscPushStats
        spscTickStats = $spscTickStats
        mpmcPushStats = $mpmcPushStats
        overallPass = ($exitCode -eq 0)
        exitCode = $exitCode
    }
}

Log "Soak run started. Budget=$BudgetMinutes min, duration=$RunDurationSeconds s/run, producers=$ProducerThreads, pin_base_core=$PinBaseCore, cooldown=$CooldownSeconds s"

$startTime = Get-Date
$budget = New-TimeSpan -Minutes $BudgetMinutes
$runIndex = 0

while (((Get-Date) - $startTime) -lt $budget) {
    $runIndex++
    $tsUtc = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")

    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $exePath
    $psi.Arguments = "$RunDurationSeconds $ProducerThreads $PinBaseCore"
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.UseShellExecute = $false
    $proc = New-Object System.Diagnostics.Process
    $proc.StartInfo = $psi
    $proc.Start() | Out-Null
    $stdout = ($proc.StandardOutput.ReadToEnd()) -replace "`r`n", "`n"
    $stderr = ($proc.StandardError.ReadToEnd()) -replace "`r`n", "`n"
    $proc.WaitForExit()
    $exitCode = $proc.ExitCode

    $logName = "run_{0:D4}.log" -f $runIndex
    Set-Content -Path (Join-Path $rawLogDir $logName) -Value ($stdout + "`n" + $stderr)

    $r = Parse-Run -text $stdout -exitCode $exitCode

    $row = @(
        $runIndex, $tsUtc, $RunDurationSeconds, $ProducerThreads, $PinBaseCore,
        ('"' + $r.cpu + '"'), $r.cores, $r.tscGhz,
        $r.spscPush, $r.seqIntact,
        $r.spscPushStats[0], $r.spscPushStats[1], $r.spscPushStats[2], $r.spscPushStats[3], $r.spscPushStats[4],
        $r.spscTickStats[0], $r.spscTickStats[1], $r.spscTickStats[2], $r.spscTickStats[3], $r.spscTickStats[4],
        $r.mpmcPps, $r.mpmcPushStats[0], $r.mpmcPushStats[1], $r.mpmcPushStats[2], $r.mpmcPushStats[3], $r.mpmcPushStats[4],
        $r.heapAllocTotal, $r.ingestPass, $r.tickPass, $r.overallPass, $r.exitCode
    ) -join ","
    Add-Content -Path $csvPath -Value $row

    Log ("Run {0} done: exit={1} spsc_p50={2}ns mpmc_pps={3} heap_allocs={4}" -f `
        $runIndex, $exitCode, $r.spscPushStats[0], $r.mpmcPps, $r.heapAllocTotal)

    if (((Get-Date) - $startTime) -lt $budget) {
        Start-Sleep -Seconds $CooldownSeconds
    }
}

$endTime = Get-Date
$totalWall = ($endTime - $startTime)
Log "Soak loop finished. Total runs=$runIndex, wall time=$($totalWall.ToString())"

# ---------------------------------------------------------------------
# Aggregate summary across all runs in the CSV
# ---------------------------------------------------------------------
$rows = Import-Csv -Path $csvPath
$numRows = $rows.Count

function Median($nums) {
    $sorted = $nums | Sort-Object
    $n = $sorted.Count
    if ($n -eq 0) { return 0 }
    if ($n % 2 -eq 1) { return $sorted[($n-1)/2] }
    return (($sorted[$n/2 - 1] + $sorted[$n/2]) / 2)
}

$spscP50s = $rows | ForEach-Object { [double]$_.spsc_push_p50_ns }
$spscP90s = $rows | ForEach-Object { [double]$_.spsc_push_p90_ns }
$spscP99s = $rows | ForEach-Object { [double]$_.spsc_push_p99_ns }
$spscMaxs = $rows | ForEach-Object { [double]$_.spsc_push_max_ns }
$tickP50s = $rows | ForEach-Object { [double]$_.spsc_tick_p50_ns }
$mpmcPpsAll = $rows | ForEach-Object { [double]$_.mpmc_pushes_per_sec }
$heapTotal = ($rows | ForEach-Object { [int]$_.heap_allocations_total } | Measure-Object -Sum).Sum
$passCount = ($rows | Where-Object { $_.overall_pass -eq "True" }).Count
$seqIntactCount = ($rows | Where-Object { $_.spsc_sequence_intact -eq "yes" }).Count

$cpuName = $rows[0].cpu.Trim('"')
$coreCount = $rows[0].logical_cores
$tscGhz = $rows[0].tsc_ghz

$summary = [PSCustomObject]@{
    TotalRuns             = $numRows
    WallClockMinutes       = [math]::Round($totalWall.TotalMinutes, 1)
    RunDurationSeconds     = $RunDurationSeconds
    ProducerThreads        = $ProducerThreads
    PinBaseCore            = $PinBaseCore
    CPU                    = $cpuName
    LogicalCores           = $coreCount
    TscGHz                 = $tscGhz
    SpscPushP50MedianNs    = [math]::Round((Median $spscP50s), 2)
    SpscPushP90MedianNs    = [math]::Round((Median $spscP90s), 2)
    SpscPushP99MedianNs    = [math]::Round((Median $spscP99s), 2)
    SpscPushMaxWorstNs     = [math]::Round(($spscMaxs | Measure-Object -Maximum).Maximum, 2)
    TickP50MedianNs        = [math]::Round((Median $tickP50s), 2)
    MpmcPpsMedian          = [math]::Round((Median $mpmcPpsAll), 0)
    MpmcPpsMin             = [math]::Round(($mpmcPpsAll | Measure-Object -Minimum).Minimum, 0)
    MpmcPpsMax             = [math]::Round(($mpmcPpsAll | Measure-Object -Maximum).Maximum, 0)
    HeapAllocationsTotal   = $heapTotal
    SequenceIntactRuns     = "$seqIntactCount / $numRows"
    OverallPassRuns        = "$passCount / $numRows"
}

$summary | ConvertTo-Json | Set-Content -Path (Join-Path $artifactDir "soak_aggregate.json")

# ---------------------------------------------------------------------
# CLIENT_EVALUATION_SOAK_REPORT.md -- ASCII/Unicode summary card
# ---------------------------------------------------------------------
$genUtc = (Get-Date).ToUniversalTime().ToString("yyyy-MM-dd HH:mm 'UTC'")
$passRate = if ($numRows -gt 0) { [math]::Round(100.0 * $passCount / $numRows, 1) } else { 0 }
$heapLine = if ($heapTotal -eq 0) { "0 (PASS -- zero heap allocations across every run)" } else { "$heapTotal (FAIL -- see soak_summary.csv for the offending run)" }
$seqLine  = if ($seqIntactCount -eq $numRows) { "intact on all $numRows runs (PASS)" } else { "$seqIntactCount / $numRows intact -- see soak_summary.csv" }

$W = 65
function BoxLine([string]$text) {
    if ($text.Length -gt $W) { $text = $text.Substring(0, $W) }
    return "|" + $text.PadRight($W) + "|"
}
$border = "+" + ("-" * $W) + "+"
$titleText = "ANIMUS CORE -- CLIENT POC EVALUATION SOAK RESULT"
$titlePad = [math]::Max(0, [math]::Floor(($W - $titleText.Length) / 2.0))

$cardLines = @(
    $border
    (BoxLine ((" " * $titlePad) + $titleText))
    $border
    (BoxLine "  CPU        : $cpuName")
    (BoxLine "  Cores      : $coreCount")
    (BoxLine "  Runs       : $numRows")
    (BoxLine "  Wall time  : $($summary.WallClockMinutes) min")
    (BoxLine "  Pin base   : core $PinBaseCore")
    $border
    (BoxLine "  SPSC INGEST LATENCY (median across all runs)")
    (BoxLine "    P50   : $($summary.SpscPushP50MedianNs) ns")
    (BoxLine "    P90   : $($summary.SpscPushP90MedianNs) ns")
    (BoxLine "    P99   : $($summary.SpscPushP99MedianNs) ns")
    (BoxLine "    Max   : $($summary.SpscPushMaxWorstNs) ns (worst sample)")
    $border
    (BoxLine "  TICK-TO-TELEMETRY LATENCY")
    (BoxLine "    P50   : $($summary.TickP50MedianNs) ns")
    $border
    (BoxLine "  MPMC THROUGHPUT (pushes/sec)")
    (BoxLine "    Median: $($summary.MpmcPpsMedian)")
    (BoxLine "    Range : $($summary.MpmcPpsMin) - $($summary.MpmcPpsMax)")
    $border
    (BoxLine "  Heap allocations : $heapTotal total across all runs")
    (BoxLine "  Sequence intact  : $seqIntactCount / $numRows runs")
    (BoxLine "  Overall PASS     : $passRate% ($passCount / $numRows runs)")
    $border
)
$card = ($cardLines -join "`n")

$report = @"
# Client Evaluation Soak Report

Generated: $genUtc
Harness: ``poc_eval/poc_eval_harness.cpp`` (built via ``run_poc_eval.bat``, MSVC cl.exe /O2 /std:c++17)
Method: unattended repeated soak -- $numRows back-to-back runs, $RunDurationSeconds s each, cores pinned starting at logical core $PinBaseCore via ``animus::sys::pin_current_thread_to_core_exclusive``, $CooldownSeconds s cooldown between runs. Every measurement below comes from samples actually captured on this machine during this soak window -- nothing simulated or backfilled.

## Summary card

``````
$card
``````

## Aggregate metrics (median across all $numRows runs)

| Metric | Value |
|---|---|
| CPU | $cpuName |
| Logical cores | $coreCount |
| TSC frequency | $tscGhz GHz |
| Producer threads (MPMC) | $ProducerThreads |
| SPSC push P50 | $($summary.SpscPushP50MedianNs) ns |
| SPSC push P90 | $($summary.SpscPushP90MedianNs) ns |
| SPSC push P99 | $($summary.SpscPushP99MedianNs) ns |
| SPSC push Max (worst observed) | $($summary.SpscPushMaxWorstNs) ns |
| Tick-to-telemetry P50 | $($summary.TickP50MedianNs) ns |
| MPMC pushes/sec (median) | $($summary.MpmcPpsMedian) |
| MPMC pushes/sec (range) | $($summary.MpmcPpsMin) - $($summary.MpmcPpsMax) |
| Heap allocations, total across all runs | $heapLine |
| Event sequence integrity | $seqLine |
| Overall PASS rate | $passRate% ($passCount / $numRows) |

## Notes

- Full per-run data: [``soak_summary.csv``](soak_summary.csv).
- Raw stdout/stderr for every individual run: [``raw_logs/``](raw_logs/).
- Machine-readable aggregate: [``soak_aggregate.json``](soak_aggregate.json).
- This soak used core pinning (base core $PinBaseCore) but not kernel-level isolation (isolcpus/nohz_full/rcu_nocbs) or NUMA-pinned allocation -- see ``docs/technical_eval/COMPATIBILITY_TUNING_GUIDE.md`` for what a fully isolated host adds on top of this.
- ``run_poc_eval.bat`` had a pre-existing quoting bug in its MSVC ``/Fo:`` argument (trailing backslash inside a quoted path being parsed as an escaped quote by cl.exe, corrupting the object-file output path) that made the client-facing entry point fail to build on this machine. Fixed as part of this verification run -- see the diff to ``run_poc_eval.bat``.
- ``poc_eval_harness.cpp`` was extended to additionally report P90 and Max latency (previously only P50/P99/P99.9), reusing the same already-collected sample arrays -- no change to measurement technique, allocation tracking, or PASS/FAIL criteria.
"@

Set-Content -Path $reportPath -Value $report -Encoding utf8

Log "Report written to $reportPath"

if (-not $SkipCommit) {
    # -------------------------------------------------------------------
    # Local commit only -- never push to origin.
    # -------------------------------------------------------------------
    Set-Location $repoRoot
    git add "artifacts/client_kit_soak_results" "poc_eval/poc_eval_harness.cpp" "run_poc_eval.bat"
    $commitMsg = "eval: $numRows-run client kit soak verification (P50/P90/P99/Max), fix run_poc_eval.bat /Fo quoting bug`n`nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>"
    git commit -m $commitMsg
    Log "Committed locally to master (not pushed to origin)."
}
Log "SOAK_WORKFLOW_COMPLETE"
