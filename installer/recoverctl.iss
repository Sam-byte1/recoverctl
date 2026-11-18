; Inno Setup script for the Windows Data Recovery Tool (RecoverCtl GUI).
;
; G5b decision record (see docs/开发日志.md, 2026-11-17 packaging entry):
; this installer is scoped to ARM64 Windows only, same scope limit as the
; G5a green package (third_party/MANIFEST.md / docs/开发规划.md already
; record why: all vendored third-party libraries have only ever been
; cross-compiled and verified for the MSYS2 CLANGARM64 target -- x86_64 is
; a separate, unstarted porting effort, not something to silently claim
; support for here).
;
; Per-user install, no admin rights needed to RUN the installer itself
; (PrivilegesRequired=lowest, installs under the user's own Programs
; folder, writes only HKCU uninstall registry keys, no system-wide
; Program Files, no machine-wide Start Menu). This is intentionally
; decoupled from whether the *installed program* needs admin rights to
; do its job -- it does (see gui/app.manifest: recoverctl_gui.exe itself
; declares requestedExecutionLevel=requireAdministrator, because opening
; \\.\PhysicalDriveN / VSS shadow devices / BitLocker-locked raw volumes
; is an OS-level requirement no manifest or installer setting can avoid).
; Windows will show the UAC consent prompt when the user actually double
; -clicks the installed exe or its Start Menu/desktop shortcut, not when
; running this installer.

#define MyAppName "Windows数据恢复软件"
#define MyAppExeName "recoverctl_gui.exe"
#define MyAppVersion "0.2"
#define MyAppPublisher "Windows数据恢复软件项目"

[Setup]
AppId={{F46FB1DB-AFA0-4F1A-BEAC-0A3EC697029E}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={userpf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=no
PrivilegesRequired=lowest
ArchitecturesAllowed=arm64
ArchitecturesInstallIn64BitMode=arm64
OutputDir=C:\dev\installer_output
OutputBaseFilename=recoverctl-setup-win-arm64-v{#MyAppVersion}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayIcon={app}\{#MyAppExeName}

; No [Languages] section: ChineseSimplified.isl is an unofficial
; community translation not bundled with the base Inno Setup 6
; installer on this VM (confirmed by inspecting the Languages\ folder
; after a fresh install -- only the ~29 languages Inno ships by default
; are present, Simplified Chinese is not one of them). Pulling the
; translation file from jrsoftware's separate is-translations repo mid
; -build would add an external download dependency to the pipeline for
; a UI-chrome nicety (the wizard's own button/label text), not a
; functional requirement -- the product name, shortcut labels, and task
; descriptions below are already Chinese regardless of which Inno
; Setup UI language is active, since those come from this .iss file's
; own strings, not from the language pack. Default English wizard
; chrome is accepted here; revisit only if 世杰 asks for a fully
; localized installer wizard.

[Tasks]
Name: "desktopicon"; Description: "创建桌面快捷方式(&D)"; GroupDescription: "附加图标:"

[Files]
; Pulls in everything gui/build_windows.ps1's green-package step already
; staged (exe + all required DLLs + Qt plugin subfolders + photorec.exe +
; third_party/THIRD_PARTY_LICENSES.txt) -- same staging folder the G5a
; green zip is built from, so there is exactly one place that decides
; "what does a real deployment of this product consist of", not two
; diverging file lists to keep in sync by hand.
Source: "C:\dev\gui_package\recoverctl-gui-win-arm64\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\卸载 {#MyAppName}"; Filename: "{uninstallexe}"
Name: "{userdesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "启动 {#MyAppName}"; Flags: nowait postinstall skipifsilent
