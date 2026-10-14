@echo off
rem fastfind one-click build: icon+version resource + console + GUI
rem Run by double-click or from any shell. Requires MinGW-w64 in PATH.

cd /d "%~dp0"

where windres >nul 2>nul
if errorlevel 1 (
    echo [ERROR] windres not found in PATH - install MinGW-w64 first.
    goto :end
)
where g++ >nul 2>nul
if errorlevel 1 (
    echo [ERROR] g++ not found in PATH - install MinGW-w64 first.
    goto :end
)

tasklist /FI "IMAGENAME eq fastfindui.exe" 2>nul | find /I "fastfindui.exe" >nul
if not errorlevel 1 (
    echo fastfindui.exe is running - closing it first ...
    taskkill /IM fastfindui.exe /F >nul 2>&1
)

echo [1/3] windres fastfindui.rc ...
windres fastfindui.rc fastfindui_rc.o
if errorlevel 1 (echo [ERROR] windres failed. & goto :end)

echo [2/3] g++ fastfind.exe (console) ...
g++ -O2 -std=c++17 -Wall fastfind.cpp fastfindui_rc.o -o fastfind.exe -ladvapi32
if errorlevel 1 (echo [ERROR] fastfind.exe build failed. & goto :end)

echo [3/3] g++ fastfindui.exe (GUI) ...
g++ -O2 -std=c++17 -municode -mwindows fastfindui.cpp fastfindui_rc.o -o fastfindui.exe -lcomctl32 -lshell32 -lcomdlg32 -ladvapi32 -pthread
if errorlevel 1 (echo [ERROR] fastfindui.exe build failed. & goto :end)

echo.
echo Build OK: fastfind.exe + fastfindui.exe

:end
rem clean up intermediate object file (regenerated on every build)
del /q fastfindui_rc.o >nul 2>&1
pause
