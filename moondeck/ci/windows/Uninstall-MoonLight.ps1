# Remove MoonLight, keeping your settings.
#
# The companion to Install-MoonLight.ps1, and the target of the Start-menu "Uninstall MoonLight"
# entry. It removes the program and nothing else.
#
# Your settings are NOT touched. They live in %LOCALAPPDATA%\MoonLight, deliberately separate from
# the program, so a reinstall or an upgrade finds your layouts, effects and drivers exactly where
# you left them. Delete that folder by hand if you want a genuinely clean slate.

$ErrorActionPreference = "Stop"

$InstallTo = Join-Path $env:LOCALAPPDATA "Programs\MoonLight"
$StartMenu = Join-Path $env:APPDATA "Microsoft\Windows\Start Menu\Programs\MoonLight"
$RegKey    = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\MoonLight"
$Settings  = Join-Path $env:LOCALAPPDATA "MoonLight"

$running = Get-Process -Name MoonLight -ErrorAction SilentlyContinue
if ($running) {
    Write-Host "Stopping the running MoonLight..."
    $running | Stop-Process -Force
    Start-Sleep -Milliseconds 800
}

# This script runs FROM the directory it is about to delete, so the shell holds it open on
# Windows. Removing the contents and leaving the directory is the honest outcome: the program is
# gone, and an empty folder disappears on the next reboot or with one manual delete.
if (Test-Path $InstallTo) {
    Write-Host "Removing $InstallTo"
    Get-ChildItem $InstallTo -Force -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -ne "Uninstall-MoonLight.ps1" } |
        Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
}

# Stop here if the program is still there. Everything below removes the ways BACK to it: the
# Start-menu shortcut and the Add/Remove Programs entry are how a user would retry. Deleting those
# while the executable survives leaves them told it is gone, with no route to try again.
if (Test-Path (Join-Path $InstallTo "MoonLight.exe")) {
    Write-Host ""
    Write-Host "Could not remove MoonLight.exe. It is most likely still running." -ForegroundColor Red
    Write-Host "Close MoonLight and run this again. Nothing else has been changed."
    exit 1
}

if (Test-Path $StartMenu) {
    Write-Host "Removing the Start-menu entry"
    Remove-Item $StartMenu -Recurse -Force -ErrorAction SilentlyContinue
}

if (Test-Path $RegKey) {
    Write-Host "Removing the Add/Remove Programs entry"
    Remove-Item $RegKey -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host ""
Write-Host "MoonLight removed." -ForegroundColor Green
if (Test-Path $Settings) {
    Write-Host "Your settings are kept in $Settings. Delete that folder to start completely fresh."
}
Write-Host "One file remains, this script itself, because it is running from that folder. Delete $InstallTo when convenient."
