<#
One-time setup: registers a Windows Scheduled Task that starts
telemetry_bridge.py (via the restart-forever wrapper) automatically at
system startup, running as SYSTEM so it doesn't need anyone logged in.

MUST be run from an elevated ("Run as Administrator") PowerShell prompt.
Safe to re-run -- it replaces the existing task definition if one exists.

Usage:
    .\scripts\install_telemetry_bridge_task.ps1
    .\scripts\install_telemetry_bridge_task.ps1 -AllowOrigin "https://your-dashboard-domain.example"

After running this once, telemetry_bridge.py will start automatically on
every boot and restart itself if it ever crashes -- no need to keep a
terminal window open. Check scripts\logs\telemetry_bridge.log for output.

To remove it later:
    Unregister-ScheduledTask -TaskName "AnimusTelemetryBridge" -Confirm:$false
#>

param(
    [string]$AllowOrigin = ""
)

$RepoRoot = Split-Path -Parent $PSScriptRoot
$WrapperScript = Join-Path $RepoRoot "scripts\run_telemetry_bridge_forever.ps1"

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Write-Error "This script must be run from an elevated (Administrator) PowerShell prompt. Right-click PowerShell -> Run as Administrator, then re-run this script."
    exit 1
}

$taskName = "AnimusTelemetryBridge"
$psExe = (Get-Command powershell.exe).Source

$argumentList = "-NoProfile -ExecutionPolicy Bypass -File `"$WrapperScript`""
if ($AllowOrigin -ne "") {
    $argumentList += " -AllowOrigin `"$AllowOrigin`""
}

$action = New-ScheduledTaskAction -Execute $psExe -Argument $argumentList -WorkingDirectory $RepoRoot
$trigger = New-ScheduledTaskTrigger -AtStartup
$settings = New-ScheduledTaskSettingsSet `
    -ExecutionTimeLimit ([TimeSpan]::Zero) `
    -RestartCount 999 `
    -RestartInterval (New-TimeSpan -Minutes 1) `
    -DontStopOnIdleEnd `
    -AllowStartIfOnBatteries `
    -DontStopIfGoingOnBatteries

Register-ScheduledTask -TaskName $taskName -Action $action -Trigger $trigger -Settings $settings `
    -User "SYSTEM" -RunLevel Highest -Force | Out-Null

Write-Host "Registered scheduled task '$taskName' -- it will start telemetry_bridge.py automatically on every boot."
Write-Host "Starting it now so it's live immediately (not just after the next reboot)..."
Start-ScheduledTask -TaskName $taskName
Start-Sleep -Seconds 2
Write-Host "Status: $((Get-ScheduledTask -TaskName $taskName).State)"
Write-Host "Logs: $RepoRoot\scripts\logs\telemetry_bridge.log"
