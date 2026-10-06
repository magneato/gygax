@echo off
rem cmd.exe entry point; the real script is build.ps1 (PowerShell, run through
rem the gygax-dev Docker image). See docs/WINDOWS.md.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
exit /b %ERRORLEVEL%
