@echo off
if not exist "%~dp0play\WiiCompiled.exe" (
  echo Run Install-MKW100.cmd first.
  pause
  exit /b 1
)
cd /d "%~dp0play"
start "MKW100" WiiCompiled.exe
