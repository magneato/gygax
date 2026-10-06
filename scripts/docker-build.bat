@echo off
rem cmd.exe entry point; the real script is docker-build.ps1 (PowerShell). See docs/WINDOWS.md.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0docker-build.ps1" %*
exit /b %ERRORLEVEL%
