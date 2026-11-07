$env:MSYSTEM = "CLANGARM64"
$cmd = @'
set -e
LIBFSNTFS_DIR=/c/dev/libfsntfs
LIBVSGPT_DIR=/c/dev/libvsgpt
LIBVSMBR_DIR=/c/dev/libvsmbr
LIBVSHADOW_DIR=/c/dev/libvshadow
LIBBDE_DIR=/c/dev/libbde
ENGINE_DIR=/c/dev/engine
OUT=/c/dev/cli/recoverctl.exe
BRIDGE_DLL=/c/dev/cli/ntfs_bfio_bridge.dll
BRIDGE_IMPLIB=/c/dev/cli/libntfs_bfio_bridge.dll.a

# Windows libfsntfs DLL does not export libbfio_handle_initialize/free (internal
# symbols, unlike the Mac dylib). Build our own bridge DLL from the same source
# files, using the same config.h so struct layout matches exactly.
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

# NOTE: this source file list must be kept in sync with ../cli/build.sh's list
# by hand (no shared generation step exists yet). When adding a new
# engine/core or engine/shell source file, add it to both scripts.
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
  /c/dev/cli/main.c \
  "$ENGINE_DIR/core/undelete_detector.c" \
  "$ENGINE_DIR/core/undelete_scanner.c" \
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
  -L /c/dev/cli \
  -lntfs_bfio_bridge \
  -L /clangarm64/lib \
  -lmpr \
  -o "$OUT"

echo "build succeeded: $OUT"
cp "$LIBFSNTFS_DIR/libfsntfs/.libs/libfsntfs-1.dll" /c/dev/cli/
cp "$LIBVSGPT_DIR/libvsgpt/.libs/libvsgpt-1.dll" /c/dev/cli/
cp "$LIBVSMBR_DIR/libvsmbr/.libs/libvsmbr-1.dll" /c/dev/cli/
cp "$LIBVSHADOW_DIR/libvshadow/.libs/libvshadow-1.dll" /c/dev/cli/
cp "$LIBBDE_DIR/libbde/.libs/libbde-1.dll" /c/dev/cli/
# libbde links against winpthread (see docs/开发日志.md B2 entry for why);
# this DLL only lives inside the MSYS2 install tree, not on a normal
# Windows system PATH, so it has to ship alongside the other DLLs just
# like libncursesw6.dll does for photorec.exe below.
cp /clangarm64/bin/libwinpthread-1.dll /c/dev/cli/
ls -la /c/dev/cli/
'@
$cmd | & "C:\msys64\usr\bin\bash.exe" --login

# D5/D6 (P4 PhotoRec fallback carving engine, 2026-11-05 step 28 implementation +
# VM hardware verification done): build photorec.exe (GPL; per AGENTS.md section 3
# it can only be compiled into a standalone exe invoked as a subprocess, never
# copied into this project/exe). Assumes testdisk vendored source is already
# synced to /c/dev/testdisk_src (syncing itself is out of scope for this script,
# same convention as the other third-party libraries above). These --with-xxx=no
# flags were verified directly against configure.ac source: keep only what the
# command-line photorec binary actually needs, drop the heavy deps that
# testdisk/qphotorec use but we don't (ext2fs/ntfs/ntfs3g/reiserfs/ewf/jpeg/zlib/qt).
#
# This is a SEPARATE bash --login invocation, not merged into the heredoc above.
# Root cause found by testing: Chinese-language comments placed directly in the
# top-level PowerShell script text (outside any here-string) on this VM corrupt
# parsing once a SECOND here-string follows. This is the same class of bug
# already documented in the project devlog from an earlier round (search for
# "PowerShell Chinese-comment-via-pipe corruption").
# Keeping comments in plain ASCII here and running this as its own
# heredoc/bash invocation avoids it; this was verified to work end-to-end
# on the real VM, not assumed from first principles.
$cmd2 = @'
set -e
PR_SRC_DIR=/c/dev/testdisk_src
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
  cp "$PR_SRC_DIR/src/photorec.exe" /c/dev/cli/
  echo "photorec.exe built"
fi
ls -la /c/dev/cli/
'@
$cmd2 | & "C:\msys64\usr\bin\bash.exe" --login

# photorec.exe's one non-system DLL dependency (verified with ldd; everything
# else it links against is a stock Windows system DLL already present on any
# install, so nothing else needs to ship). ncurses on this cross-compiled
# build cannot resolve a POSIX-style terminfo path at runtime, so the full
# terminfo database has to ship alongside it in a Windows-reachable directory,
# and photorec_invoker.c's Windows branch points TERMINFO there explicitly
# (see that file's comments for the full story). Copying these two with
# PowerShell's own Copy-Item instead of a bash "cp" inside the heredoc above:
# that bash cp was flaky in VM testing (succeeded sometimes, silently no-op'd
# other times, cause not fully root-caused), while Copy-Item was reliable
# across every repeated run during verification.
if( Test-Path "C:\dev\cli\photorec.exe" ) {
    Copy-Item "C:\msys64\clangarm64\bin\libncursesw6.dll" "C:\dev\cli\" -Force
    if( Test-Path "C:\dev\cli\terminfo" ) { Remove-Item "C:\dev\cli\terminfo" -Recurse -Force }
    Copy-Item "C:\msys64\clangarm64\share\terminfo" "C:\dev\cli\terminfo" -Recurse -Force
    Write-Host "photorec.exe + libncursesw6.dll + terminfo/ packaged into C:\dev\cli\"
}
