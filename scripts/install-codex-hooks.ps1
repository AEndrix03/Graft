#!/usr/bin/env pwsh
# Install graft hooks for Codex.

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
$sourceDir = Join-Path $repoRoot "integrations\codex\hooks\graft"
$targetDir = Join-Path $CodexHome "hooks\graft"
$configPath = Join-Path $CodexHome "config.toml"
$hooksPath = Join-Path $CodexHome "hooks.json"

if (-not (Test-Path $sourceDir)) {
    throw "Missing hook source directory: $sourceDir"
}

Write-Step "Copying Codex graft hooks..."
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
$mergeScript = @'
const fs = require('fs');
const path = require('path');
const hooksPath = process.argv[2];
const targetDir = process.argv[3].replace(/\\/g, '/');
let doc = {};
try {
  if (fs.existsSync(hooksPath)) {
    const raw = fs.readFileSync(hooksPath, 'utf8').replace(/^\uFEFF/, '').trim();
    if (raw) doc = JSON.parse(raw);
  }
} catch (e) {
  console.error(`cannot parse existing hooks file: ${hooksPath}: ${e.message}`);
  process.exit(2);
}
if (!doc || Array.isArray(doc) || typeof doc !== 'object') doc = {};
if (!doc.hooks || Array.isArray(doc.hooks) || typeof doc.hooks !== 'object') doc.hooks = {};
function cmd(script, timeout, statusMessage) {
  const hook = {
    type: 'command',
    command: `node "${script.replace(/"/g, '\\"')}"`,
    timeout,
    __script: script,
  };
  if (statusMessage) hook.statusMessage = statusMessage;
  return hook;
}
function sameHook(hook, script) {
  return hook && hook.type === 'command' && typeof hook.command === 'string' && hook.command.includes(script);
}
function upsert(event, matcher, hook) {
  const groups = Array.isArray(doc.hooks[event]) ? doc.hooks[event] : [];
  let placed = false;
  doc.hooks[event] = groups.map(group => {
    if (!group || typeof group !== 'object') return group;
    const hooks = Array.isArray(group.hooks) ? group.hooks.filter(h => !sameHook(h, hook.__script)) : [];
    const matches = matcher === null ? !Object.prototype.hasOwnProperty.call(group, 'matcher') : group.matcher === matcher;
    if (!matches) return { ...group, hooks };
    placed = true;
    return { ...group, hooks: [...hooks, hook] };
  });
  if (!placed) {
    doc.hooks[event].push(matcher === null ? { hooks: [hook] } : { matcher, hooks: [hook] });
  }
  for (const group of doc.hooks[event]) {
    if (group && Array.isArray(group.hooks)) {
      for (const h of group.hooks) delete h.__script;
    }
  }
}
upsert('UserPromptSubmit', null, cmd(path.posix.join(targetDir, 'query_inject.js'), 10, 'graft cache lookup'));
upsert('PostToolUse', 'apply_patch', cmd(path.posix.join(targetDir, 'mark_candidate.js'), 5));
upsert('Stop', null, cmd(path.posix.join(targetDir, 'propose_memoryze.js'), 5));
fs.mkdirSync(path.dirname(hooksPath), { recursive: true });
fs.writeFileSync(hooksPath, JSON.stringify(doc, null, 2) + '\n', { encoding: 'utf8' });
'@
$tmpScript = Join-Path $env:TEMP "graft-merge-codex-hooks.js"
Write-Utf8NoBom $tmpScript $mergeScript
try {
    & node $tmpScript $hooksPath $targetDir
    if ($LASTEXITCODE -ne 0) {
        throw "node merge failed with exit code $LASTEXITCODE"
    }
} finally {
    Remove-Item -Force $tmpScript -ErrorAction SilentlyContinue
}
Write-Ok "hooks.json updated at $hooksPath"

Write-Step "Done."
Write-Host "Restart Codex so it reloads config.toml and hooks.json."
