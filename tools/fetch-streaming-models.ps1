#Requires -Version 5.1
[CmdletBinding()]
param(
    [string[]]$Languages = @("en", "es"),
    [string]$Version = "",
    [ValidateSet("default", "small", "medium", "all")]
    [string]$Set = "all",
    [switch]$Force
)

$ErrorActionPreference = "Stop"

# ---- TLS ----
# Old Windows PowerShell defaults to TLS 1.0. download.moonshine.ai requires
# TLS 1.2+. Without this you get "The underlying connection was closed".
[Net.ServicePointManager]::SecurityProtocol = `
    [Net.SecurityProtocolType]::Tls12 -bor `
    [Net.SecurityProtocolType]::Tls11

# ---- locate repo root ----
# This script lives in <root>/tools/, so the parent of $PSScriptRoot is the
# repo root. Fall back to $MyInvocation if $PSScriptRoot is unavailable.
$scriptDir = $PSScriptRoot
if (-not $scriptDir) { $scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path }
$root = Split-Path -Parent $scriptDir
$modelsRoot = Join-Path $root "models"

# ---- per-language model family ----
#
# OLAS 1.1 has two modes and both must work offline, so the default fetch set
# (-Set all) is every model the app can load:
#
#   medium-streaming-en   Normal mode, English
#   small-streaming-en    Potato mode, English
#   small-streaming-<x>   every other language (only Small exists)
#
# -Set default fetches just what Normal mode needs (medium English, small
# others); -Set small or -Set medium forces one family everywhere.
#
# Get-Families returns a list because -Set all yields two entries for English.
# Note: no ternary operator here. The script declares #Requires -Version 5.1
# and PS 5.1 has no `? :`, only PS 7+ does.
function Get-Families([string]$lang) {
    switch ($Set) {
        "small"  { return @("small-streaming") }
        "medium" { return @("medium-streaming") }
        "default" {
            if ($lang -eq "en") { return @("medium-streaming") }
            return @("small-streaming")
        }
        default {
            if ($lang -eq "en") { return @("medium-streaming", "small-streaming") }
            return @("small-streaming")
        }
    }
}

# ---- known quantized folder names ----
# Moonshine publishes a new dated folder periodically. The script probes these
# in order and uses the first one that responds. If a download 404s, run the
# directory-listing probe manually (see bottom of this file) and add the new
# name to the top of this list.
$KnownVersions = @(
    "quantized_26_08_24",
    "quantized_26_08_21",
    "quantized_26_08_15",
    "quantized_26_08_08",
    "quantized_26_08_01",
    "quantized_26_07_25"
)

$Files = @(
    "adapter.ort",
    "cross_kv.ort",
    "decoder_kv.ort",
    "encoder.ort",
    "frontend.model.ort",
    "frontend.weights.ort",
    "streaming_config.json",
    "tokenizer.bin"
)

function Test-Url([string]$url) {
    try {
        $r = Invoke-WebRequest -Uri $url -UseBasicParsing -TimeoutSec 15
        return $r.StatusCode -eq 200
    } catch {
        return $false
    }
}

function Resolve-Version([string]$family, [string]$lang, [string]$explicit) {
    if ($explicit) { return $explicit }

    Write-Host "  probing available quantized folders for $family-$lang..."
    foreach ($v in $KnownVersions) {
        $probe = "https://download.moonshine.ai/model/$family-$lang/$v/streaming_config.json"
        Write-Host "    try $v ..." -NoNewline
        if (Test-Url $probe) {
            Write-Host " OK"
            return $v
        }
        Write-Host " 404"
    }
    throw "No valid quantized_* folder found for $family-$lang. " +
          "Check https://download.moonshine.ai/model/$family-$lang/ " +
          "and add the current folder name to `$KnownVersions at the top of this script."
}

function Fetch-Model([string]$family, [string]$lang, [string]$forcedVersion) {
    $version = Resolve-Version $family $lang $forcedVersion
    $base = "https://download.moonshine.ai/model/$family-$lang/$version"
    $dst  = Join-Path $modelsRoot "$family-$lang"

    Write-Host ""
    Write-Host "== $family-$lang ==" -ForegroundColor Cyan
    Write-Host "   version: $version"
    Write-Host "   source : $base"
    Write-Host "   target : $dst"

    New-Item -ItemType Directory -Force -Path $dst | Out-Null

    foreach ($f in $Files) {
        $out = Join-Path $dst $f
        if ((Test-Path $out) -and -not $Force) {
            $sz = (Get-Item $out).Length
            Write-Host ("   {0,-22} cached  ({1,14:N0} bytes)" -f $f, $sz)
            continue
        }
        Write-Host ("   {0,-22} downloading..." -f $f) -NoNewline
        try {
            Invoke-WebRequest -Uri "$base/$f" -OutFile $out -UseBasicParsing
            $sz = (Get-Item $out).Length
            Write-Host ("`b`b`b`b`b`b`b`b`b`b`b`b`b`b {0,14:N0} bytes" -f $sz)
        } catch {
            Write-Host ""
            Write-Host "   ERROR downloading $f" -ForegroundColor Red
            Write-Host "     $($_.Exception.Message)" -ForegroundColor Red
            throw
        }
    }
}

Write-Host "Fetching streaming models into $modelsRoot (set: $Set)"
$wanted = @()
foreach ($lang in $Languages) {
    foreach ($family in (Get-Families $lang)) {
        $wanted += ,@($family, $lang)
        Fetch-Model $family $lang $Version
    }
}

Write-Host ""
Write-Host "Verifying..." -ForegroundColor Cyan
$all_ok = $true
foreach ($pair in $wanted) {
    $family = $pair[0]; $lang = $pair[1]
    $dst = Join-Path $modelsRoot "$family-$lang"
    foreach ($f in $Files) {
        $p = Join-Path $dst $f
        if (-not (Test-Path $p) -or (Get-Item $p).Length -eq 0) {
            Write-Host "  MISSING or empty: $p" -ForegroundColor Red
            $all_ok = $false
        }
    }
}

Write-Host ""
if ($all_ok) {
    Write-Host "All model files present." -ForegroundColor Green
    Write-Host ""
    Write-Host "Models ready. Run the app with:"
    Write-Host "    cd `"$root`""
    Write-Host "    .\OLAS.bat"
    Write-Host ""
    Write-Host "First launch asks whether to use Normal mode (Medium English)"
    Write-Host "or Potato mode (Small English)."
    Write-Host ""
    Write-Host "Normal mode needs medium-streaming-en; Potato mode needs"
    Write-Host "small-streaming-en. -Set all (the default) fetches both, so"
    Write-Host "the choice works offline either way."
} else {
    Write-Host "Some files are missing. Re-run with -Force to redownload." -ForegroundColor Yellow
    exit 1
}