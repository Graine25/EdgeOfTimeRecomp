@echo off
setlocal
cd /d "%~dp0.."
if errorlevel 1 goto :cd_failed

set "PRESET=win-amd64-relwithdebinfo"

if not exist "generated\default\sources.cmake" goto :missing_codegen
if not exist "generated\default\codegen.build.stamp" goto :missing_codegen

echo [1/2] Configuring %PRESET%...
cmake --preset %PRESET%
if errorlevel 1 goto :configure_failed

echo.
echo [2/2] Building from existing generated sources...
cmake --build --preset %PRESET% --parallel
if errorlevel 1 goto :build_failed

echo.
echo Build complete.
goto :success

:cd_failed
echo [FAILED] Could not enter the repository root.
goto :failed

:missing_codegen
echo [FAILED] Generated sources are missing. Run scripts\build.bat first.
goto :failed

:configure_failed
echo [FAILED] Configure step failed.
goto :failed

:build_failed
echo [FAILED] Build step failed.
goto :failed

:failed
if /i not "%~1"=="--no-pause" pause
exit /b 1

:success
if /i not "%~1"=="--no-pause" pause
exit /b 0
