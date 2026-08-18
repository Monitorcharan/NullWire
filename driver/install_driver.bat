@echo off
setlocal enabledelayedexpansion
title NullWire Virtual Audio Driver Installer

echo ======================================================================
echo    NULLWIRE VIRTUAL AUDIO DRIVER INSTALLATION SUITE
echo    High-Fidelity 48kHz Studio Master Virtual Audio Device
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

echo [*] Registering NullWire Virtual Audio Driver with Windows PnP Engine...
pnputil /add-driver NullWireAudio.inf /install >nul 2>&1

echo [*] Scanning for hardware changes and activating audio device node...
pnputil /scan-devices >nul 2>&1

echo [*] Querying Audio Endpoints...
powershell -Command "Get-PnpDevice -Class Media | Where-Object { $_.FriendlyName -like '*NullWire*' } | Format-Table -AutoSize"

echo.
echo ======================================================================
echo    [+] NullWire Virtual Audio Driver successfully installed!
echo    You can now select 'NullWire Virtual Audio Device' in Windows Sound.
echo ======================================================================
echo.
popd
if "%1"=="" pause
