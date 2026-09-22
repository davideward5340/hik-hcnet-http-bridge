@echo off
setlocal EnableExtensions DisableDelayedExpansion
if not exist "%~dp0windows-service.cmd" (
    echo [ERROR] Missing windows-service.cmd. Keep the complete package together.
    exit /b 1
)
"%~dp0windows-service.cmd" uninstall %*
