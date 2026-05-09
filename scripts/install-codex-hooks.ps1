#!/usr/bin/env pwsh
# Install memgraph hooks for Codex.

[CmdletBinding()]
param(
    [string]$CodexHome = (Join-Path $env:USERPROFILE ".codex")
)

$ErrorActionPreference = "Stop"

function Write-Step($msg) { Write-Host "`n==> $msg" -ForegroundColor Cyan }
function Write-Ok($msg) { Write-Host "  [OK] $msg" -ForegroundColor Green }
function Write-Utf8NoBom($path, $text) {
    $encoding = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($path, $text, $encoding)
}

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$sourceDir = Join-Path $repoRoot "integrations\codex\hooks\memgraph"
$targetDir = Join-Path $CodexHome "hooks\memgraph"
$configPath = Join-Path $CodexHome "config.toml"
$hooksPath = Join-Path $CodexHome "hooks.json"

if (-not (Test-Path $sourceDir)) {
    throw "Missing hook source directory: $sourceDir"
}

Write-Step "Copying Codex memgraph hooks..."
New-Item -ItemType Directory -Force -Path $targetDir | Out-Null
Copy-Item -Force (Join-Path $sourceDir "*.js") $targetDir
Write-Ok "hooks copied to $targetDir"

Write-Step "Updating Codex feature flag..."
New-Item -ItemType Directory -Force -Path $CodexHome | Out-Null
if (-not (Test-Path $configPath)) {
    Write-Utf8NoBom $configPath "[features]`nhooks = true`n"
} else {
    $config = Get-Content -Raw $configPath
    $config = $config -replace '(?m)^\s*codex_hooks\s*=\s*true\s*\r?\n?', ''
    if ($config -match '(?m)^\[features\]\s*$') {
        if ($config -notmatch '(?m)^\s*hooks\s*=') {
            $config = $config -replace '(?m)^(\[features\]\s*)$', "`$1`nhooks = true"
        } else {
            $config = $config -replace '(?m)^\s*hooks\s*=.*$', 'hooks = true'
        }
    } else {
        $config = $config.TrimEnd() + "`n`n[features]`nhooks = true`n"
    }
    Write-Utf8NoBom $configPath $config
}
Write-Ok "enabled [features].hooks"

Write-Step "Writing Codex hooks.json entries..."
$query = (Join-Path $targetDir "query_inject.js").Replace("\", "/")
$mark = (Join-Path $targetDir "mark_candidate.js").Replace("\", "/")
$propose = (Join-Path $targetDir "propose_memoryze.js").Replace("\", "/")

if (Test-Path $hooksPath) {
    $raw = Get-Content -Raw $hooksPath
    $hooksDoc = if ([string]::IsNullOrWhiteSpace($raw)) {
        [pscustomobject]@{}
    } else {
        $raw | ConvertFrom-Json
    }
} else {
    $hooksDoc = [pscustomobject]@{}
}

$hooksDoc | Add-Member -Force -MemberType NoteProperty -Name hooks -Value ([pscustomobject]@{
    UserPromptSubmit = @(
        [pscustomobject]@{
            hooks = @(
                [pscustomobject]@{
                    type = "command"
                    command = "node `"$query`""
                    timeout = 10
                    statusMessage = "memgraph cache lookup"
                }
            )
        }
    )
    PostToolUse = @(
        [pscustomobject]@{
            matcher = "apply_patch"
            hooks = @(
                [pscustomobject]@{
                    type = "command"
                    command = "node `"$mark`""
                    timeout = 5
                }
            )
        }
    )
    Stop = @(
        [pscustomobject]@{
            hooks = @(
                [pscustomobject]@{
                    type = "command"
                    command = "node `"$propose`""
                    timeout = 5
                }
            )
        }
    )
})

Write-Utf8NoBom $hooksPath (($hooksDoc | ConvertTo-Json -Depth 20) + "`n")
Write-Ok "hooks.json updated at $hooksPath"

Write-Step "Done."
Write-Host "Restart Codex so it reloads config.toml and hooks.json."
