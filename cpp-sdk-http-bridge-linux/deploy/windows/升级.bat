@echo off
setlocal EnableExtensions DisableDelayedExpansion
if not exist "%~dp0hik-bridge-upgrade.exe" (
    echo [ERROR] Missing hik-bridge-upgrade.exe. Extract the complete new package first.
    exit /b 1
)
if /i "%~1"=="/nopause" goto :run
"%~dp0hik-bridge-upgrade.exe" %*
set "RESULT=%errorlevel%"
if /i "%~1"=="/check" goto :finish
pause
goto :finish
:run
"%~dp0hik-bridge-upgrade.exe"
set "RESULT=%errorlevel%"
:finish
exit /b %RESULT%
