@echo off
setlocal
set "PATH=%~dp0..\toolchain\w64devkit\bin;%PATH%"
echo [NullWire] Compiling Windows Resources (app.rc -> app.res)...
windres -i "%~dp0app.rc" -o "%~dp0app.res" -O coff
if %ERRORLEVEL% neq 0 (
    echo [NullWire] Resource Compilation Failed!
    exit /b 1
)

echo [NullWire] Compiling Final Release NullWireSender.exe with Official Icon...
g++ -O3 -s -std=c++20 -mwindows "%~dp0main.cpp" "%~dp0app.res" -lgdiplus -lavrt -lksuser -lpropsys -lole32 -lws2_32 -lgdi32 -lcomctl32 -lshell32 -static -o "%~dp0NullWireSender.exe"
if %ERRORLEVEL% equ 0 (
    echo [NullWire] Release Build Successful! Binary: %~dp0NullWireSender.exe
) else (
    echo [NullWire] Release Build Failed!
)
endlocal
