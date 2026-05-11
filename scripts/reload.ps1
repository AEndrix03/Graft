#!/usr/bin/env pwsh
# Reload graftd: stop the running daemon and let the CLI auto-respawn it
# with the current ~/.graft/config.yaml. graftd has no hot-reload signal.
# Usage: .\scripts\reload.ps1

$ErrorActionPreference = "Stop"

$proc = Get-Process -Name graftd -ErrorAction SilentlyContinue
if ($proc) {
    Stop-Process -Id $proc.Id -Force
    Start-Sleep -Milliseconds 500
}

# Drop any leftover unix-socket files that would block respawn on the same path.
$sockDir = Join-Path $env:USERPROFILE ".graft\sockets"
if (Test-Path $sockDir) {
    Get-ChildItem $sockDir -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
}

# Trigger auto-respawn via a cheap CLI call.
graft stats | Out-Null
if ($LASTEXITCODE -ne 0) {
    Write-Error "graftd did not respawn (graft stats exited $LASTEXITCODE)"
    exit 1
}

Write-Output "Reloaded."
