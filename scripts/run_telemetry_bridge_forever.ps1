<#
Restart-forever wrapper around telemetry_bridge.py.

Windows Task Scheduler's "AtStartup" trigger only launches a task once --
it doesn't restart it if the process itself crashes or exits. This script
is the thing Task Scheduler actually launches: it loops forever, restarting
telemetry_bridge.py whenever it exits for any reason, with a short cooldown
so a fast-crashing process doesn't spin the CPU.

Not meant to be run by hand for normal use (just run
`python telemetry_bridge.py` directly for that) -- this is specifically
for the 24/7 unattended service path. See scripts/install_telemetry_bridge_task.ps1.
#>

param(
    [string]$AllowOrigin = "",
    [int]$Port = 8766
)

$RepoRoot = Split-Path -Parent $PSScriptRoot
$LogDir = Join-Path $RepoRoot "scripts\logs"
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null
$LogFile = Join-Path $LogDir "telemetry_bridge.log"

Set-Location $RepoRoot

$argsList = @("telemetry_bridge.py", "--port", $Port)
if ($AllowOrigin -ne "") {
    foreach ($origin in ($AllowOrigin -split ",")) {
        $argsList += @("--allow-origin", $origin.Trim())
    }
}

while ($true) {
    $timestamp = Get-Date -Format "yyyy-MM-dd HH:mm:ss"
    Add-Content -Path $LogFile -Value "[$timestamp] starting: python $($argsList -join ' ')"
    try {
        & python @argsList *>> $LogFile
    } catch {
        Add-Content -Path $LogFile -Value "[$timestamp] launch error: $_"
    }
    $timestamp = Get-Date -Format "yyyy-MM-dd HH:mm:ss"
    Add-Content -Path $LogFile -Value "[$timestamp] telemetry_bridge.py exited -- restarting in 5s"
    Start-Sleep -Seconds 5
}
