@echo off
rem cmd.exe entry point; the real script is lint.ps1 (PowerShell). See docs/WINDOWS.md.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0lint.ps1" %*
exit /b %ERRORLEVEL%
