@echo off
setlocal
set "PATH=%~dp0..\..\toolchain\w64devkit\bin;%PATH%"

echo [NullWire Standard] Compiling Uninstaller Resources (uninst.rc -> uninst.res)...
windres -i "%~dp0uninst.rc" -o "%~dp0uninst.res" -O coff
if %ERRORLEVEL% neq 0 (
    echo [NullWire Standard] Uninstaller Resource Compilation Failed!
    exit /b 1
)

echo [NullWire Standard] Compiling Final Release NullWire_Uninstall.exe...
g++ -O3 -s -std=c++20 -mwindows "%~dp0uninstaller.cpp" "%~dp0uninst.res" -lgdiplus -luuid -lole32 -lshell32 -lshlwapi -lcomctl32 -lgdi32 -ladvapi32 -static -o "%~dp0NullWire_Uninstall.exe"
if %ERRORLEVEL% neq 0 (
    echo [NullWire Standard] Uninstaller Build Failed!
    exit /b 1
)

echo [NullWire Standard] Copying binaries for embedded installer payload...
copy /Y "%~dp0..\sender\NullWire.exe" "%~dp0NullWire.exe" >nul

echo [NullWire Standard] Compiling Setup Resources (app.rc -> app.res with embedded binaries)...
windres -i "%~dp0app.rc" -o "%~dp0app.res" -O coff
if %ERRORLEVEL% neq 0 (
    echo [NullWire Standard] Setup Resource Compilation Failed!
    exit /b 1
)

echo [NullWire Standard] Compiling Final Standalone Self-Contained NullWire_Setup.exe...
g++ -O3 -s -std=c++20 -mwindows "%~dp0main.cpp" "%~dp0app.res" -lgdiplus -luuid -lole32 -lshell32 -lshlwapi -lcomctl32 -lgdi32 -ladvapi32 -static -o "%~dp0NullWire_Setup.exe"
if %ERRORLEVEL% equ 0 (
    echo [NullWire Standard] Build Successful! Binaries: NullWire_Setup.exe, NullWire_Uninstall.exe
) else (
    echo [NullWire Standard] Setup Build Failed!
)
endlocal
