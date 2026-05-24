@echo off
REM Convenience wrapper that calls build.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1"
