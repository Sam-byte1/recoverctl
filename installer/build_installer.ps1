# Kept in plain ASCII -- same reasoning as gui/build_windows.ps1 (see
# AGENTS.md section 8 and that script's own header comment): a .ps1 file
# with non-ASCII characters anywhere near the top, run non-interactively
# via "prlctl exec ... powershell -File <this file>", can silently do
# nothing or throw a parser error that points nowhere near the real
# problem. All Chinese-language decision history stays in the devlog
# (docs dir), not in this file.
#
# This script compiles installer/recoverctl.iss with Inno Setup's ISCC
# compiler (installed separately on the VM -- not a redistributed
# dependency of the product itself, just a build tool, same category as
# clang/MSYS2/windeployqt; see the devlog's 2026-11-17 packaging entry
# for why this does not need a third_party/MANIFEST.md entry).
#
# Depends on gui/build_windows.ps1 having already produced
# C:\dev\gui_package\recoverctl-gui-win-arm64\ (the installer's [Files]
# section pulls straight from that staging folder -- one source of truth
# for "what ships", not a second file list to keep in sync by hand).

$ISCC = "C:\Program Files (x86)\Inno Setup 6\ISCC.exe"
$ISS = "C:\dev\installer\recoverctl.iss"

if (-not (Test-Path $ISCC)) {
    Write-Host "ERROR: Inno Setup ISCC.exe not found at $ISCC"
    exit 1
}
if (-not (Test-Path $ISS)) {
    Write-Host "ERROR: $ISS not found -- sync installer/recoverctl.iss to C:\dev\installer\ first"
    exit 1
}
if (-not (Test-Path "C:\dev\gui_package\recoverctl-gui-win-arm64\recoverctl_gui.exe")) {
    Write-Host "ERROR: C:\dev\gui_package\recoverctl-gui-win-arm64\recoverctl_gui.exe not found -- run gui\build_windows.ps1 first"
    exit 1
}

New-Item -ItemType Directory -Path "C:\dev\installer_output" -Force | Out-Null

& $ISCC $ISS
$exitCode = $LASTEXITCODE

if ($exitCode -ne 0) {
    Write-Host "ISCC failed with exit code $exitCode"
    exit $exitCode
}

Write-Host "installer build succeeded"
Get-ChildItem "C:\dev\installer_output" | Select-Object FullName, Length
