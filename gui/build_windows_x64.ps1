# x86_64 counterpart of gui/build_windows.ps1 (CLANGARM64/ARM64). Kept in
# plain ASCII for the same reason as that script (see its own header
# comment, and AGENTS.md section 8): a .ps1 file with non-ASCII characters
# anywhere near the top, run non-interactively via
# "prlctl exec ... powershell -File <this file>", can silently do nothing
# or throw a parser error that points nowhere near the real problem.
#
# Depends on cli/build_windows_x64.ps1 having been run at least once
# already (reuses the ntfs_bfio_bridge.dll / libntfs_bfio_bridge.dll.a it
# builds under C:\dev_x64\cli, and photorec.exe + its runtime deps), and
# on the x86_64 third-party libraries having already been configured and
# built under C:\dev_x64\<libname> (libfsntfs/libvshadow/libbde -- see
# the devlog (docs dir) for the one-time build commands used).
$env:MSYSTEM = "CLANG64"
$cmd = @'
set -e
LIBFSNTFS_DIR=/c/dev_x64/libfsntfs
LIBVSHADOW_DIR=/c/dev_x64/libvshadow
LIBBDE_DIR=/c/dev_x64/libbde
ENGINE_DIR=/c/dev/engine
GUI_DIR=/c/dev_x64/gui
CLI_DIR=/c/dev_x64/cli
OUT=$GUI_DIR/recoverctl_gui.exe

if [ ! -f "$CLI_DIR/libntfs_bfio_bridge.dll.a" ]; then
  echo "ERROR: $CLI_DIR/libntfs_bfio_bridge.dll.a not found -- run cli/build_windows_x64.ps1 first"
  exit 1
fi

export PKG_CONFIG_PATH=/clang64/lib/pkgconfig
QTFLAGS=$(pkg-config --cflags Qt6Widgets)
QTLIBS=$(pkg-config --libs Qt6Widgets)

MOC=/clang64/share/qt6/bin/moc.exe
"$MOC" "$GUI_DIR/scan_worker.h" -o "$GUI_DIR/moc_scan_worker.cpp"
"$MOC" "$GUI_DIR/main_window.h" -o "$GUI_DIR/moc_main_window.cpp"
"$MOC" "$GUI_DIR/bitlocker_wizard_dialog.h" -o "$GUI_DIR/moc_bitlocker_wizard_dialog.cpp"
"$MOC" "$GUI_DIR/carve_worker.h" -o "$GUI_DIR/moc_carve_worker.cpp"
"$MOC" "$GUI_DIR/device_card.h" -o "$GUI_DIR/moc_device_card.cpp"
"$MOC" "$GUI_DIR/vss_page.h" -o "$GUI_DIR/moc_vss_page.cpp"
"$MOC" "$GUI_DIR/carve_page.h" -o "$GUI_DIR/moc_carve_page.cpp"
echo "moc done"

# NOTE: keep this source file list manually in sync with cli/build.sh,
# cli/build_windows.ps1, cli/build_windows_x64.ps1 and
# gui/build_windows.ps1 -- when adding a new engine/core or engine/shell
# source file, update all of them.
OBJ_DIR=$GUI_DIR/obj
mkdir -p "$OBJ_DIR"

CPP_SOURCES="$GUI_DIR/main.cpp $GUI_DIR/scan_worker.cpp $GUI_DIR/main_window.cpp $GUI_DIR/bitlocker_wizard_dialog.cpp $GUI_DIR/carve_worker.cpp $GUI_DIR/device_card.cpp $GUI_DIR/vss_page.cpp $GUI_DIR/carve_page.cpp $GUI_DIR/icon_painter.cpp $GUI_DIR/moc_scan_worker.cpp $GUI_DIR/moc_main_window.cpp $GUI_DIR/moc_bitlocker_wizard_dialog.cpp $GUI_DIR/moc_carve_worker.cpp $GUI_DIR/moc_device_card.cpp $GUI_DIR/moc_vss_page.cpp $GUI_DIR/moc_carve_page.cpp"
C_SOURCES="$ENGINE_DIR/core/undelete_detector.c $ENGINE_DIR/core/undelete_scanner.c $ENGINE_DIR/core/ntfs_file_reference.c $ENGINE_DIR/core/path_builder.c $ENGINE_DIR/core/sector_aligned_read.c $ENGINE_DIR/core/utf16_to_utf8.c $ENGINE_DIR/core/fat_boot_sector.c $ENGINE_DIR/core/fat_dir_entry.c $ENGINE_DIR/core/fat_undelete_scanner.c $ENGINE_DIR/core/exfat_boot_sector.c $ENGINE_DIR/core/exfat_dir_entry.c $ENGINE_DIR/core/exfat_undelete_scanner.c $ENGINE_DIR/core/exfat_fat_chain.c $ENGINE_DIR/core/fat_fat_chain.c $ENGINE_DIR/core/bad_sector_policy.c $ENGINE_DIR/core/bad_sector_log.c $ENGINE_DIR/core/bitlocker_credential.c $ENGINE_DIR/core/hidden_input_core.c $ENGINE_DIR/core/photorec_report_parser.c $ENGINE_DIR/core/photorec_command_builder.c $ENGINE_DIR/shell/aligned_device_reader.c $ENGINE_DIR/shell/partition_extractor.c $ENGINE_DIR/shell/file_writer.c $ENGINE_DIR/shell/ntfs_bfio_adapter.c $ENGINE_DIR/shell/vshadow_bfio_adapter.c $ENGINE_DIR/shell/secure_memory.c $ENGINE_DIR/shell/hidden_input_reader.c $ENGINE_DIR/shell/bitlocker_credential_source.c $ENGINE_DIR/shell/bitlocker_unlock.c $ENGINE_DIR/shell/bitlocker_bfio_adapter.c $ENGINE_DIR/shell/photorec_invoker.c $ENGINE_DIR/shell/path_resolver.c $ENGINE_DIR/shell/disk_enumerator.c $ENGINE_DIR/shell/bitlocker_status_checker.c $ENGINE_DIR/shell/volume_disk_extents.c $ENGINE_DIR/shell/fat_reader.c $ENGINE_DIR/shell/exfat_reader.c"

OBJS=""
for src in $CPP_SOURCES; do
  obj="$OBJ_DIR/$(basename "$src").o"
  clang++ -std=c++17 -Wno-ignored-attributes $QTFLAGS -I "$LIBFSNTFS_DIR/include" -I "$LIBFSNTFS_DIR/common" -I "$LIBVSHADOW_DIR/include" -I "$LIBVSHADOW_DIR/common" -I "$LIBBDE_DIR/include" -I "$LIBBDE_DIR/common" -I "$ENGINE_DIR/core" -c "$src" -o "$obj"
  OBJS="$OBJS $obj"
done
echo "cpp compile done"
for src in $C_SOURCES; do
  obj="$OBJ_DIR/$(basename "$src").o"
  clang -std=c11 -I "$LIBFSNTFS_DIR/include" -I "$LIBFSNTFS_DIR/common" -I "$LIBVSHADOW_DIR/include" -I "$LIBVSHADOW_DIR/common" -I "$LIBBDE_DIR/include" -I "$LIBBDE_DIR/common" -I "$ENGINE_DIR/core" -c "$src" -o "$obj"
  OBJS="$OBJS $obj"
done
echo "c compile done"

# Same manifest embedding as the ARM64 build (requestedExecutionLevel=
# requireAdministrator) -- gui/app.rc/app.manifest are architecture
# agnostic declarative resources, windres picks the right COFF target
# from the active toolchain automatically.
/clang64/bin/windres.exe "$GUI_DIR/app.rc" -O coff -o "$OBJ_DIR/app_manifest.o"
echo "manifest resource compiled"

clang++ $OBJS "$OBJ_DIR/app_manifest.o" \
  -L "$LIBFSNTFS_DIR/libfsntfs/.libs" \
  -lfsntfs \
  -L "$LIBVSHADOW_DIR/libvshadow/.libs" \
  -lvshadow \
  -L "$LIBBDE_DIR/libbde/.libs" \
  -lbde \
  -L "$CLI_DIR" \
  -lntfs_bfio_bridge \
  -L /clang64/lib \
  -lmpr \
  $QTLIBS \
  -o "$OUT"

echo "build succeeded: $OUT"
cp "$LIBFSNTFS_DIR/libfsntfs/.libs/libfsntfs-1.dll" "$GUI_DIR/"
cp "$LIBVSHADOW_DIR/libvshadow/.libs/libvshadow-1.dll" "$GUI_DIR/"
cp "$LIBBDE_DIR/libbde/.libs/libbde-1.dll" "$GUI_DIR/"
cp /clang64/bin/libwinpthread-1.dll "$GUI_DIR/"
cp "$CLI_DIR/ntfs_bfio_bridge.dll" "$GUI_DIR/"

if [ -f "$CLI_DIR/photorec.exe" ]; then
  cp "$CLI_DIR/photorec.exe" "$GUI_DIR/"
  cp "$CLI_DIR/libncursesw6.dll" "$GUI_DIR/"
  rm -rf "$GUI_DIR/terminfo"
  cp -r "$CLI_DIR/terminfo" "$GUI_DIR/terminfo"
  echo "photorec.exe + libncursesw6.dll + terminfo/ reused from cli build"
else
  echo "WARNING: $CLI_DIR/photorec.exe not found -- run cli/build_windows_x64.ps1 first, PhotoRec carving will not work in this GUI build"
fi

rm -rf "$GUI_DIR/generic" "$GUI_DIR/networkinformation" "$GUI_DIR/tls" "$GUI_DIR/Qt6Network.dll"

windeployqt --skip-plugin-types generic "$OUT" 2>&1 || echo "windeployqt failed or not found"

for dep in $(ldd "$OUT" | grep '=>' | awk '{print $1}'); do
  if [ -f "/clang64/bin/$dep" ] && [ ! -f "$GUI_DIR/$dep" ]; then
    cp "/clang64/bin/$dep" "$GUI_DIR/"
    echo "copied runtime dep: $dep (needed by $(basename "$OUT"))"
  fi
done

newly_copied_plugin_deps=""
for dll in "$GUI_DIR"/imageformats/*.dll "$GUI_DIR"/platforms/*.dll "$GUI_DIR"/styles/*.dll; do
  [ -f "$dll" ] || continue
  deps=$( { ldd "$dll" 2>/dev/null | grep '=>' | awk '{print $1}'; objdump -p "$dll" 2>/dev/null | grep "DLL Name:" | awk '{print $3}'; } | sort -u )
  for dep in $deps; do
    if [ -f "/clang64/bin/$dep" ] && [ ! -f "$GUI_DIR/$dep" ]; then
      cp "/clang64/bin/$dep" "$GUI_DIR/"
      echo "copied runtime dep: $dep (needed by $(basename "$dll"))"
      newly_copied_plugin_deps="$newly_copied_plugin_deps $GUI_DIR/$dep"
    fi
  done
done
for dll in $newly_copied_plugin_deps; do
  for dep in $(ldd "$dll" 2>/dev/null | grep '=>' | awk '{print $1}'); do
    if [ -f "/clang64/bin/$dep" ] && [ ! -f "$GUI_DIR/$dep" ]; then
      cp "/clang64/bin/$dep" "$GUI_DIR/"
      echo "copied runtime dep: $dep (needed by $(basename "$dll"))"
    fi
  done
done

ls -la "$GUI_DIR/"
'@
$cmd | & "C:\msys64\usr\bin\bash.exe" --login *> "C:\dev_x64\gui\build_log.txt"
Get-Content "C:\dev_x64\gui\build_log.txt"

# Same green-package staging step as gui/build_windows.ps1, x86_64 naming.
$packageName = "recoverctl-gui-win-x64"
$packageDir = "C:\dev_x64\gui_package\$packageName"
if (Test-Path "C:\dev_x64\gui_package") {
    Remove-Item "C:\dev_x64\gui_package" -Recurse -Force
}
New-Item -ItemType Directory -Path $packageDir -Force | Out-Null

Copy-Item "C:\dev_x64\gui\recoverctl_gui.exe" $packageDir
Get-ChildItem "C:\dev_x64\gui\*.dll" | Copy-Item -Destination $packageDir
if (Test-Path "C:\dev_x64\gui\photorec.exe") {
    Copy-Item "C:\dev_x64\gui\photorec.exe" $packageDir
}

if (Test-Path "\\Mac\devproject\third_party\THIRD_PARTY_LICENSES.txt") {
    Copy-Item "\\Mac\devproject\third_party\THIRD_PARTY_LICENSES.txt" $packageDir
} elseif (Test-Path "C:\dev\THIRD_PARTY_LICENSES.txt") {
    Copy-Item "C:\dev\THIRD_PARTY_LICENSES.txt" $packageDir
} else {
    Write-Host "WARNING: THIRD_PARTY_LICENSES.txt not found -- real distribution package is missing a required license disclosure file"
}
foreach ($pluginDir in @("imageformats", "platforms", "styles", "terminfo")) {
    $src = "C:\dev_x64\gui\$pluginDir"
    if (Test-Path $src) {
        Copy-Item $src -Destination "$packageDir\$pluginDir" -Recurse
    }
}

$zipPath = "C:\dev_x64\gui_package\$packageName.zip"
if (Test-Path $zipPath) {
    Remove-Item $zipPath -Force
}
Compress-Archive -Path $packageDir -DestinationPath $zipPath

Write-Host "green package built: $zipPath"
Get-ChildItem $packageDir -Recurse | Select-Object FullName,Length
