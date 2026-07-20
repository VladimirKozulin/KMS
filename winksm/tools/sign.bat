@echo off
REM WinKSM Test Signing Script
REM Enables test signing mode for driver development

echo ============================================
echo   WinKSM Test Signing Setup
echo ============================================
echo.

REM Check for admin
net session >nul 2>&1
if %errorLevel% neq 0 (
    echo [ERROR] Run as Administrator!
    pause
    exit /b 1
)

echo Enabling test signing mode...
bcdedit /set testsigning on

if %errorLevel% equ 0 (
    echo.
    echo [OK] Test signing enabled.
    echo.
    echo You may need to reboot for changes to take effect.
    echo After reboot, you can install the WinKSM driver.
    echo.
    echo To disable later: bcdedit /set testsigning off
) else (
    echo [ERROR] Failed to enable test signing.
)

pause
