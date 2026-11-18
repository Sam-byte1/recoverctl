$env:MSYSTEM = "CLANG64"
$cmd = @'
set -e
LIBFSNTFS_DIR=/c/dev_x64/libfsntfs
LIBVSGPT_DIR=/c/dev_x64/libvsgpt
LIBVSMBR_DIR=/c/dev_x64/libvsmbr
LIBVSHADOW_DIR=/c/dev_x64/libvshadow
LIBBDE_DIR=/c/dev_x64/libbde
ENGINE_DIR=/c/dev/engine
OUT=/c/dev_x64/cli/recoverctl.exe
BRIDGE_DLL=/c/dev_x64/cli/ntfs_bfio_bridge.dll
BRIDGE_IMPLIB=/c/dev_x64/cli/libntfs_bfio_bridge.dll.a

mkdir -p /c/dev_x64/cli

# x86_64 counterpart of cli/build_windows.ps1 (CLANGARM64). Same reasoning
# applies verbatim: the Windows libfsntfs DLL does not export
# libbfio_handle_initialize/free (internal symbols, unlike the Mac dylib),
# so a small bridge DLL is built from the same source files, with the same
# config.h, to keep struct layout identical across the DLL boundary.
clang -std=c11 -shared \
  -DHAVE_CONFIG_H=1 \
  -I "$LIBFSNTFS_DIR/include" \
  -I "$LIBFSNTFS_DIR/common" \
  -I "$LIBFSNTFS_DIR/libbfio" \
  -I "$LIBFSNTFS_DIR/libcdata" \
  -I "$LIBFSNTFS_DIR/libcerror" \
  "$LIBFSNTFS_DIR/libbfio/libbfio_handle.c" \
  "$LIBFSNTFS_DIR/libcdata/libcdata_range_list.c" \
  "$LIBFSNTFS_DIR/libcdata/libcdata_range_list_value.c" \
  "$LIBFSNTFS_DIR/libcdata/libcdata_list_element.c" \
  "$LIBFSNTFS_DIR/libcerror/libcerror_error.c" \
  -Wl,--export-all-symbols \
  -Wl,--out-implib="$BRIDGE_IMPLIB" \
  -o "$BRIDGE_DLL"

echo "bridge DLL built: $BRIDGE_DLL"

# NOTE: this source file list must be kept in sync with ../cli/build.sh and
# ../cli/build_windows.ps1 by hand (no shared generation step exists yet).
# When adding a new engine/core or engine/shell source file, add it to all
# three scripts.
clang -std=c11 \
  -I "$LIBFSNTFS_DIR/include" \
  -I "$LIBFSNTFS_DIR/common" \
  -I "$LIBVSGPT_DIR/include" \
  -I "$LIBVSGPT_DIR/common" \
  -I "$LIBVSMBR_DIR/include" \
  -I "$LIBVSMBR_DIR/common" \
  -I "$LIBVSHADOW_DIR/include" \
  -I "$LIBVSHADOW_DIR/common" \
  -I "$LIBBDE_DIR/include" \
  -I "$LIBBDE_DIR/common" \
  -I "$ENGINE_DIR/core" \
  /c/dev_x64/cli/main.c \
  "$ENGINE_DIR/core/undelete_detector.c" \
  "$ENGINE_DIR/core/undelete_scanner.c" \
  "$ENGINE_DIR/core/ads_enumerator.c" \
  "$ENGINE_DIR/core/ntfs_file_reference.c" \
  "$ENGINE_DIR/core/path_builder.c" \
  "$ENGINE_DIR/core/sector_aligned_read.c" \
  "$ENGINE_DIR/core/utf16_to_utf8.c" \
  "$ENGINE_DIR/core/fat_boot_sector.c" \
  "$ENGINE_DIR/core/fat_dir_entry.c" \
  "$ENGINE_DIR/core/fat_undelete_scanner.c" \
  "$ENGINE_DIR/core/exfat_boot_sector.c" \
  "$ENGINE_DIR/core/exfat_dir_entry.c" \
  "$ENGINE_DIR/core/exfat_undelete_scanner.c" \
  "$ENGINE_DIR/core/exfat_fat_chain.c" \
  "$ENGINE_DIR/core/fat_fat_chain.c" \
  "$ENGINE_DIR/core/bad_sector_policy.c" \
  "$ENGINE_DIR/core/bad_sector_log.c" \
  "$ENGINE_DIR/core/photorec_report_parser.c" \
  "$ENGINE_DIR/core/photorec_command_builder.c" \
  "$ENGINE_DIR/shell/photorec_invoker.c" \
  "$ENGINE_DIR/core/bitlocker_key_protector_type_name.c" \
  "$ENGINE_DIR/core/bitlocker_credential.c" \
  "$ENGINE_DIR/core/hidden_input_core.c" \
  "$ENGINE_DIR/shell/secure_memory.c" \
  "$ENGINE_DIR/shell/hidden_input_reader.c" \
  "$ENGINE_DIR/shell/bitlocker_volume_opener.c" \
  "$ENGINE_DIR/shell/bitlocker_credential_source.c" \
  "$ENGINE_DIR/shell/bitlocker_unlock.c" \
  "$ENGINE_DIR/shell/bitlocker_bfio_adapter.c" \
  "$ENGINE_DIR/shell/aligned_device_reader.c" \
  "$ENGINE_DIR/shell/partition_extractor.c" \
  "$ENGINE_DIR/shell/file_writer.c" \
  "$ENGINE_DIR/shell/ntfs_bfio_adapter.c" \
  "$ENGINE_DIR/shell/vshadow_bfio_adapter.c" \
  "$ENGINE_DIR/shell/path_resolver.c" \
  "$ENGINE_DIR/shell/disk_enumerator.c" \
  "$ENGINE_DIR/shell/bitlocker_status_checker.c" \
  "$ENGINE_DIR/shell/volume_disk_extents.c" \
  "$ENGINE_DIR/shell/fat_reader.c" \
  "$ENGINE_DIR/shell/exfat_reader.c" \
  "$ENGINE_DIR/shell/partition_reader.c" \
  -L "$LIBFSNTFS_DIR/libfsntfs/.libs" \
  -lfsntfs \
  -L "$LIBVSGPT_DIR/libvsgpt/.libs" \
  -lvsgpt \
  -L "$LIBVSMBR_DIR/libvsmbr/.libs" \
  -lvsmbr \
  -L "$LIBVSHADOW_DIR/libvshadow/.libs" \
  -lvshadow \
  -L "$LIBBDE_DIR/libbde/.libs" \
  -lbde \
  -L /c/dev_x64/cli \
  -lntfs_bfio_bridge \
  -L /clang64/lib \
  -lmpr \
  -o "$OUT"

echo "build succeeded: $OUT"
cp "$LIBFSNTFS_DIR/libfsntfs/.libs/libfsntfs-1.dll" /c/dev_x64/cli/
cp "$LIBVSGPT_DIR/libvsgpt/.libs/libvsgpt-1.dll" /c/dev_x64/cli/
cp "$LIBVSMBR_DIR/libvsmbr/.libs/libvsmbr-1.dll" /c/dev_x64/cli/
cp "$LIBVSHADOW_DIR/libvshadow/.libs/libvshadow-1.dll" /c/dev_x64/cli/
cp "$LIBBDE_DIR/libbde/.libs/libbde-1.dll" /c/dev_x64/cli/
# libbde links against winpthread (see the devlog (docs dir) B2 entry for why);
# this DLL only lives inside the MSYS2 install tree, not on a normal
# Windows system PATH, so it has to ship alongside the other DLLs just
# like libncursesw6.dll does for photorec.exe below.
cp /clang64/bin/libwinpthread-1.dll /c/dev_x64/cli/
ls -la /c/dev_x64/cli/
'@
$cmd | & "C:\msys64\usr\bin\bash.exe" --login

# x86_64 counterpart of cli/build_windows.ps1's photorec.exe build (D5/D6,
# GPL; per AGENTS.md section 3 it can only be compiled into a standalone
# exe invoked as a subprocess, never copied into this project/exe). Reuses
# the same configure flags already verified against configure.ac on the
# ARM64 build. Assumes testdisk vendored source has already been synced to
# /c/dev_x64/testdisk_src.
$cmd2 = @'
set -e
PR_SRC_DIR=/c/dev_x64/testdisk_src
if [ ! -d "$PR_SRC_DIR" ]; then
  echo "WARNING: $PR_SRC_DIR not found, skipping photorec.exe build (sync testdisk source first)"
else
  (
    cd "$PR_SRC_DIR"
    if [ ! -f configure ]; then
      autoreconf -vif -I config -W all
    fi
    if [ ! -f Makefile ]; then
      ./configure --with-ext2fs=no --with-ntfs=no --with-ntfs3g=no --with-reiserfs=no \
        --with-ewf=no --with-jpeg=no --with-zlib=no --enable-missing-uuid-ok --enable-qt=no
    fi
    make -C src photorec.exe
  )
  cp "$PR_SRC_DIR/src/photorec.exe" /c/dev_x64/cli/
  echo "photorec.exe built"
fi
ls -la /c/dev_x64/cli/
'@
$cmd2 | & "C:\msys64\usr\bin\bash.exe" --login

# photorec.exe's one non-system DLL dependency on this toolchain, plus the
# terminfo database ncurses needs at runtime (same story as the ARM64
# build -- see photorec_invoker.c's Windows branch comments).
if( Test-Path "C:\dev_x64\cli\photorec.exe" ) {
    Copy-Item "C:\msys64\clang64\bin\libncursesw6.dll" "C:\dev_x64\cli\" -Force
    if( Test-Path "C:\dev_x64\cli\terminfo" ) { Remove-Item "C:\dev_x64\cli\terminfo" -Recurse -Force }
    Copy-Item "C:\msys64\clang64\share\terminfo" "C:\dev_x64\cli\terminfo" -Recurse -Force
    Write-Host "photorec.exe + libncursesw6.dll + terminfo/ packaged into C:\dev_x64\cli\"
}
