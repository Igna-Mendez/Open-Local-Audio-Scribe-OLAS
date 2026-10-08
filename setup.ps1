# OLAS — Open Local Audio Scribe (Windows) — build preparation
#
# Prepares a checkout of OLAS 1.1 so it can be configured and built:
#
#   1. verifies the toolchain (CMake, git, a C++ compiler, tar)
#   2. fetches the models the app can load, into .\models\
#   3. reports what CMake needs and how to run it
#
# Idempotent: re-run any time to repair or top up.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File setup.ps1
#   powershell -ExecutionPolicy Bypass -File setup.ps1 -Force
#   powershell -ExecutionPolicy Bypass -File setup.ps1 -SkipModels
#
# A release build bundles the models, so end users never run this. It is for
# building from source.

[CmdletBinding()]
param(
    [string[]]$Languages = @("en", "es"),
    [switch]$SkipModels,
    [switch]$Force
)

$ErrorActionPreference = "Stop"

function Write-Step([string]$msg) {
    Write-Host ""
    Write-Host "==> $msg" -ForegroundColor Cyan
}
function Write-Ok([string]$msg)   { Write-Host "    [ok] $msg" -ForegroundColor Green }
function Write-Warn([string]$msg) { Write-Host "    [warn] $msg" -ForegroundColor Yellow }
function Write-Err([string]$msg)  { Write-Host "    [err] $msg" -ForegroundColor Red }

function Find-Tool([string]$name) {
    return (Get-Command $name -ErrorAction SilentlyContinue)
}

# Script lives at the repo root.
$Root = $PSScriptRoot
if (-not $Root) { $Root = Split-Path -Parent $MyInvocation.MyCommand.Path }
Set-Location $Root

Write-Host ""
Write-Host "OLAS 1.1 - build preparation" -ForegroundColor Cyan
Write-Host "repository: $Root"

# ---------------------------------------------------------------------------
# 1. Toolchain
# ---------------------------------------------------------------------------

Write-Step "Checking the toolchain"

if ($PSVersionTable.PSVersion.Major -lt 5) {
    throw "PowerShell 5.1 or newer is required."
}

$arch = [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture
if ($arch -ne "X64") {
    Write-Warn "Windows $arch detected; only x64 builds are supported."
}

$missing = @()

if (Find-Tool cmake) {
    $cmakeVer = (& cmake --version | Select-Object -First 1)
    Write-Ok "$cmakeVer"
} else {
    $missing += "cmake"
    Write-Err "CMake not found. Install from https://cmake.org/download/ or 'winget install Kitware.CMake'."
}

if (Find-Tool git) {
    Write-Ok "git present (CMake uses it to fetch Moonshine)"
} else {
    $missing += "git"
    Write-Err "git not found. Install from https://git-scm.com/ or 'winget install Git.Git'."
}

if (Find-Tool tar.exe) {
    Write-Ok "tar.exe present"
} else {
    $missing += "tar"
    Write-Err "tar.exe not found; Windows 10 1803 or newer is required."
}

# A C++ toolchain. CMake will look for MSVC or MinGW itself; this is a hint.
if (Find-Tool cl) {
    Write-Ok "MSVC cl.exe on PATH"
} elseif (Find-Tool g++) {
    Write-Ok "g++ on PATH (MinGW)"
} else {
    Write-Warn "No C++ compiler on PATH. CMake can still find MSVC through"
    Write-Warn "Visual Studio, but the 'Desktop development with C++' workload"
    Write-Warn "must be installed:"
    Write-Warn "  winget install Microsoft.VisualStudio.2022.BuildTools"
}

if ($missing.Count -gt 0) {
    Write-Host ""
    Write-Err "Install the missing tool(s) above, then re-run this script."
    exit 1
}

# ---------------------------------------------------------------------------
# 2. Models
# ---------------------------------------------------------------------------

$modelsDir = Join-Path $Root "models"

if ($SkipModels) {
    Write-Step "Skipping model download (-SkipModels)"
} else {
    Write-Step "Fetching models"

    # Delegate to the one script that knows the model layout and the CDN
    # folder probing, so there is a single source of truth. -Set all gets
    # every model the app can load, so both modes work offline.
    $fetch = Join-Path $Root "tools\fetch-streaming-models.ps1"
    if (-not (Test-Path $fetch)) {
        throw "tools\fetch-streaming-models.ps1 not found; is this the repo root?"
    }

    $fetchArgs = @{
        Languages = $Languages
        Set       = "all"
    }
    if ($Force) { $fetchArgs["Force"] = $true }

    & $fetch @fetchArgs
    if ($LASTEXITCODE -ne 0) {
        Write-Err "model fetch failed (exit $LASTEXITCODE)"
        exit 1
    }

    if (-not (Test-Path $modelsDir)) {
        Write-Err "models\ was not created; model fetch failed."
        Write-Err "Run it directly to see the error:"
        Write-Err "  powershell -ExecutionPolicy Bypass -File tools\fetch-streaming-models.ps1"
        exit 1
    }
}

# ---------------------------------------------------------------------------
# 3. Report
# ---------------------------------------------------------------------------

Write-Step "Model inventory"

$expected = @(
    @{ dir = "medium-streaming-en"; mode = "Normal mode (English)" },
    @{ dir = "small-streaming-en";  mode = "Potato mode (English)" },
    @{ dir = "small-streaming-es";  mode = "Spanish (both modes)"  }
)

foreach ($e in $expected) {
    $p = Join-Path $modelsDir $e.dir
    $cfg = Join-Path $p "streaming_config.json"
    if (Test-Path $cfg) {
        $sizeMB = [math]::Round(
            ((Get-ChildItem $p -File | Measure-Object -Property Length -Sum).Sum / 1MB), 1)
        Write-Ok "$($e.dir)  ($sizeMB MB)  - $($e.mode)"
    } else {
        Write-Warn "$($e.dir) missing - $($e.mode) will not work"
    }
}

Write-Host ""
Write-Host "Preparation complete." -ForegroundColor Green
Write-Host ""
Write-Host "Configure and build:" -ForegroundColor Cyan
Write-Host "    mkdir build"
Write-Host "    cd build"
Write-Host "    cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DONNXRUNTIME_MODE=bundled .."
Write-Host "    cmake --build . --config Release -j"
Write-Host ""
Write-Host "The first configure takes 5-15 minutes: it fetches the Moonshine"
Write-Host "sources and builds ONNX Runtime from them."
Write-Host ""
Write-Host "Then run:  .\OLAS.bat      (or build\olas_win.exe from the build dir)"
Write-Host ""
Write-Host "On first launch OLAS asks whether to use Normal mode (Medium"
Write-Host "English) or Potato mode (Small English)."
