#!/bin/sh
# Task #7：Walking Skeleton 端到端验证脚本。
#
# 跑一遍完整闭环：扫描 fixture 卷 -> 识别已删除文件 -> 恢复内容到磁盘 ->
# 对比恢复出来的内容和删除前记录的原始哈希。
#
# 这是整个 docs/开发规划.md 里定义的"第一个 Walking Skeleton 里程碑"的
# 正式验收脚本，不是探索性工具——这个脚本跑过、全部 PASS，就代表
# 这个里程碑真正完成了。
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../../.." && pwd)"
FIXTURE_DIR="$ROOT_DIR/engine/tests/fixtures/ntfs-basic-undelete"
RECOVERCTL="$ROOT_DIR/cli/recoverctl"
VOLUME_IMG="$FIXTURE_DIR/volume.img"
OUT_DIR="$(mktemp -d)"
FAIL=0

cleanup() {
	rm -rf "$OUT_DIR"
}
trap cleanup EXIT

echo "=========================================="
echo " Walking Skeleton 端到端验证"
echo "=========================================="

if [ ! -x "$RECOVERCTL" ]; then
	echo "recoverctl 还没编译，先跑 cli/build.sh"
	exit 1
fi

echo
echo "[1/3] 扫描测试卷: $VOLUME_IMG"
SCAN_OUTPUT="$("$RECOVERCTL" scan "$VOLUME_IMG")"
printf '%s\n' "$SCAN_OUTPUT"

if ! printf '%s\n' "$SCAN_OUTPUT" | grep -q "文件名=passwords.txt"; then
	echo "  [FAIL] passwords.txt 直接在根目录下，应该只显示文件名，不应该带路径前缀"
	FAIL=1
fi

if ! printf '%s\n' "$SCAN_OUTPUT" | grep -q "路径=a_directory\\\\another_file"; then
	echo "  [FAIL] another_file 在 a_directory 子目录里，应该重建出完整路径 a_directory\\another_file"
	FAIL=1
fi

check_hash() {
	LOGICAL_NAME="$1"
	MFT_INDEX="$2"

	echo
	echo "[*] 恢复 MFT 索引 $MFT_INDEX ($LOGICAL_NAME) ..."
	"$RECOVERCTL" recover "$VOLUME_IMG" "$MFT_INDEX" "$OUT_DIR/$LOGICAL_NAME"

	EXPECTED="$(shasum -a 256 "$FIXTURE_DIR/$LOGICAL_NAME.original" | awk '{print $1}')"
	ACTUAL="$(shasum -a 256 "$OUT_DIR/$LOGICAL_NAME" | awk '{print $1}')"

	if [ "$EXPECTED" = "$ACTUAL" ]; then
		echo "  [PASS] $LOGICAL_NAME 哈希一致 ($ACTUAL)"
	else
		echo "  [FAIL] $LOGICAL_NAME 哈希不一致：期望 $EXPECTED，实际 $ACTUAL"
		FAIL=1
	fi
}

echo
echo "[2/3] 逐个恢复已知的已删除文件并校验哈希"
check_hash "passwords.txt" 66
check_hash "another_file" 67

echo
echo "[3/3] 汇总结果"

if [ "$FAIL" -eq 0 ]; then
	echo
	echo "=========================================="
	echo " 全部通过：Walking Skeleton 里程碑验收成功"
	echo "=========================================="
	exit 0
else
	echo
	echo "=========================================="
	echo " 存在失败项，里程碑验收未通过"
	echo "=========================================="
	exit 1
fi
