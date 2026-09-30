@echo off
REM Install MoonLight for the current user. Double-click this file.
REM
REM This is a three-line wrapper around Install-MoonLight.ps1, which is where the actual work
REM lives and which you can read first. The wrapper exists because it has to: Windows marks
REM every file extracted from a downloaded zip as coming from the internet, and the default
REM PowerShell policy (RemoteSigned) refuses to run an unsigned script carrying that mark. So
REM right-clicking the .ps1 and choosing "Run with PowerShell" fails on a stock machine with
REM "is not digitally signed. You cannot run this script on the current system."
REM
REM -ExecutionPolicy Bypass applies to THIS ONE invocation only. It changes nothing on your
REM machine and does not persist: the policy is passed to a single powershell.exe process and
REM dies with it.

REM Double-clicking this from INSIDE the unextracted zip copies only this file to a temp folder,
REM so the script it calls is not beside it. Say what the .ps1 would have said, rather than
REM letting powershell.exe report a missing path.
if not exist "%~dp0Install-MoonLight.ps1" (
  echo Cannot find Install-MoonLight.ps1 beside this file.
  echo Extract the whole zip first, then run this from the extracted folder.
  echo.
  pause
  exit /b 1
)

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Install-MoonLight.ps1"
if errorlevel 1 (
  echo.
  echo Install failed. See the messages above.
)
echo.
pause
