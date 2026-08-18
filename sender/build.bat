@echo off
setlocal
set "PATH=%~dp0..\toolchain\w64devkit\bin;%PATH%"
echo [NullWire] Compiling Windows Resources (app.rc -> app.res)...
windres -i "%~dp0app.rc" -o "%~dp0app.res" -O coff
if %ERRORLEVEL% neq 0 (
    echo [NullWire] Resource Compilation Failed!
    exit /b 1
)

echo [NullWire] Compiling Final Release NullWire.exe with Official Icon...
g++ -O3 -s -std=c++20 -mwindows "%~dp0main.cpp" "%~dp0app.res" -lgdiplus -lavrt -lwinmm -lksuser -lpropsys -lole32 -lws2_32 -lgdi32 -lcomctl32 -lshell32 -ldwmapi -lmsimg32 -static -o "%~dp0NullWire.exe"
if %ERRORLEVEL% equ 0 (
    copy /Y "%~dp0NullWire.exe" "%~dp0NullWireSender.exe" >nul
    echo [NullWire] Release Build Successful! Binary: %~dp0NullWire.exe
) else (
    echo [NullWire] Release Build Failed!
)
endlocal
