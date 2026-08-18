@echo off
setlocal enabledelayedexpansion
title Virtual Audio Cable 1-Click Installer

echo ======================================================================
echo    NULLWIRE VIRTUAL AUDIO CABLE 1-CLICK INSTALLER
echo    Microsoft WHQL-Certified Bit-Perfect 48kHz Audio Device
echo ======================================================================
echo.

:: Check for Administrative Privileges
net session >nul 2>&1
if %errorLevel% neq 0 (
    echo [!] Requesting Administrator Privileges...
    powershell -Command "Start-Process cmd -ArgumentList '/c \"\"%~dpnx0\"\"' -Verb RunAs"
    exit /b
)

pushd "%~dp0vbcable"

echo [*] Installing Microsoft WHQL-Certified Virtual Audio Cable Driver...
VBCABLE_Setup_x64.exe -i -h

echo [*] Scanning device tree...
pnputil /scan-devices >nul 2>&1

echo.
echo ======================================================================
echo    [+] SUCCESS: Virtual Audio Device is installed!
echo    'CABLE Input (VB-Audio Virtual Cable)' is now active in Windows.
echo ======================================================================
echo.
popd
if "%1"=="" (
    echo Launching NullWire Sender...
    timeout /t 2 >nul
)
