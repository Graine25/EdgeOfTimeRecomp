@echo off
setlocal
set "PAUSE_ON_EXIT=1"
if /i "%~1"=="--no-pause" (
    set "PAUSE_ON_EXIT="
    shift /1
)

cd /d "%~dp0.."
if errorlevel 1 (
    echo [FAILED] Could not enter the repository root.
    if defined PAUSE_ON_EXIT pause
    exit /b 1
)

python scripts\add_latest_missing_boundary.py %1 %2 %3 %4 %5 %6 %7 %8 %9
set "RESULT=%errorlevel%"

echo.
if "%RESULT%"=="0" (
    echo Done. Run scripts\build.bat to regenerate and rebuild.
) else (
    echo [FAILED] No boundary was added.
)

if defined PAUSE_ON_EXIT pause
exit /b %RESULT%
