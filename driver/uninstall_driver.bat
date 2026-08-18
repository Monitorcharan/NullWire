@echo off
setlocal enabledelayedexpansion
title NullWire Virtual Audio Driver Uninstaller

echo ======================================================================
echo    NULLWIRE VIRTUAL AUDIO DRIVER UNINSTALLATION
echo ======================================================================
echo.

:: Check for Administrative Privileges
net session >nul 2>&1
if %errorLevel% neq 0 (
    echo [!] Administrator privileges required. Elevating...
    powershell -Command "Start-Process cmd -ArgumentList '/c \"\"%~dpnx0\"\"' -Verb RunAs"
    exit /b
)

pushd "%~dp0"

echo [*] Removing NullWire Virtual Audio Device from Windows Driver Store...
for /f "tokens=1" %%i in ('pnputil /enum-drivers ^| findstr /i "NullWireAudio.inf"') do (
    pnputil /delete-driver %%i /uninstall /force >nul 2>&1
)

echo [*] Scanning device tree...
pnputil /scan-devices >nul 2>&1

echo.
echo ======================================================================
echo    [+] NullWire Virtual Audio Driver cleanly removed.
echo ======================================================================
echo.
popd
if "%1"=="" pause
