<#
  graft - one-line Windows installer (prebuilt binaries, no MSYS2, no toolchain).

    irm https://raw.githubusercontent.com/AEndrix03/Graft/master/install.ps1 | iex

  What it does:
    1. downloads the graft-windows-x86_64.zip release asset + SHA256SUMS
    2. verifies the checksum (fail-closed)
    3. extracts into $env:GRAFT_HOME (default %USERPROFILE%\.graft)
    4. downloads the BGE-M3 embedding model (~600 MB) if not already there
    5. writes ~\.graft\config.yaml with absolute paths (never clobbers yours)
    6. puts ~\.graft\bin on the user PATH and runs a smoke check

  Env knobs: GRAFT_HOME, GRAFT_VERSION, GRAFT_REPO, GRAFT_MODEL_URL,
             GRAFT_NO_MODEL=1, GRAFT_NO_PATH=1, GRAFT_NO_SETUP=1

  To build from source instead, see scripts\build-from-source.ps1.
#>

[CmdletBinding()]
param(
    [string]$GraftHome = $(if ($env:GRAFT_HOME) { $env:GRAFT_HOME } else { Join-Path $env:USERPROFILE ".graft" }),
    [string]$Version   = $env:GRAFT_VERSION,
    [string]$Repo      = $(if ($env:GRAFT_REPO) { $env:GRAFT_REPO } else { "AEndrix03/Graft" })
)

$ErrorActionPreference = "Stop"
$ProgressPreference    = "SilentlyContinue"

function Step($m) { Write-Host "`n==> $m" }
function Ok  ($m) { Write-Host "    ok   $m" }
function Warn($m) { Write-Host "    warn $m" }
function Note($m) { Write-Host "    $m" }
function Fail($m) { Write-Host "    FAIL $m"; exit 1 }

$modelUrl = if ($env:GRAFT_MODEL_URL) { $env:GRAFT_MODEL_URL } else {
    "https://huggingface.co/lm-kit/bge-m3-gguf/resolve/main/bge-m3-Q8_0.gguf"
}
$asset = "graft-windows-x86_64.zip"
$tmp   = Join-Path ([System.IO.Path]::GetTempPath()) ("graft-install-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $tmp -Force | Out-Null

try {
    # ---------- 1. resolve release ----------

    Step "Resolving release"
    $api = if ($Version) {
        "https://api.github.com/repos/$Repo/releases/tags/$Version"
    } else {
        "https://api.github.com/repos/$Repo/releases/latest"
    }
    try {
        $release = Invoke-RestMethod -Uri $api -Headers @{ "User-Agent" = "graft-install" }
    } catch {
        Fail "cannot reach the GitHub release API ($api): $($_.Exception.Message)"
    }
    $tag      = $release.tag_name
    $assetUrl = ($release.assets | Where-Object { $_.name -eq $asset }      | Select-Object -First 1).browser_download_url
    $sumsUrl  = ($release.assets | Where-Object { $_.name -eq "SHA256SUMS" } | Select-Object -First 1).browser_download_url
    if (-not $assetUrl) { Fail "release $tag has no $asset - build from source with scripts\build-from-source.ps1" }
    if (-not $sumsUrl)  { Fail "release $tag publishes no SHA256SUMS - refusing to install unverified binaries" }
    Ok "$tag ($asset)"

    # ---------- 2. download + verify ----------

    Step "Downloading"
    $zip  = Join-Path $tmp $asset
    $sums = Join-Path $tmp "SHA256SUMS"
    Invoke-WebRequest -Uri $assetUrl -OutFile $zip  -Headers @{ "User-Agent" = "graft-install" }
    Invoke-WebRequest -Uri $sumsUrl  -OutFile $sums -Headers @{ "User-Agent" = "graft-install" }

    $want = $null
    foreach ($line in Get-Content $sums) {
        $parts = $line -split '\s+' | Where-Object { $_ }
        if ($parts.Count -ge 2 -and ($parts[1] -eq $asset -or $parts[1] -eq "*$asset")) { $want = $parts[0]; break }
    }
    if (-not $want) { Fail "SHA256SUMS has no entry for $asset - refusing to install" }
    $got = (Get-FileHash -Path $zip -Algorithm SHA256).Hash
    if ($got -ne $want.ToUpper()) { Fail "SHA256 mismatch for $asset (want $want, got $got)" }
    Ok "checksum verified"

    # ---------- 3. extract ----------

    Step "Installing into $GraftHome"
    $stage = Join-Path $tmp "stage"
    Expand-Archive -Path $zip -DestinationPath $stage -Force
    # The CI zip wraps everything in a single top-level folder; a flat archive is
    # also accepted. Pick whichever actually holds bin\graft.exe.
    $root = $stage
    if (-not (Test-Path (Join-Path $root "bin\graft.exe"))) {
        $inner = Get-ChildItem -Path $stage -Directory |
                 Where-Object { Test-Path (Join-Path $_.FullName "bin\graft.exe") } |
                 Select-Object -First 1
        if (-not $inner) { Fail "archive did not contain bin\graft.exe" }
        $root = $inner.FullName
    }
    New-Item -ItemType Directory -Path $GraftHome -Force | Out-Null
    # Copy-Item -Recurse refuses to merge into directories that already exist,
    # which is exactly the re-install case, so walk the tree by hand.
    $rootLen = $root.TrimEnd('\').Length + 1
    foreach ($item in Get-ChildItem -Path $root -Recurse -Force) {
        $target = Join-Path $GraftHome $item.FullName.Substring($rootLen)
        if ($item.PSIsContainer) {
            New-Item -ItemType Directory -Path $target -Force | Out-Null
        } else {
            $parent = Split-Path $target -Parent
            if (-not (Test-Path $parent)) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
            Copy-Item -Path $item.FullName -Destination $target -Force
        }
    }
    $bin = Join-Path $GraftHome "bin"
    if (-not (Test-Path (Join-Path $bin "graft.exe"))) { Fail "install did not produce $bin\graft.exe" }
    Ok "binaries under $bin"

    # ---------- 4. model ----------

    $models = Join-Path $GraftHome "models"
    $model  = Join-Path $models "bge-m3.gguf"
    if ($env:GRAFT_NO_MODEL -eq "1") {
        Warn "skipping model download (GRAFT_NO_MODEL=1) - the daemon cannot embed until $model exists"
    } elseif ((Test-Path $model) -and ((Get-Item $model).Length -gt 0)) {
        Step "Embedding model"
        Ok "already present at $model"
    } else {
        Step "Downloading BGE-M3 embedding model (~600 MB, one time)"
        New-Item -ItemType Directory -Path $models -Force | Out-Null
        $part = "$model.part"
        try {
            Invoke-WebRequest -Uri $modelUrl -OutFile $part -Headers @{ "User-Agent" = "graft-install" }
            Move-Item -Path $part -Destination $model -Force
        } catch {
            Remove-Item $part -Force -ErrorAction SilentlyContinue
            Fail "model download failed: $modelUrl"
        }
        Ok "model at $model"
    }

    # ---------- 5. config ----------

    Step "Configuring"
    $config  = Join-Path $GraftHome "config.yaml"
    $example = @(
        (Join-Path $GraftHome "config.example.yaml"),
        (Join-Path $GraftHome "share\graft\config.example.yaml")
    ) | Where-Object { Test-Path $_ } | Select-Object -First 1

    $viewer = Join-Path $GraftHome "viewer\dist"
    $shared = Join-Path $GraftHome "share\graft\viewer"
    if (Test-Path $shared) { $viewer = $shared }

    if (Test-Path $config) {
        Ok "keeping your existing $config"
    } else {
        # Only the two paths the daemon cannot guess (graftd resolves relative
        # paths against its own cwd). Everything else stays on the built-in
        # defaults, so later releases can improve them for existing installs
        # too - copying the 400-line example here would freeze today's tuning.
        # Forward slashes are YAML-safe on Windows.
        $out = New-Object System.Collections.Generic.List[string]
        $out.Add('# graft configuration.')
        $out.Add('#')
        $out.Add('# Only the paths that depend on where you installed are set here;')
        $out.Add('# every other setting uses the built-in default.')
        if ($example) { $out.Add("# Every available knob, documented: $example") }
        $out.Add('')
        $out.Add('embedding:')
        $out.Add('  model_path: "' + $model.Replace('\', '/') + '"')
        if (Test-Path $viewer) {
            $out.Add('')
            $out.Add('http:')
            $out.Add('  viewer_path: "' + $viewer.Replace('\', '/') + '"')
        }
        # Set-Content -Encoding utf8 writes a BOM on Windows PowerShell, and the
        # YAML reader chokes on it, so write UTF-8 without one.
        [IO.File]::WriteAllLines($config, [string[]]$out, (New-Object System.Text.UTF8Encoding($false)))
        Ok "wrote $config"
    }

    # ---------- 6. PATH ----------

    if ($env:GRAFT_NO_PATH -ne "1") {
        $userPath = [Environment]::GetEnvironmentVariable("Path", "User")
        $entries  = @()
        if ($userPath) { $entries = $userPath -split ';' | Where-Object { $_ } }
        if ($entries -notcontains $bin) {
            Step "Adding $bin to your user PATH"
            $newPath = (@($entries) + $bin) -join ';'
            [Environment]::SetEnvironmentVariable("Path", $newPath, "User")
            Ok "user PATH updated"
            Note "open a new terminal for it to take effect"
        }
        $env:Path = "$bin;$env:Path"
    }

    # ---------- 7. smoke check ----------

    Step "Smoke check"
    # A native command writing to stderr becomes a terminating NativeCommandError
    # while ErrorActionPreference is Stop, which would abort the installer on its
    # very last step. The smoke check is informational: never let it fail the run.
    $smokeOk = $false
    try {
        $ErrorActionPreference = "Continue"
        & (Join-Path $bin "graft.exe") stats 2>&1 | Out-Null
        $smokeOk = ($LASTEXITCODE -eq 0)
    } catch {
        $smokeOk = $false
    } finally {
        $ErrorActionPreference = "Stop"
    }
    if ($smokeOk) {
        Ok "daemon answered - graft is ready"
    } else {
        Warn "'graft stats' did not answer yet"
        Note "the first call cold-starts the daemon and loads the model; run 'graft stats' again"
    }

    # ---------- 8. agent skills ----------

    $setupOk = $false
    if ($env:GRAFT_NO_SETUP -ne "1") {
        Step "Installing the agent skills"
        try {
            $ErrorActionPreference = "Continue"
            & (Join-Path $bin "graft.exe") setup 2>&1 | ForEach-Object { Note $_ }
            $setupOk = ($LASTEXITCODE -eq 0)
        } catch {
            $setupOk = $false
        } finally {
            $ErrorActionPreference = "Stop"
        }
        if (-not $setupOk) { Note "no agent set up yet - run 'graft setup' once your agent is installed" }
    }

    Write-Host "`ngraft $tag installed.`n"
    if ($setupOk) {
        Write-Host "  One step left: run /graft-init inside your agent.`n"
    } else {
        Write-Host "  Next: graft setup      installs the skills into Claude Code / Codex / OpenCode"
        Write-Host "        /graft-init      run that inside the agent; it does the rest`n"
    }

    # The installer succeeded. Without this, the script inherits $LASTEXITCODE
    # from the last native call (smoke check / setup) and reports failure.
    exit 0
}
finally {
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}
