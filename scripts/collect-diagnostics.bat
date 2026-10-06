@echo off
rem cmd.exe entry point; the real script is collect-diagnostics.ps1 (PowerShell). See docs/WINDOWS.md.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0collect-diagnostics.ps1" %*
exit /b %ERRORLEVEL%
