#!/usr/bin/env pwsh
# Avvia memgraphd se non è già in esecuzione.
# Uso: .\scripts\memgraphd-start.ps1 [-Config <path>]

param(
    [string]$Config = "$PSScriptRoot\..\config.example.yaml"
)

$ErrorActionPreference = "Stop"

$root = Resolve-Path "$PSScriptRoot\.."
$daemon = Join-Path $root "build\memgraphd.exe"
$llamaBin = Join-Path $root "third_party\llama.cpp\build\bin"
$logOut = Join-Path $root "memgraphd.out.log"
$logErr = Join-Path $root "memgraphd.err.log"

if (Get-Process -Name memgraphd -ErrorAction SilentlyContinue) {
    Write-Output "Already running."
    exit 0
}

if (-not (Test-Path $daemon)) {
    Write-Error "memgraphd.exe non trovato in $daemon. Esegui prima: cmake --build build"
    exit 1
}

$env:PATH = "$llamaBin;$env:PATH"

$proc = Start-Process -FilePath $daemon `
    -ArgumentList "--config",$Config `
    -RedirectStandardOutput $logOut `
    -RedirectStandardError $logErr `
    -WindowStyle Hidden -PassThru

Start-Sleep -Milliseconds 500
if ($proc.HasExited) {
    Write-Error "memgraphd è uscito subito. Controlla $logErr"
    exit 1
}

# Attesa breve che il socket sia pronto (max ~15s)
$deadline = (Get-Date).AddSeconds(15)
while ((Get-Date) -lt $deadline) {
    if (Select-String -Path $logErr -Pattern "listening on" -Quiet -ErrorAction SilentlyContinue) {
        Write-Output "Started."
        exit 0
    }
    if ($proc.HasExited) {
        Write-Error "memgraphd è uscito durante l'avvio. Controlla $logErr"
        exit 1
    }
    Start-Sleep -Milliseconds 300
}

Write-Output "Started."
