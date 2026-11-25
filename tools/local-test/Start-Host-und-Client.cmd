@echo off
setlocal
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0Extras\Start-Host-und-Client.ps1"
if not errorlevel 1 exit /b 0
echo.
pause
exit /b 1
