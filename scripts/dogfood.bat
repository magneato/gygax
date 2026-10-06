@echo off
rem cmd.exe entry point; the real script is dogfood.ps1 (PowerShell). See docs/WINDOWS.md.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0dogfood.ps1" %*
exit /b %ERRORLEVEL%
