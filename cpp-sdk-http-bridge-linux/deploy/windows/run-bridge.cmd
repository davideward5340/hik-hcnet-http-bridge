@echo off
setlocal EnableExtensions
cd /d "%~dp0"
"%~dp0hik-sdk-http-bridge.exe" run --config "%~dp0config.json"
exit /b %errorlevel%
