Write-Host "==================================================" -ForegroundColor Cyan
Write-Host "     ANIMUSCORE TELEMETRY SANDBOX LAUNCHER        " -ForegroundColor Cyan
Write-Host "==================================================" -ForegroundColor Cyan

$portCheck = netstat -ano | findstr 8765
if (-not $portCheck) {
    Write-Host "[*] Starting dashboard telemetry daemon on 127.0.0.1:8765..." -ForegroundColor Yellow
    Start-Process -FilePath "powershell.exe" -ArgumentList "-NoExit", "-Command", "cd '$PSScriptRoot'; .\venv\Scripts\python.exe animus_sandbox\dashboard_server.py"
    Start-Sleep -Seconds 2
} else {
    Write-Host "[+] Telemetry daemon already active on port 8765." -ForegroundColor Green
}

$htmlPath = "$PSScriptRoot\benchmark_artifacts\dashboard_snapshot\index.html"
Write-Host "[*] Launching Chrome client..." -ForegroundColor Yellow
Start-Process "chrome.exe" $htmlPath
Write-Host "[+] Sandbox running." -ForegroundColor Green
