# x86_64 counterpart of installer/build_installer.ps1. Kept in plain
# ASCII for the same reason as that script (see its own header comment
# and AGENTS.md section 8).
#
# Depends on gui/build_windows_x64.ps1 having already produced
# C:\dev_x64\gui_package\recoverctl-gui-win-x64\ (the installer's [Files]
# section pulls straight from that staging folder).

$ISCC = "C:\Program Files (x86)\Inno Setup 6\ISCC.exe"
$ISS = "C:\dev\installer\recoverctl-x64.iss"

if (-not (Test-Path $ISCC)) {
    Write-Host "ERROR: Inno Setup ISCC.exe not found at $ISCC"
    exit 1
}
if (-not (Test-Path $ISS)) {
    Write-Host "ERROR: $ISS not found -- sync installer/recoverctl-x64.iss to C:\dev\installer\ first"
    exit 1
}
if (-not (Test-Path "C:\dev_x64\gui_package\recoverctl-gui-win-x64\recoverctl_gui.exe")) {
    Write-Host "ERROR: C:\dev_x64\gui_package\recoverctl-gui-win-x64\recoverctl_gui.exe not found -- run gui\build_windows_x64.ps1 first"
    exit 1
}

New-Item -ItemType Directory -Path "C:\dev_x64\installer_output" -Force | Out-Null

& $ISCC $ISS
$exitCode = $LASTEXITCODE

if ($exitCode -ne 0) {
    Write-Host "ISCC failed with exit code $exitCode"
    exit $exitCode
}

Write-Host "installer build succeeded"
Get-ChildItem "C:\dev_x64\installer_output" | Select-Object FullName, Length
