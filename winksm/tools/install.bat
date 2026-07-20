@echo off
REM WinKSM Installation Script
REM Run as Administrator

setlocal enabledelayedexpansion

echo ============================================
echo   WinKSM Installation Script
echo ============================================
echo.

REM Check for admin privileges
net session >nul 2>&1
if %errorLevel% neq 0 (
    echo [ERROR] This script must be run as Administrator!
    echo Right-click and select "Run as administrator"
    pause
    exit /b 1
)

REM Check if build exists
set "BUILD_DIR=%~dp0..\build\bin\Release"
if not exist "%BUILD_DIR%\winksm.sys" (
    echo [ERROR] Driver not found at %BUILD_DIR%\winksm.sys
    echo Please build the project first: powershell -ExecutionPolicy Bypass -File build.ps1
    pause
    exit /b 1
)

REM Enable test signing (required for development)
echo [1/4] Enabling test signing mode...
bcdedit /set testsigning on
if %errorLevel% neq 0 (
    echo [WARNING] Could not enable test signing. Driver may not load.
)

REM Stop existing service if running
echo [2/4] Stopping existing service...
sc stop winksm >nul 2>&1
sc delete winksm >nul 2>&1

REM Install driver
echo [3/4] Installing WinKSM driver...
sc create winksm type= kernel binPath= "%BUILD_DIR%\winksm.sys"
if %errorLevel% neq 0 (
    echo [ERROR] Failed to create driver service
    pause
    exit /b 1
)

sc start winksm
if %errorLevel% neq 0 (
    echo [WARNING] Driver start returned error. This may be normal on first install.
)

REM Install user-mode service
echo [4/4] Installing WinKSM service...
sc create WinKSM_Service type= own start= auto binPath= "%BUILD_DIR%\WinKSM_Service.exe"
if %errorLevel% neq 0 (
    echo [ERROR] Failed to create WinKSM service
    pause
    exit /b 1
)

sc start WinKSM_Service
if %errorLevel% neq 0 (
    echo [WARNING] Service start returned error.
)

echo.
echo ============================================
echo   WinKSM Installed Successfully!
echo ============================================
echo.
echo Dashboard: http://localhost:8787
echo.
echo To uninstall: install.bat --uninstall
echo.

REM Check for --uninstall flag
if "%1"=="--uninstall" (
    echo Uninstalling...
    sc stop WinKSM_Service >nul 2>&1
    sc delete WinKSM_Service >nul 2>&1
    sc stop winksm >nul 2>&1
    sc delete winksm >nul 2>&1
    echo Uninstalled.
)

pause
