#!/bin/sh
# F6：FAT32 Walking Skeleton 正式验收脚本，和 NTFS 版
# （walking_skeleton_test.sh）同等标准：扫描 fixture 卷 -> 识别已删除文件 ->
# 恢复内容到磁盘 -> 对比恢复出来的内容和删除前记录的原始哈希。
#
# 范围提醒（和 docs/开发规划.md 第 8 节"第 2 步"保持一致）：这一版 FAT32
# 支持根目录 + 任意深度子目录递归（2026-11-05 小树裁决第12步追加一层，
# 第14步扩展成真正的递归）+ VFAT 长文件名解析含非 ASCII 转码
# （2026-11-05 小树裁决第13步追加）+ 假设数据连续存储，碎片拼接是明确
# 列出的后续增量，不在这个脚本的验收范围内。目录本身超过一个簇时跟随
# FAT 链继续读取已支持（2026-11-05 第21步，"目录单簇假设"修复，BIGDIR
# 场景验证）。
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../../.." && pwd)"
FIXTURE_DIR="$ROOT_DIR/engine/tests/fixtures/fat32-basic-undelete"
RECOVERCTL="$ROOT_DIR/cli/recoverctl"
VOLUME_IMG="$FIXTURE_DIR/volume.img"
OUT_DIR="$(mktemp -d)"
FAIL=0

cleanup() {
	rm -rf "$OUT_DIR"
}
trap cleanup EXIT

echo "=========================================="
echo " FAT32 Walking Skeleton 端到端验证"
echo "=========================================="

if [ ! -x "$RECOVERCTL" ]; then
	echo "recoverctl 还没编译，先跑 cli/build.sh"
	exit 1
fi

echo
echo "[1/3] 扫描测试卷: $VOLUME_IMG"
SCAN_OUTPUT="$("$RECOVERCTL" scan "$VOLUME_IMG")"
printf '%s\n' "$SCAN_OUTPUT"

if ! printf '%s\n' "$SCAN_OUTPUT" | grep -q "正在扫描 (FAT32)"; then
	echo "  [FAIL] 没有走 FAT32 分支，可能被误判成了 NTFS 或者没识别出来"
	FAIL=1
fi

if ! printf '%s\n' "$SCAN_OUTPUT" | grep -q "起始簇=3"; then
	echo "  [FAIL] 没有扫到起始簇=3（HELLO.TXT）"
	FAIL=1
fi

if printf '%s\n' "$SCAN_OUTPUT" | grep -q "WORLD"; then
	echo "  [FAIL] 不应该把正常存在的 WORLD.TXT 也判定成已删除"
	FAIL=1
fi

if ! printf '%s\n' "$SCAN_OUTPUT" | grep -q "路径=SUBDIR\\\\_ECRET.TXT"; then
	echo "  [FAIL] 没有在子目录 SUBDIR 里扫到已删除的 SECRET.TXT（路径重建应该是 SUBDIR\\_ECRET.TXT）"
	FAIL=1
fi

if printf '%s\n' "$SCAN_OUTPUT" | grep -q "NORMAL"; then
	echo "  [FAIL] 不应该把子目录里正常存在的 NORMAL.TXT 也判定成已删除"
	FAIL=1
fi

if ! printf '%s\n' "$SCAN_OUTPUT" | grep -q "中文长文件名测试文件这是一个测试.txt"; then
	echo "  [FAIL] 没有扫到完整的中文长文件名（VFAT 长文件名跨多条目录项拼接+非ASCII转码应该都正确）"
	FAIL=1
fi

if ! printf '%s\n' "$SCAN_OUTPUT" | grep -q "路径=SUBDIR\\\\SUBSUB2\\\\_EEP2.TXT"; then
	echo "  [FAIL] 没有在第二层子目录 SUBDIR\\SUBSUB2 里扫到已删除的 DEEP2.TXT（验证递归不是只支持固定一层）"
	FAIL=1
fi

if printf '%s\n' "$SCAN_OUTPUT" | grep -q "STILL2"; then
	echo "  [FAIL] 不应该把第二层子目录里正常存在的 STILL2.TXT 也判定成已删除"
	FAIL=1
fi

if ! printf '%s\n' "$SCAN_OUTPUT" | grep -q "路径=BIGDIR\\\\_EEP3.TXT"; then
	echo "  [FAIL] 没有在 BIGDIR 里扫到已删除的 DEEP3.TXT——BIGDIR 自己的目录内容超过一个簇，"
	echo "         DEEP3.TXT 藏在第二个簇里，必须跟着 FAT 链走才能找到（2026-11-05 第21步"
	echo "         '目录单簇假设'修复，如果这个失败说明回退到了旧的单簇读取行为）"
	FAIL=1
fi

if printf '%s\n' "$SCAN_OUTPUT" | grep -q "STILL3"; then
	echo "  [FAIL] 不应该把 BIGDIR 第一个簇里正常存在的 STILL3.TXT 也判定成已删除"
	FAIL=1
fi

echo
echo "[2/3] 恢复 HELLO.TXT（候选项序号 0）、中文长文件名文件（候选项序号 1）、子目录里的 SECRET.TXT（候选项序号 2）、"
echo "      第二层子目录里的 DEEP2.TXT（候选项序号 3）和多簇目录 BIGDIR 第二簇里的 DEEP3.TXT（候选项序号 4）并校验哈希"
"$RECOVERCTL" recover "$VOLUME_IMG" 0 "$OUT_DIR/HELLO.TXT"

EXPECTED="$(shasum -a 256 "$FIXTURE_DIR/HELLO.TXT.original" | awk '{print $1}')"
ACTUAL="$(shasum -a 256 "$OUT_DIR/HELLO.TXT" | awk '{print $1}')"

if [ "$EXPECTED" = "$ACTUAL" ]; then
	echo "  [PASS] HELLO.TXT 哈希一致 ($ACTUAL)"
else
	echo "  [FAIL] HELLO.TXT 哈希不一致：期望 $EXPECTED，实际 $ACTUAL"
	FAIL=1
fi

"$RECOVERCTL" recover "$VOLUME_IMG" 1 "$OUT_DIR/longname.txt"

EXPECTED_LN="$(shasum -a 256 "$FIXTURE_DIR/LONGNAME_CHINESE.original" | awk '{print $1}')"
ACTUAL_LN="$(shasum -a 256 "$OUT_DIR/longname.txt" | awk '{print $1}')"

if [ "$EXPECTED_LN" = "$ACTUAL_LN" ]; then
	echo "  [PASS] 中文长文件名文件哈希一致 ($ACTUAL_LN)"
else
	echo "  [FAIL] 中文长文件名文件哈希不一致：期望 $EXPECTED_LN，实际 $ACTUAL_LN"
	FAIL=1
fi

"$RECOVERCTL" recover "$VOLUME_IMG" 2 "$OUT_DIR/SECRET.TXT"

EXPECTED_SUB="$(shasum -a 256 "$FIXTURE_DIR/SECRET.TXT.original" | awk '{print $1}')"
ACTUAL_SUB="$(shasum -a 256 "$OUT_DIR/SECRET.TXT" | awk '{print $1}')"

if [ "$EXPECTED_SUB" = "$ACTUAL_SUB" ]; then
	echo "  [PASS] SUBDIR\\SECRET.TXT 哈希一致 ($ACTUAL_SUB)"
else
	echo "  [FAIL] SUBDIR\\SECRET.TXT 哈希不一致：期望 $EXPECTED_SUB，实际 $ACTUAL_SUB"
	FAIL=1
fi

"$RECOVERCTL" recover "$VOLUME_IMG" 3 "$OUT_DIR/DEEP2.TXT"

EXPECTED_DEEP2="$(shasum -a 256 "$FIXTURE_DIR/DEEP2.TXT.original" | awk '{print $1}')"
ACTUAL_DEEP2="$(shasum -a 256 "$OUT_DIR/DEEP2.TXT" | awk '{print $1}')"

if [ "$EXPECTED_DEEP2" = "$ACTUAL_DEEP2" ]; then
	echo "  [PASS] SUBDIR\\SUBSUB2\\DEEP2.TXT 哈希一致 ($ACTUAL_DEEP2)"
else
	echo "  [FAIL] SUBDIR\\SUBSUB2\\DEEP2.TXT 哈希不一致：期望 $EXPECTED_DEEP2，实际 $ACTUAL_DEEP2"
	FAIL=1
fi

"$RECOVERCTL" recover "$VOLUME_IMG" 4 "$OUT_DIR/BIGDIR_DEEP3.TXT"

EXPECTED_DEEP3="$(shasum -a 256 "$FIXTURE_DIR/BIGDIR_DEEP3.TXT.original" | awk '{print $1}')"
ACTUAL_DEEP3="$(shasum -a 256 "$OUT_DIR/BIGDIR_DEEP3.TXT" | awk '{print $1}')"

if [ "$EXPECTED_DEEP3" = "$ACTUAL_DEEP3" ]; then
	echo "  [PASS] BIGDIR\\DEEP3.TXT 哈希一致（多簇目录、跟随 FAT 链找到第二簇）: $ACTUAL_DEEP3"
else
	echo "  [FAIL] BIGDIR\\DEEP3.TXT 哈希不一致：期望 $EXPECTED_DEEP3，实际 $ACTUAL_DEEP3"
	FAIL=1
fi

echo
echo "[3/3] 汇总结果"

if [ "$FAIL" -eq 0 ]; then
	echo
	echo "=========================================="
	echo " 全部通过：FAT32 Walking Skeleton 验收成功"
	echo "=========================================="
	exit 0
else
	echo
	echo "=========================================="
	echo " 存在失败项，FAT32 Walking Skeleton 验收未通过"
	echo "=========================================="
	exit 1
fi
