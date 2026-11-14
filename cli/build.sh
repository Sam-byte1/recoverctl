#!/bin/sh
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
LIBFSNTFS_DIR="$ROOT_DIR/third_party/lgpl-dynamic/libfsntfs"
LIBVSGPT_DIR="$ROOT_DIR/third_party/lgpl-dynamic/libvsgpt"
LIBVSMBR_DIR="$ROOT_DIR/third_party/lgpl-dynamic/libvsmbr"
LIBVSHADOW_DIR="$ROOT_DIR/third_party/lgpl-dynamic/libvshadow"
LIBBDE_DIR="$ROOT_DIR/third_party/lgpl-dynamic/libbde"
ENGINE_DIR="$ROOT_DIR/engine"

cc -std=c11 \
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
  "$SCRIPT_DIR/main.c" \
  "$ENGINE_DIR/core/undelete_detector.c" \
  "$ENGINE_DIR/core/undelete_scanner.c" \
  "$ENGINE_DIR/core/ads_enumerator.c" \
  "$ENGINE_DIR/core/ntfs_file_reference.c" \
  "$ENGINE_DIR/core/path_builder.c" \
  "$ENGINE_DIR/core/sector_aligned_read.c" \
  "$ENGINE_DIR/core/bad_sector_policy.c" \
  "$ENGINE_DIR/core/bad_sector_log.c" \
  "$ENGINE_DIR/core/utf16_to_utf8.c" \
  "$ENGINE_DIR/core/fat_boot_sector.c" \
  "$ENGINE_DIR/core/fat_dir_entry.c" \
  "$ENGINE_DIR/core/fat_undelete_scanner.c" \
  "$ENGINE_DIR/core/exfat_boot_sector.c" \
  "$ENGINE_DIR/core/exfat_dir_entry.c" \
  "$ENGINE_DIR/core/exfat_undelete_scanner.c" \
  "$ENGINE_DIR/core/exfat_fat_chain.c" \
  "$ENGINE_DIR/core/fat_fat_chain.c" \
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
  "$ENGINE_DIR/shell/file_writer.c" \
  "$ENGINE_DIR/shell/path_resolver.c" \
  "$ENGINE_DIR/shell/ntfs_bfio_adapter.c" \
  "$ENGINE_DIR/shell/vshadow_bfio_adapter.c" \
  "$ENGINE_DIR/shell/aligned_device_reader.c" \
  "$ENGINE_DIR/shell/partition_extractor.c" \
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
  -Wl,-rpath,"$LIBFSNTFS_DIR/libfsntfs/.libs" \
  -Wl,-rpath,"$LIBVSGPT_DIR/libvsgpt/.libs" \
  -Wl,-rpath,"$LIBVSMBR_DIR/libvsmbr/.libs" \
  -Wl,-rpath,"$LIBVSHADOW_DIR/libvshadow/.libs" \
  -Wl,-rpath,"$LIBBDE_DIR/libbde/.libs" \
  -o "$SCRIPT_DIR/recoverctl"

echo "编译成功: $SCRIPT_DIR/recoverctl"
