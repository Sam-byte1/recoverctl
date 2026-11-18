; Inno Setup script for the Windows Data Recovery Tool (RecoverCtl GUI),
; x86_64 build.
;
; x64 porting record (see docs/开发日志.md, 2026-11-18 entry): this is the
; x86_64 counterpart of recoverctl.iss (ARM64). Same per-user install
; policy, same admin-rights split (installer itself needs none; the
; installed exe declares requireAdministrator in its own manifest because
; opening \\.\PhysicalDriveN / VSS shadow devices / BitLocker-locked raw
; volumes is an OS-level requirement no installer setting can avoid).
;
; AppId is intentionally the SAME GUID as recoverctl.iss (both are the
; same product, just a different target architecture) -- this lets Inno
; Setup's uninstaller treat them as the same logical app for upgrade
; bookkeeping purposes, consistent with how a single product normally
; ships multiple architecture-specific installers.

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
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir=C:\dev_x64\installer_output
OutputBaseFilename=recoverctl-setup-win-x64-v{#MyAppVersion}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayIcon={app}\{#MyAppExeName}

; No [Languages] section -- same reasoning as recoverctl.iss (ARM64):
; ChineseSimplified.isl is not bundled with this VM's Inno Setup 6
; install, and the product-facing strings below (AppName/Icons/Tasks) are
; already Chinese regardless of wizard chrome language.

[Tasks]
Name: "desktopicon"; Description: "创建桌面快捷方式(&D)"; GroupDescription: "附加图标:"

[Files]
; Pulls in everything gui/build_windows_x64.ps1's green-package step
; already staged (exe + all required DLLs + Qt plugin subfolders +
; photorec.exe + third_party/THIRD_PARTY_LICENSES.txt) -- same single-
; source-of-truth staging folder pattern as the ARM64 installer.
Source: "C:\dev_x64\gui_package\recoverctl-gui-win-x64\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\卸载 {#MyAppName}"; Filename: "{uninstallexe}"
Name: "{userdesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "启动 {#MyAppName}"; Flags: nowait postinstall skipifsilent
