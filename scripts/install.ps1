#!/usr/bin/env pwsh
# graft - Windows installer.
#
# graft builds with MinGW64 from MSYS2. This script:
#   1. checks for an MSYS2 install (offers to install it via winget)
#   2. invokes scripts/install.sh inside the MSYS2 MINGW64 shell - which
#      handles every other step (deps, llama.cpp, model, build, smoke test).
#
# If you already have MSYS2 set up, you can skip this entirely and just run:
#   bash scripts/install.sh    (from MSYS2 MinGW64 shell)

[CmdletBinding()]
param(
    [string]$Msys2Root = $env:MSYS2_ROOT,
    [ValidateSet("none","cuda","hip")]
    [string]$Gpu = $(if ($env:GRAFT_GPU) { $env:GRAFT_GPU } else { "none" })
)

$ErrorActionPreference = "Stop"

function Write-Step($msg) { Write-Host "`n==> $msg" -ForegroundColor Cyan }
function Write-Ok  ($msg) { Write-Host "  [OK] $msg"      -ForegroundColor Green }
function Write-Warn($msg) { Write-Host "  [!]  $msg"      -ForegroundColor Yellow }
function Write-Err ($msg) { Write-Host "  [X]  $msg"      -ForegroundColor Red; exit 1 }

function Ask-YesNo($prompt, [string]$default = "Y") {
    $hint = if ($default -eq "Y") { "[Y/n]" } else { "[y/N]" }
    $ans  = Read-Host "$prompt $hint"
    if ([string]::IsNullOrWhiteSpace($ans)) { $ans = $default }
    return $ans -match '^(y|yes)$'
}

# ---------- detect MSYS2 ----------

Write-Step "Looking for MSYS2..."
$candidates = @($Msys2Root, "C:\msys64", "C:\tools\msys64", "$env:USERPROFILE\msys64") |
    Where-Object { $_ } | Select-Object -Unique
$found = $null
foreach ($c in $candidates) {
    if (Test-Path "$c\usr\bin\bash.exe") { $found = $c; break }
}

if (-not $found) {
    Write-Warn "MSYS2 not found."
    if (Get-Command winget -ErrorAction SilentlyContinue) {
        if (Ask-YesNo "Install MSYS2 via winget now?" "Y") {
            winget install --id MSYS2.MSYS2 --silent --accept-source-agreements --accept-package-agreements
            $found = "C:\msys64"
            if (-not (Test-Path "$found\usr\bin\bash.exe")) {
                Write-Err "MSYS2 install did not produce $found - please install manually from https://www.msys2.org and re-run."
            }
            Write-Ok "MSYS2 installed at $found"
        } else {
            Write-Err "Install MSYS2 manually from https://www.msys2.org and re-run."
        }
    } else {
        Write-Err "winget not available. Install MSYS2 manually from https://www.msys2.org and re-run."
    }
} else {
    Write-Ok "MSYS2 found at $found"
}

# ---------- run install.sh inside MINGW64 ----------

$repo = (Resolve-Path "$PSScriptRoot\..").Path
# Convert C:\foo\bar to /c/foo/bar for MSYS
$drive = $repo.Substring(0, 1).ToLower()
$rest  = $repo.Substring(2) -replace '\\', '/'
$repoMsys = "/$drive$rest"

Write-Step "Launching MSYS2 MINGW64 shell to run install.sh..."
if ($Gpu -ne "none") { Write-Host "  GPU backend: $Gpu" -ForegroundColor Cyan }
$bash = "$found\usr\bin\bash.exe"
$env:CHERE_INVOKING = "1"
$env:MSYSTEM        = "MINGW64"
$env:GRAFT_GPU   = $Gpu

& $bash -lc "cd '$repoMsys'; GRAFT_GPU='$Gpu' bash scripts/install.sh --yes"

if ($LASTEXITCODE -ne 0) { Write-Err "install.sh exited with code $LASTEXITCODE" }

$installDir = Join-Path $env:USERPROFILE ".graft"

Write-Step ("All done. graft is installed at: " + $installDir)
Write-Host ""
Write-Host "The installer also added " ($installDir + "\bin") " to your user PATH."
Write-Host "Open a NEW PowerShell window (so the PATH change is picked up) and try:"
Write-Host "  graft profile list"
Write-Host "  graft insert --summary hello --detail world --keyword test"
Write-Host "  graft query hello"
Write-Host "  graft analytics"
