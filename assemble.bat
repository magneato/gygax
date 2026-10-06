@echo off
rem cmd.exe entry point; the real script is assemble.ps1 (PowerShell). See docs/WINDOWS.md.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0assemble.ps1" %*
exit /b %ERRORLEVEL%
