@echo off
rem cmd.exe entry point; the real script is sdk-check.ps1 (PowerShell). See docs/WINDOWS.md.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0sdk-check.ps1" %*
exit /b %ERRORLEVEL%
