#!/bin/sh
# 编译并运行 engine/core 的单元测试。
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ENGINE_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
LIBFSNTFS_DIR="$ENGINE_DIR/../third_party/lgpl-dynamic/libfsntfs"
FIXTURE="$ENGINE_DIR/tests/fixtures/ntfs-basic-undelete/volume.img"

run_test() {
	TEST_NAME="$1"
	echo "=== $TEST_NAME ==="
	cc -std=c11 \
	  -I "$LIBFSNTFS_DIR/include" \
	  -I "$LIBFSNTFS_DIR/common" \
	  -I "$ENGINE_DIR/core" \
	  "$SCRIPT_DIR/$TEST_NAME.c" \
	  "$ENGINE_DIR/core/undelete_detector.c" \
	  "$ENGINE_DIR/core/undelete_scanner.c" \
	  "$ENGINE_DIR/core/ntfs_file_reference.c" \
	  -L "$LIBFSNTFS_DIR/libfsntfs/.libs" \
	  -lfsntfs \
	  -Wl,-rpath,"$LIBFSNTFS_DIR/libfsntfs/.libs" \
	  -o "$SCRIPT_DIR/$TEST_NAME"

	"$SCRIPT_DIR/$TEST_NAME" "$FIXTURE"
}

run_test "test_undelete_detector"
run_test "test_undelete_scanner"

run_fat_test() {
	TEST_NAME="$1"
	FAT_FIXTURE="$ENGINE_DIR/tests/fixtures/fat32-basic-undelete/volume.img"
	echo "=== $TEST_NAME ==="
	cc -std=c11 \
	  -I "$ENGINE_DIR/core" \
	  "$SCRIPT_DIR/$TEST_NAME.c" \
	  "$ENGINE_DIR/core/fat_boot_sector.c" \
	  "$ENGINE_DIR/core/fat_dir_entry.c" \
	  "$ENGINE_DIR/core/fat_undelete_scanner.c" \
	  "$ENGINE_DIR/core/utf16_to_utf8.c" \
	  -o "$SCRIPT_DIR/$TEST_NAME"

	"$SCRIPT_DIR/$TEST_NAME" "$FAT_FIXTURE"
}

run_fat_test "test_fat_boot_sector"
run_fat_test "test_fat_dir_entry"
run_fat_test "test_fat_undelete_scanner"

run_exfat_test() {
	TEST_NAME="$1"
	EXFAT_FIXTURE="$ENGINE_DIR/tests/fixtures/exfat-basic-undelete/volume.img"
	echo "=== $TEST_NAME ==="
	cc -std=c11 \
	  -I "$ENGINE_DIR/core" \
	  "$SCRIPT_DIR/$TEST_NAME.c" \
	  "$ENGINE_DIR/core/exfat_boot_sector.c" \
	  "$ENGINE_DIR/core/exfat_dir_entry.c" \
	  "$ENGINE_DIR/core/exfat_undelete_scanner.c" \
	  "$ENGINE_DIR/core/utf16_to_utf8.c" \
	  -o "$SCRIPT_DIR/$TEST_NAME"

	"$SCRIPT_DIR/$TEST_NAME" "$EXFAT_FIXTURE"
}

run_exfat_test "test_exfat_boot_sector"
run_exfat_test "test_exfat_dir_entry"
run_exfat_test "test_exfat_undelete_scanner"

run_plain_test() {
	TEST_NAME="$1"
	shift
	echo "=== $TEST_NAME ==="
	cc -std=c11 \
	  -I "$ENGINE_DIR/core" \
	  "$SCRIPT_DIR/$TEST_NAME.c" \
	  "$@" \
	  -o "$SCRIPT_DIR/$TEST_NAME"

	"$SCRIPT_DIR/$TEST_NAME"
}

run_plain_test "test_ntfs_file_reference" "$ENGINE_DIR/core/ntfs_file_reference.c"
run_plain_test "test_path_builder" "$ENGINE_DIR/core/path_builder.c"
run_plain_test "test_sector_aligned_read" "$ENGINE_DIR/core/sector_aligned_read.c"
run_plain_test "test_utf16_to_utf8" "$ENGINE_DIR/core/utf16_to_utf8.c"
run_plain_test "test_exfat_fat_chain" "$ENGINE_DIR/core/exfat_fat_chain.c"
run_plain_test "test_fat_fat_chain" "$ENGINE_DIR/core/fat_fat_chain.c"
run_plain_test "test_bad_sector_policy" "$ENGINE_DIR/core/bad_sector_policy.c"
run_plain_test "test_bad_sector_log" "$ENGINE_DIR/core/bad_sector_log.c"
run_plain_test "test_photorec_report_parser" "$ENGINE_DIR/core/photorec_report_parser.c"
run_plain_test "test_photorec_command_builder" "$ENGINE_DIR/core/photorec_command_builder.c"

run_shell_test() {
	TEST_NAME="$1"
	shift
	echo "=== $TEST_NAME ==="
	cc -std=c11 \
	  -I "$ENGINE_DIR/core" \
	  -I "$ENGINE_DIR/shell" \
	  "$SCRIPT_DIR/$TEST_NAME.c" \
	  "$@" \
	  -o "$SCRIPT_DIR/$TEST_NAME"

	"$SCRIPT_DIR/$TEST_NAME"
}

run_shell_test "test_aligned_device_reader" \
  "$ENGINE_DIR/shell/aligned_device_reader.c" \
  "$ENGINE_DIR/core/sector_aligned_read.c" \
  "$ENGINE_DIR/core/bad_sector_policy.c" \
  "$ENGINE_DIR/core/bad_sector_log.c"

# ntfs_bfio_adapter.c 链接了真实的 libfsntfs（需要用它导出的
# libbfio_handle_initialize 符号，2026-11-05 小树裁决第20步 N7），
# 和上面纯 shell 层的 mock 测试不一样，需要单独一个带 libfsntfs 链接
# 参数的编译命令，不能复用 run_shell_test
run_ntfs_shell_test() {
	TEST_NAME="$1"
	shift
	echo "=== $TEST_NAME ==="
	cc -std=c11 \
	  -I "$LIBFSNTFS_DIR/include" \
	  -I "$ENGINE_DIR/core" \
	  -I "$ENGINE_DIR/shell" \
	  "$SCRIPT_DIR/$TEST_NAME.c" \
	  "$@" \
	  -L "$LIBFSNTFS_DIR/libfsntfs/.libs" \
	  -lfsntfs \
	  -Wl,-rpath,"$LIBFSNTFS_DIR/libfsntfs/.libs" \
	  -o "$SCRIPT_DIR/$TEST_NAME"

	"$SCRIPT_DIR/$TEST_NAME"
}

run_ntfs_shell_test "test_ntfs_bfio_adapter" \
  "$ENGINE_DIR/shell/ntfs_bfio_adapter.c" \
  "$ENGINE_DIR/core/sector_aligned_read.c" \
  "$ENGINE_DIR/core/bad_sector_policy.c" \
  "$ENGINE_DIR/core/bad_sector_log.c"
