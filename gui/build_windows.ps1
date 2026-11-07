# This file is kept entirely in plain ASCII, no Chinese characters at
# all -- not just inside the here-string, but anywhere in the file,
# including the explanatory comments below.
#
# Root-caused by direct testing on the real VM: a BOM-less .ps1 file
# that contains multi-byte UTF-8 characters (Chinese comments) anywhere
# near the top of the file, or apparently close enough to the here-string
# opener `@'`, makes "prlctl exec ... powershell -File <this file>"
# either (a) silently do nothing at all -- no error, no partial output,
# the remote command returns as if it ran instantly and succeeded, but
# none of the script's actual effects ever happen -- or (b) throw a
# PowerShell parser error pointing at lines that are clearly meant to be
# inside the here-string (as if the `@'` opener were not recognized at
# all). Toggled this back and forth several times while debugging this
# script: every version with Chinese text anywhere in the file exhibited
# one of these two failure modes; the all-ASCII version below has run
# cleanly and repeatably. cli/build_windows.ps1 has never hit this
# because, by coincidence, none of its Chinese comments happen to sit
# close enough to its own here-string boundaries to trigger it -- this
# is a narrower, more severe variant of the already-documented
# "PowerShell heredoc pipe corrupts Chinese text" issue (see AGENTS.md
# section 8), not a brand new unrelated bug, but the trigger condition
# turned out to be wider than "only content piped through bash" -- it
# can also corrupt the .ps1 file's own parsing before bash is ever
# involved. Given how much narrower "just never put Chinese anywhere in
# this file" is than fully characterizing the exact trigger, that is
# the policy here: this build script documents itself in English only.
# Full design rationale and decision history (G3 architecture, the
# ntfs_bfio_adapter.c / libbfio_handle_initialize C-vs-C++ linkage
# issue, etc.) stays in the devlog (docs dir) where Chinese is fine, not in
# this file.
#
# Depends on cli/build_windows.ps1 having been run at least once already
# -- this script reuses the ntfs_bfio_bridge.dll / libntfs_bfio_bridge.dll.a
# it builds (the Windows libfsntfs.dll does not export the internal
# libbfio_handle_initialize symbol; see cli/build_windows.ps1 for the
# full story), rather than rebuilding a second copy of it.
$env:MSYSTEM = "CLANGARM64"
$cmd = @'
set -e
LIBFSNTFS_DIR=/c/dev/libfsntfs
LIBVSHADOW_DIR=/c/dev/libvshadow
LIBBDE_DIR=/c/dev/libbde
ENGINE_DIR=/c/dev/engine
GUI_DIR=/c/dev/gui
CLI_DIR=/c/dev/cli
OUT=$GUI_DIR/recoverctl_gui.exe

if [ ! -f "$CLI_DIR/libntfs_bfio_bridge.dll.a" ]; then
  echo "ERROR: $CLI_DIR/libntfs_bfio_bridge.dll.a not found -- run cli/build_windows.ps1 first"
  exit 1
fi

export PKG_CONFIG_PATH=/clangarm64/lib/pkgconfig
QTFLAGS=$(pkg-config --cflags Qt6Widgets)
QTLIBS=$(pkg-config --libs Qt6Widgets)

# moc is just a code-generation step: given a header with Q_OBJECT, it
# produces a companion .cpp that gets compiled like any other source
# file. No CMake/qmake needed for that alone (see the devlog (docs dir)
# 2026-11-06 decision record for the full reasoning). moc.exe is NOT on
# the default MSYS2 PATH -- verified it actually lives under Qt6's own
# bin dir, not /clangarm64/bin (which only has a few top-level tools
# like windeployqt).
MOC=/clangarm64/share/qt6/bin/moc.exe
"$MOC" "$GUI_DIR/scan_worker.h" -o "$GUI_DIR/moc_scan_worker.cpp"
"$MOC" "$GUI_DIR/main_window.h" -o "$GUI_DIR/moc_main_window.cpp"
"$MOC" "$GUI_DIR/bitlocker_wizard_dialog.h" -o "$GUI_DIR/moc_bitlocker_wizard_dialog.cpp"
"$MOC" "$GUI_DIR/carve_worker.h" -o "$GUI_DIR/moc_carve_worker.cpp"
"$MOC" "$GUI_DIR/device_card.h" -o "$GUI_DIR/moc_device_card.cpp"
"$MOC" "$GUI_DIR/vss_page.h" -o "$GUI_DIR/moc_vss_page.cpp"
"$MOC" "$GUI_DIR/carve_page.h" -o "$GUI_DIR/moc_carve_page.cpp"
echo "moc done"

# NOTE: keep this source file list manually in sync with cli/build.sh and
# cli/build_windows.ps1 -- when adding a new engine/core or engine/shell
# source file, update all three scripts.
#
# Compiled in two separate passes (C++ GUI sources with clang++, C engine
# sources with plain clang), then linked together as object files --
# NOT fed in one single clang++ invocation. Root cause found by testing:
# clang++ defaults to compiling a bare .c file as C++ (it even warns
# "treating 'c' input as 'c++' in C++ mode"), which applies C++ name
# mangling to every symbol in that translation unit -- including the
# hand-declared "extern" libbfio_handle_initialize inside
# ntfs_bfio_adapter.c, which is NOT wrapped in extern "C" (it is a plain
# .c file, correctly assuming a C compiler per the project's normal
# build scripts). The resulting .o then looks for a mangled symbol name
# that does not exist in libntfs_bfio_bridge.dll.a (built with a plain C
# compiler, exporting the real unmangled C name), and the link fails
# with "undefined symbol: libbfio_handle_initialize(...)". A first fix
# attempt tried "-x c" to force clang++ to treat the .c inputs as C
# while staying in one invocation, but that collides with the single
# global "-std=c++17" flag ("invalid argument '-std=c++17' not allowed
# with 'C'") -- a C translation unit and a C++ translation unit need
# different -std values, which a single clang++ command line can't
# express per-file. Separate compile invocations (each with its own
# correct -std) sidesteps this entirely and matches how these same .c
# files are already compiled everywhere else in this project (plain
# clang -std=c11, never clang++), rather than inventing a GUI-specific
# compilation mode for them.
OBJ_DIR=$GUI_DIR/obj
mkdir -p "$OBJ_DIR"

CPP_SOURCES="$GUI_DIR/main.cpp $GUI_DIR/scan_worker.cpp $GUI_DIR/main_window.cpp $GUI_DIR/bitlocker_wizard_dialog.cpp $GUI_DIR/carve_worker.cpp $GUI_DIR/device_card.cpp $GUI_DIR/vss_page.cpp $GUI_DIR/carve_page.cpp $GUI_DIR/icon_painter.cpp $GUI_DIR/moc_scan_worker.cpp $GUI_DIR/moc_main_window.cpp $GUI_DIR/moc_bitlocker_wizard_dialog.cpp $GUI_DIR/moc_carve_worker.cpp $GUI_DIR/moc_device_card.cpp $GUI_DIR/moc_vss_page.cpp $GUI_DIR/moc_carve_page.cpp"
C_SOURCES="$ENGINE_DIR/core/undelete_detector.c $ENGINE_DIR/core/undelete_scanner.c $ENGINE_DIR/core/ntfs_file_reference.c $ENGINE_DIR/core/path_builder.c $ENGINE_DIR/core/sector_aligned_read.c $ENGINE_DIR/core/utf16_to_utf8.c $ENGINE_DIR/core/fat_boot_sector.c $ENGINE_DIR/core/fat_dir_entry.c $ENGINE_DIR/core/fat_undelete_scanner.c $ENGINE_DIR/core/exfat_boot_sector.c $ENGINE_DIR/core/exfat_dir_entry.c $ENGINE_DIR/core/exfat_undelete_scanner.c $ENGINE_DIR/core/exfat_fat_chain.c $ENGINE_DIR/core/fat_fat_chain.c $ENGINE_DIR/core/bad_sector_policy.c $ENGINE_DIR/core/bad_sector_log.c $ENGINE_DIR/core/bitlocker_credential.c $ENGINE_DIR/core/hidden_input_core.c $ENGINE_DIR/core/photorec_report_parser.c $ENGINE_DIR/core/photorec_command_builder.c $ENGINE_DIR/shell/aligned_device_reader.c $ENGINE_DIR/shell/partition_extractor.c $ENGINE_DIR/shell/file_writer.c $ENGINE_DIR/shell/ntfs_bfio_adapter.c $ENGINE_DIR/shell/vshadow_bfio_adapter.c $ENGINE_DIR/shell/secure_memory.c $ENGINE_DIR/shell/hidden_input_reader.c $ENGINE_DIR/shell/bitlocker_credential_source.c $ENGINE_DIR/shell/bitlocker_unlock.c $ENGINE_DIR/shell/bitlocker_bfio_adapter.c $ENGINE_DIR/shell/photorec_invoker.c $ENGINE_DIR/shell/path_resolver.c $ENGINE_DIR/shell/disk_enumerator.c $ENGINE_DIR/shell/bitlocker_status_checker.c $ENGINE_DIR/shell/volume_disk_extents.c $ENGINE_DIR/shell/fat_reader.c $ENGINE_DIR/shell/exfat_reader.c"

OBJS=""
for src in $CPP_SOURCES; do
  obj="$OBJ_DIR/$(basename "$src").o"
  # -Wno-ignored-attributes: Qt headers redeclare a bunch of dllimport'd
  # inline functions, which triggers a wall of "redeclared inline;
  # 'dllimport' attribute ignored" warnings on every single file that
  # includes QtWidgets/QtGui/QtCore -- harmless (same pattern G1 already
  # saw and confirmed benign), just noise that drowns out real warnings.
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

clang++ $OBJS \
  -L "$LIBFSNTFS_DIR/libfsntfs/.libs" \
  -lfsntfs \
  -L "$LIBVSHADOW_DIR/libvshadow/.libs" \
  -lvshadow \
  -L "$LIBBDE_DIR/libbde/.libs" \
  -lbde \
  -L "$CLI_DIR" \
  -lntfs_bfio_bridge \
  -L /clangarm64/lib \
  -lmpr \
  $QTLIBS \
  -o "$OUT"

echo "build succeeded: $OUT"
cp "$LIBFSNTFS_DIR/libfsntfs/.libs/libfsntfs-1.dll" "$GUI_DIR/"
cp "$LIBVSHADOW_DIR/libvshadow/.libs/libvshadow-1.dll" "$GUI_DIR/"
cp "$LIBBDE_DIR/libbde/.libs/libbde-1.dll" "$GUI_DIR/"
# libbde links against winpthread (see the devlog (docs dir) B2 entry for
# why, same reason cli/build_windows.ps1 ships this DLL) -- only lives
# inside the MSYS2 install tree, not on a normal Windows PATH, so it must
# ship alongside the other DLLs.
cp /clangarm64/bin/libwinpthread-1.dll "$GUI_DIR/"
cp "$CLI_DIR/ntfs_bfio_bridge.dll" "$GUI_DIR/"

# G8 (2026-11-06 decision record): photorec.exe is built once from the
# vendored testdisk source by cli/build_windows.ps1 (a multi-minute
# from-source compile, not something this script should repeat) -- reuse
# that already-built binary plus its one non-system DLL dependency and the
# terminfo database it needs at runtime (see photorec_invoker.c's Windows
# branch comments for why TERMINFO has to point at a Windows-reachable
# path). Depends on cli/build_windows.ps1 having been run at least once,
# same dependency style as the ntfs_bfio_bridge.dll reuse above.
if [ -f "$CLI_DIR/photorec.exe" ]; then
  cp "$CLI_DIR/photorec.exe" "$GUI_DIR/"
  cp "$CLI_DIR/libncursesw6.dll" "$GUI_DIR/"
  rm -rf "$GUI_DIR/terminfo"
  cp -r "$CLI_DIR/terminfo" "$GUI_DIR/terminfo"
  echo "photorec.exe + libncursesw6.dll + terminfo/ reused from cli build"
else
  echo "WARNING: $CLI_DIR/photorec.exe not found -- run cli/build_windows.ps1 first, PhotoRec carving will not work in this GUI build"
fi

# Remove stale artifacts from earlier builds that ran without
# --skip-plugin-types generic below -- windeployqt only adds/updates
# files, it never removes ones that used to be needed but no longer are,
# so a Qt6Network.dll left over from a previous build would otherwise
# silently stick around even after this script stops asking for it.
rm -rf "$GUI_DIR/generic" "$GUI_DIR/networkinformation" "$GUI_DIR/tls" "$GUI_DIR/Qt6Network.dll"

# --skip-plugin-types generic: this app does not need the "generic"
# plugin category (touchscreen/tablet input plugins etc.), and skipping
# it also drops windeployqt's transitive pull of Qt6Network.dll (and
# whatever it links, e.g. openssl) -- confirmed by testing: without this
# flag, windeployqt logs "Adding Qt6Network for qtuiotouchplugin.dll from
# plugin type: generic" even though this app's own code only links
# Qt6Widgets. Fewer deployed DLLs means a smaller license-compliance
# surface to check before any real distribution (see the devlog (docs
# dir) G5 decision record and third_party/MANIFEST.md).
windeployqt --skip-plugin-types generic "$OUT" 2>&1 || echo "windeployqt failed or not found"

# windeployqt only deploys Qt's own modules/plugins, not the MSYS2
# toolchain runtime libs Qt itself depends on (libc++/libicu*/libharfbuzz
# etc. -- G1 already hit this, see the devlog (docs dir) G1 record).
#
# 2026-11-06 (real-device bug report: JPEG thumbnail preview silently
# "not supported" for every single file): root-caused via
# QT_DEBUG_PLUGINS=1 to qjpeg.dll failing to LoadLibrary with "module
# not found" at runtime, even though it was present on disk and Qt could
# read its plugin metadata fine. `objdump -p qjpeg.dll` showed a real PE
# import-table dependency on libjpeg-8.dll -- but the original loop here
# only ever scanned $OUT (the main exe) via `ldd`, never the plugin DLLs
# windeployqt drops into imageformats/platforms/styles/, so this
# transitive-only-through-a-lazily-loaded-plugin dependency was never
# discovered or copied.
#
# A first fix attempt swapped `ldd` out for `objdump -p` entirely (direct
# testing had found `ldd qjpeg.dll` genuinely omits libjpeg-8.dll from its
# output on this cross-compiled ARM64 target, even with the dependency
# manually placed right next to it -- not a fluke, reproduced twice).
# That broke a different real thing: a real-device launch test afterwards
# failed with STATUS_DLL_NOT_FOUND, and only 1 dependency (libjpeg-8.dll)
# got copied where ~20 used to (libpng16-16.dll, libharfbuzz-0.dll,
# zlib1.dll, etc.) -- because `ldd "$OUT"` was quietly doing full
# *transitive* resolution in one call (walking multiple dependency-of-
# dependency levels for the one file it was given), while `objdump -p`
# only reads one file's direct PE import table with no recursion, and the
# loop was never re-run against newly-copied files to pick up *their*
# dependencies either.
#
# Final approach, revised after a real-device timing problem: the first
# "union both tools, loop every .dll in $GUI_DIR to a fixed point" version
# was *correct* but devastatingly slow on real hardware -- it re-ran `ldd`
# AND `objdump -p` against every single .dll already sitting in $GUI_DIR
# (by this point ~25 files, including the multi-megabyte Qt6Core/Gui/
# Widgets.dll) on *every single pass*, and this toolchain's `ldd`/`objdump`
# run under ARM64-on-ARM64-Windows-emulation-of-x86_64-tools overhead
# (xtajit64se.dll shows up in every ldd invocation's own dependency list)
# -- a real build sat for 16+ minutes making zero forward progress before
# this was caught and killed.
#
# Bounded rewrite: `ldd "$OUT"` alone reliably does full transitive
# resolution for everything $OUT itself needs (confirmed across every
# earlier successful build this session -- this part was never broken,
# the bug is specifically that it can't see what a *lazily-loaded Qt
# plugin* needs). So: run that exactly once (cheap, proven), then scan
# ONLY the handful of small plugin files in imageformats/platforms/styles/
# with the ldd+objdump union (this is the actual fix for the qjpeg.dll/
# libjpeg-8.dll blind spot), then do exactly one bounded extra `ldd` pass
# over whatever that step freshly copied (to catch a dependency-of-a-
# dependency one level deep, without re-scanning the whole directory
# again). No loop that revisits already-resolved large files.
for dep in $(ldd "$OUT" | grep '=>' | awk '{print $1}'); do
  if [ -f "/clangarm64/bin/$dep" ] && [ ! -f "$GUI_DIR/$dep" ]; then
    cp "/clangarm64/bin/$dep" "$GUI_DIR/"
    echo "copied runtime dep: $dep (needed by $(basename "$OUT"))"
  fi
done

newly_copied_plugin_deps=""
for dll in "$GUI_DIR"/imageformats/*.dll "$GUI_DIR"/platforms/*.dll "$GUI_DIR"/styles/*.dll; do
  [ -f "$dll" ] || continue
  deps=$( { ldd "$dll" 2>/dev/null | grep '=>' | awk '{print $1}'; objdump -p "$dll" 2>/dev/null | grep "DLL Name:" | awk '{print $3}'; } | sort -u )
  for dep in $deps; do
    if [ -f "/clangarm64/bin/$dep" ] && [ ! -f "$GUI_DIR/$dep" ]; then
      cp "/clangarm64/bin/$dep" "$GUI_DIR/"
      echo "copied runtime dep: $dep (needed by $(basename "$dll"))"
      newly_copied_plugin_deps="$newly_copied_plugin_deps $GUI_DIR/$dep"
    fi
  done
done
for dll in $newly_copied_plugin_deps; do
  for dep in $(ldd "$dll" 2>/dev/null | grep '=>' | awk '{print $1}'); do
    if [ -f "/clangarm64/bin/$dep" ] && [ ! -f "$GUI_DIR/$dep" ]; then
      cp "/clangarm64/bin/$dep" "$GUI_DIR/"
      echo "copied runtime dep: $dep (needed by $(basename "$dll"))"
    fi
  done
done

ls -la "$GUI_DIR/"
'@
# Writing to a log file first and then re-emitting it, instead of just
# piping straight through to the console, is a deliberate difference
# from cli/build_windows.ps1 (which pipes directly) -- confirmed by
# direct testing that piping this particular script's (longer, heavier)
# output straight through "prlctl exec" sometimes silently returns
# before the remote command has actually produced any output at all
# (no error, no partial output, nothing -- repeatable across several
# bare re-runs), while routing through a file on disk and reading it
# back has been reliable every time tried. Not fully root-caused (did
# not chase it further than confirming the workaround holds up), but
# worth keeping this note so a future session does not waste time
# rediscovering the same flakiness from scratch.
$cmd | & "C:\msys64\usr\bin\bash.exe" --login *> "C:\dev\gui\build_log.txt"
Get-Content "C:\dev\gui\build_log.txt"

# G5a Task 2 (2026-11-06 decision record): package a "green" distribution
# -- a folder/zip the user can copy or unzip and run directly, no
# installer, no registry writes, no Program Files -- deferred the real
# installer (G5b, Inno Setup) to later. This step only runs after the
# license check above (G5a Task 1, see third_party/MANIFEST.md) already
# confirmed every DLL here is safe to redistribute as-is for internal
# verification use.
#
# Copies only what the exe actually needs to run (itself, the DLLs next
# to it, and the Qt plugin subdirectories windeployqt created) into a
# clean staging folder -- NOT the whole gui/ source directory, which
# also has .cpp/.h source files, the obj/ intermediate build directory,
# this very .ps1 script, and leftover log files from previous runs. Then
# zips that staging folder with PowerShell's built-in Compress-Archive,
# no extra tool needed.
$packageName = "recoverctl-gui-win-arm64"
$packageDir = "C:\dev\gui_package\$packageName"
if (Test-Path "C:\dev\gui_package") {
    Remove-Item "C:\dev\gui_package" -Recurse -Force
}
New-Item -ItemType Directory -Path $packageDir -Force | Out-Null

Copy-Item "C:\dev\gui\recoverctl_gui.exe" $packageDir
Get-ChildItem "C:\dev\gui\*.dll" | Copy-Item -Destination $packageDir
if (Test-Path "C:\dev\gui\photorec.exe") {
    Copy-Item "C:\dev\gui\photorec.exe" $packageDir
}
foreach ($pluginDir in @("imageformats", "platforms", "styles", "terminfo")) {
    $src = "C:\dev\gui\$pluginDir"
    if (Test-Path $src) {
        Copy-Item $src -Destination "$packageDir\$pluginDir" -Recurse
    }
}

$zipPath = "C:\dev\gui_package\$packageName.zip"
if (Test-Path $zipPath) {
    Remove-Item $zipPath -Force
}
Compress-Archive -Path $packageDir -DestinationPath $zipPath

Write-Host "green package built: $zipPath"
Get-ChildItem $packageDir -Recurse | Select-Object FullName,Length
