@echo off
setlocal

rem Resolve the project directory without leaving a trailing backslash
rem inside a quoted CMake -S argument.
cd /d "%~dp0"
if errorlevel 1 (
    echo ERROR: Could not change to project directory.
    pause
    exit /b 1
)

where cmake >nul 2>&1
if errorlevel 1 (
    echo ERROR: CMake not found. Install Visual Studio 2022 with
    echo Desktop development with C++ and CMake.
    pause
    exit /b 1
)

cmake -S . -B build -G "Visual Studio 17 2022" -A x64
if errorlevel 1 (
    echo ERROR: CMake configuration failed.
    pause
    exit /b 1
)

cmake --build build --config Release --parallel
if errorlevel 1 (
    echo ERROR: Compilation failed.
    pause
    exit /b 1
)

echo.
echo Build successful!
echo Executable: "%CD%\build\Release\PS5DirectView.exe"
pause
endlocal
