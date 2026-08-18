@echo off
setlocal enabledelayedexpansion
title Virtual Audio Cable Uninstaller

echo ======================================================================
echo    NULLWIRE VIRTUAL AUDIO CABLE UNINSTALLATION
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

echo [*] Removing Virtual Audio Device...
VBCABLE_Setup_x64.exe -u -h

echo [*] Scanning device tree...
pnputil /scan-devices >nul 2>&1

echo.
echo ======================================================================
echo    [+] Virtual Audio Device cleanly removed.
echo ======================================================================
echo.
popd
if "%1"=="" timeout /t 2 >nul
