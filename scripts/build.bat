@echo off
setlocal
cd /d "%~dp0.."
if errorlevel 1 goto :cd_failed

set "PRESET=win-amd64-release"

where rexglue.exe >nul 2>nul
if not errorlevel 1 goto :rexglue_found
if not exist "%CD%\..\rexglue-sdk-dll\out\win-amd64\rexglue.exe" goto :rexglue_missing
set "PATH=%CD%\..\rexglue-sdk-dll\out\win-amd64;%PATH%"

:rexglue_found
echo [1/3] Running rexglue codegen reeot_manifest.toml...
rexglue codegen reeot_manifest.toml
if errorlevel 1 goto :codegen_failed

echo.
echo [2/3] Configuring %PRESET%...
cmake --preset %PRESET%
if errorlevel 1 goto :configure_failed

echo.
echo [3/3] Building...
cmake --build --preset %PRESET% --parallel
if errorlevel 1 goto :build_failed

echo.
echo Codegen and build complete.
goto :success

:cd_failed
echo [FAILED] Could not enter the repository root.
goto :failed

:rexglue_missing
echo [FAILED] rexglue.exe was not found on PATH or in the sibling rexglue-sdk-dll build.
goto :failed

:configure_failed
echo [FAILED] Configure step failed.
goto :failed

:codegen_failed
echo [FAILED] Codegen step failed.
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
