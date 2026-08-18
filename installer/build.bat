@echo off
setlocal
set "PATH=%~dp0..\toolchain\w64devkit\bin;%PATH%"

echo [NullWire] Copying app.ico...
if not exist "%~dp0app.ico" (
    copy "%~dp0..\sender\app.ico" "%~dp0app.ico" >nul
)

echo [NullWire] Compiling Uninstaller Resources (uninst.rc -> uninst.res)...
windres -i "%~dp0uninst.rc" -o "%~dp0uninst.res" -O coff
if %ERRORLEVEL% neq 0 (
    echo [NullWire] Uninstaller Resource Compilation Failed!
    exit /b 1
)

echo [NullWire] Compiling Final Release Uninstall.exe...
g++ -O3 -s -std=c++20 -mwindows "%~dp0uninstaller.cpp" "%~dp0uninst.res" -lgdiplus -luuid -lole32 -lshell32 -lshlwapi -lcomctl32 -lgdi32 -ladvapi32 -static -o "%~dp0Uninstall.exe"
if %ERRORLEVEL% neq 0 (
    echo [NullWire] Uninstaller Build Failed!
    exit /b 1
)

echo [NullWire] Copying binaries and certified driver payload for embedded setup...
copy /Y "%~dp0Uninstall.exe" "%~dp0NullWire_Uninstall.exe" >nul
copy /Y "%~dp0Uninstall.exe" "%~dp0..\Uninstall.exe" >nul
copy /Y "%~dp0Uninstall.exe" "%~dp0..\NullWire_Uninstall.exe" >nul

if exist "%~dp0..\sender\NullWire.exe" (
    copy /Y "%~dp0..\sender\NullWire.exe" "%~dp0NullWire.exe" >nul
) else (
    copy /Y "%~dp0..\sender\NullWireSender.exe" "%~dp0NullWire.exe" >nul
)

copy /Y "%~dp0..\driver\vbcable\VBCABLE_Setup_x64.exe" "%~dp0VBCABLE_Setup_x64.exe" >nul
copy /Y "%~dp0..\driver\vbcable\vbaudio_cable64_win7.sys" "%~dp0vbaudio_cable64_win7.sys" >nul
copy /Y "%~dp0..\driver\vbcable\vbaudio_cable64_win7.cat" "%~dp0vbaudio_cable64_win7.cat" >nul
copy /Y "%~dp0..\driver\vbcable\vbMmeCable64_win7.inf" "%~dp0vbMmeCable64_win7.inf" >nul

echo [NullWire] Compiling Setup Resources (app.rc -> app.res with embedded binaries and WHQL driver)...
windres -i "%~dp0app.rc" -o "%~dp0app.res" -O coff
if %ERRORLEVEL% neq 0 (
    echo [NullWire] Setup Resource Compilation Failed!
    exit /b 1
)

echo [NullWire] Compiling Final Standalone Self-Contained Setup.exe...
g++ -O3 -s -std=c++20 -mwindows "%~dp0main.cpp" "%~dp0app.res" -lgdiplus -luuid -lole32 -lshell32 -lshlwapi -lcomctl32 -lgdi32 -ladvapi32 -static -o "%~dp0Setup.exe"
if %ERRORLEVEL% equ 0 (
    copy /Y "%~dp0Setup.exe" "%~dp0NullWire_Setup.exe" >nul
    copy /Y "%~dp0Setup.exe" "%~dp0..\Setup.exe" >nul
    copy /Y "%~dp0Setup.exe" "%~dp0..\NullWire_Setup.exe" >nul
    echo [NullWire] Build Successful! Binaries: Setup.exe, Uninstall.exe
) else (
    echo [NullWire] Setup Build Failed!
)
endlocal
