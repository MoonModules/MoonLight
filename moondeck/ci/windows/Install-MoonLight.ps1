# Install MoonLight for the current user.
#
# This does exactly what the setup.exe does, in plain text you can read before running it. That is
# the point of it existing: Microsoft Defender occasionally flags a freshly built, unsigned
# installer as malware on a machine-learning guess (Trojan:Win32/Wacatac!ml), and a blocked
# download leaves no file to rescue. A script gives its scoring model nothing to judge, and gives
# you something you can audit line by line instead of trusting.
#
# Run it by double-clicking Install-MoonLight.cmd beside this file, which is a wrapper around it.
# Right-clicking this script and choosing "Run with PowerShell" does NOT work on a stock machine:
# Windows marks everything extracted from a downloaded zip as internet-sourced, and the default
# RemoteSigned policy refuses to run an unsigned script carrying that mark. That is what the
# wrapper is for, and the only thing it adds is -ExecutionPolicy Bypass for its own invocation.
#
# No administrator rights are needed: everything below lives under your own user profile.

param(
    # Substituted by the packaging script. Left as the placeholder when run from a source
    # checkout, in which case the Add/Remove Programs entry simply carries no version.
    [string]$Version = "@VERSION@"
)

$ErrorActionPreference = "Stop"

$AppName   = "MoonLight"
$Source    = Join-Path $PSScriptRoot "MoonLight.exe"
$InstallTo = Join-Path $env:LOCALAPPDATA "Programs\MoonLight"
$StartMenu = Join-Path $env:APPDATA "Microsoft\Windows\Start Menu\Programs\MoonLight"
$RegKey    = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\MoonLight"

if (-not (Test-Path $Source)) {
    Write-Host "Cannot find MoonLight.exe beside this script." -ForegroundColor Red
    Write-Host "Extract the whole zip first, then run this from the extracted folder."
    exit 1
}

# A running copy holds a lock on its own executable, so the copy below would fail part-way.
# MoonLight is a local server you restart from the Start menu, so stopping it costs a reconnect.
$running = Get-Process -Name MoonLight -ErrorAction SilentlyContinue
if ($running) {
    Write-Host "Stopping the running MoonLight..."
    # SilentlyContinue because $ErrorActionPreference is Stop: Stop-Process raises on a process
    # that exited between the two calls, or one owned by another user, and neither is a reason to
    # abandon the install. A lock that genuinely survives fails loudly at the copy below instead.
    $running | Stop-Process -Force -ErrorAction SilentlyContinue
    # Wait for the exit rather than guessing at it. A fixed sleep is a race: too short and the copy
    # below throws with the app already killed, leaving nothing installed.
    $running | Wait-Process -Timeout 10 -ErrorAction SilentlyContinue
}

Write-Host "Installing to $InstallTo"
New-Item -ItemType Directory -Path $InstallTo -Force | Out-Null
$destExe = Join-Path $InstallTo "MoonLight.exe"
# Someone who extracted the zip straight into the install directory would otherwise copy the file
# onto itself, which throws under $ErrorActionPreference = "Stop" AFTER the running copy was
# stopped: app killed, nothing installed.
if ([IO.Path]::GetFullPath($Source) -ne [IO.Path]::GetFullPath($destExe)) {
    Copy-Item $Source $destExe -Force
} else {
    Write-Host "  already in the install directory, keeping it in place"
}
foreach ($extra in @("README.txt", "Uninstall-MoonLight.ps1")) {
    $p = Join-Path $PSScriptRoot $extra
    if (Test-Path $p) {
        $dest = Join-Path $InstallTo $extra
        Copy-Item $p $dest -Force
        # A copy inherits the "came from the internet" mark, and PowerShell's default policy
        # refuses to run a marked unsigned script. The Start-menu shortcut passes -ExecutionPolicy
        # Bypass so it works either way, but someone running the uninstaller by hand would hit
        # "is not digitally signed" for a file that came from their own install.
        Unblock-File -Path $dest -ErrorAction SilentlyContinue
    }
}

# The shortcut takes its icon straight from the executable, which carries its own.
Write-Host "Adding the Start-menu entry"
New-Item -ItemType Directory -Path $StartMenu -Force | Out-Null
$shell = New-Object -ComObject WScript.Shell
$lnk = $shell.CreateShortcut((Join-Path $StartMenu "MoonLight.lnk"))
$lnk.TargetPath       = Join-Path $InstallTo "MoonLight.exe"
$lnk.WorkingDirectory = $InstallTo
$lnk.Description      = "Drive large LED installations and DMX fixtures"
$lnk.Save()

$un = $shell.CreateShortcut((Join-Path $StartMenu "Uninstall MoonLight.lnk"))
$un.TargetPath       = "powershell.exe"
$un.Arguments        = "-NoProfile -ExecutionPolicy Bypass -File `"$InstallTo\Uninstall-MoonLight.ps1`""
$un.WorkingDirectory = $InstallTo
$un.Description      = "Remove MoonLight, keeping your settings"
$un.Save()

# HKCU rather than HKLM, to match the per-user install: an HKLM entry would need administrator
# rights and would advertise the app to users who do not have it.
Write-Host "Registering in Add/Remove Programs"
New-Item -Path $RegKey -Force | Out-Null
New-ItemProperty -Path $RegKey -Name "DisplayName"     -Value $AppName -PropertyType String -Force | Out-Null
New-ItemProperty -Path $RegKey -Name "Publisher"       -Value "MoonModules" -PropertyType String -Force | Out-Null
New-ItemProperty -Path $RegKey -Name "URLInfoAbout"    -Value "https://github.com/MoonModules/MoonLight" -PropertyType String -Force | Out-Null
New-ItemProperty -Path $RegKey -Name "DisplayIcon"     -Value (Join-Path $InstallTo "MoonLight.exe") -PropertyType String -Force | Out-Null
New-ItemProperty -Path $RegKey -Name "InstallLocation" -Value $InstallTo -PropertyType String -Force | Out-Null
New-ItemProperty -Path $RegKey -Name "UninstallString" -Value "powershell.exe -NoProfile -ExecutionPolicy Bypass -File `"$InstallTo\Uninstall-MoonLight.ps1`"" -PropertyType String -Force | Out-Null
New-ItemProperty -Path $RegKey -Name "NoModify"        -Value 1 -PropertyType DWord -Force | Out-Null
New-ItemProperty -Path $RegKey -Name "NoRepair"        -Value 1 -PropertyType DWord -Force | Out-Null
if ($Version -and $Version -notmatch "@") {
    New-ItemProperty -Path $RegKey -Name "DisplayVersion" -Value $Version -PropertyType String -Force | Out-Null
}

Write-Host ""
Write-Host "Done. MoonLight is in your Start menu." -ForegroundColor Green
Write-Host "Your settings live in $env:LOCALAPPDATA\MoonLight and are untouched by installing or removing the program."
