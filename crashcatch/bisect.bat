@echo off
if "%~1"=="" (
  echo Drag the game folder onto bisect.bat.
  pause
  exit /b 1
)
"%~dp0crashcatch.exe" --bisect "%~1"
