# WinKSM Build Script
# Requires: Visual Studio 2022, WDK, CMake 3.20+

param(
    [string]$BuildType = "Release",
    [switch]$Clean,
    [switch]$Install
)

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = Split-Path -Parent $ScriptDir
$BuildDir = Join-Path $ProjectRoot "build"

Write-Host "=== WinKSM Build Script ===" -ForegroundColor Cyan
Write-Host "Project root: $ProjectRoot"
Write-Host "Build type: $BuildType"

# Check prerequisites
function Test-Prerequisites {
    $cmake = Get-Command cmake -ErrorAction SilentlyContinue
    if (-not $cmake) {
        Write-Host "[ERROR] CMake not found. Install from https://cmake.org/download/" -ForegroundColor Red
        exit 1
    }

    # Check for Visual Studio
    $vsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vsWhere) {
        $vsPath = & $vsWhere -latest -property installationPath
        if ($vsPath) {
            Write-Host "[OK] Visual Studio found: $vsPath" -ForegroundColor Green
        }
    } else {
        Write-Host "[WARNING] vswhere not found. Ensure Visual Studio 2022 is installed." -ForegroundColor Yellow
    }

    # Check for WDK
    $wdkPath = Get-ItemProperty -Path "HKLM:\SOFTWARE\Microsoft\Windows Driver Kit" -ErrorAction SilentlyContinue
    if (-not $wdkPath) {
        Write-Host "[WARNING] WDK not found. Driver build may fail." -ForegroundColor Yellow
        Write-Host "  Download from: https://learn.microsoft.com/en-us/windows-hardware/drivers/download-the-wdk" -ForegroundColor Yellow
    } else {
        Write-Host "[OK] WDK found" -ForegroundColor Green
    }
}

# Clean build directory
function Clean-Build {
    if (Test-Path $BuildDir) {
        Write-Host "Cleaning build directory..." -ForegroundColor Yellow
        Remove-Item -Recurse -Force $BuildDir
    }
}

# Build
function Build-Project {
    Write-Host "`nCreating build directory..." -ForegroundColor Cyan
    New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null

    Write-Host "Configuring CMake..." -ForegroundColor Cyan
    $cmakeArgs = @(
        "-S", $ProjectRoot,
        "-B", $BuildDir,
        "-G", "Visual Studio 17 2022",
        "-A", "x64"
    )
    & cmake @cmakeArgs
    if ($LASTEXITCODE -ne 0) {
        Write-Host "[ERROR] CMake configure failed" -ForegroundColor Red
        exit 1
    }

    Write-Host "Building..." -ForegroundColor Cyan
    $buildArgs = @(
        "--build", $BuildDir,
        "--config", $BuildType,
        "--parallel"
    )
    & cmake @buildArgs
    if ($LASTEXITCODE -ne 0) {
        Write-Host "[ERROR] Build failed" -ForegroundColor Red
        exit 1
    }

    Write-Host "`n[OK] Build completed successfully!" -ForegroundColor Green
    Write-Host "Output: $BuildDir\bin\$BuildType"
}

# Install (copy to output)
function Install-Project {
    $outputDir = Join-Path $BuildDir "bin\$BuildType"
    if (Test-Path $outputDir) {
        Write-Host "`nBuild artifacts:" -ForegroundColor Cyan
        Get-ChildItem $outputDir -Recurse | ForEach-Object {
            Write-Host "  $_"
        }
    }
}

# Main
Check-Prerequisites

if ($Clean) {
    Clean-Build
}

Build-Project
Install-Project

Write-Host "`n=== Build Complete ===" -ForegroundColor Green
Write-Host "Next steps:"
Write-Host "  1. Enable test signing: bcdedit /set testsigning on"
Write-Host "  2. Install driver: sc create winksm type= kernel binPath= $BuildDir\bin\$BuildType\winksm.sys"
Write-Host "  3. Start service: $BuildDir\bin\$BuildType\WinKSM_Service.exe --console"
