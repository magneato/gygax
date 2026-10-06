@echo off
rem cmd.exe entry point; the real script is setup.ps1 (PowerShell). See docs/WINDOWS.md.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0setup.ps1" %*
exit /b %ERRORLEVEL%
