#!/bin/sh
# exFAT Walking Skeleton 正式验收脚本，和 FAT32 版
# （fat32_walking_skeleton_test.sh）同等标准：扫描 fixture 卷 -> 识别已删除文件 ->
# 恢复内容到磁盘 -> 对比恢复出来的内容和删除前记录的原始哈希。
#
# 范围提醒（和 engine/tests/fixtures/exfat-basic-undelete/make_fixture.py 里的
# 说明保持一致）：这一版 exFAT 支持根目录 + 任意深度子目录递归
# （2026-11-05 小树裁决第12步先加了一层，第14步扩展成真正的递归）+
# 文件名<=15字符 + 正确的 UTF-16LE→UTF-8 转码含非ASCII字符（2026-11-05
# 小树裁决第13步修复，之前是简化处理会产生垃圾字节）+ 碎片化（非连续
# 存储）文件的恢复（2026-11-05 小树裁决第16步：跟随 FAT 链逐簇读取，
# 第15步真机验证已证实 exFAT 删除不会清空 FAT 链信息）+ 目录本身超过
# 一个簇时跟随 FAT 链继续读取（2026-11-05 第21步，"目录单簇假设"修复，
# BIGDIR 场景验证）。超长文件名（需要多条 File Name 目录项）是明确列出
# 的后续增量，不在这个脚本的验收范围内。
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../../.." && pwd)"
FIXTURE_DIR="$ROOT_DIR/engine/tests/fixtures/exfat-basic-undelete"
RECOVERCTL="$ROOT_DIR/cli/recoverctl"
VOLUME_IMG="$FIXTURE_DIR/volume.img"
OUT_DIR="$(mktemp -d)"
FAIL=0

cleanup() {
	rm -rf "$OUT_DIR"
}
trap cleanup EXIT

echo "=========================================="
echo " exFAT Walking Skeleton 端到端验证"
echo "=========================================="

if [ ! -x "$RECOVERCTL" ]; then
	echo "recoverctl 还没编译，先跑 cli/build.sh"
	exit 1
fi

echo
echo "[1/3] 扫描测试卷: $VOLUME_IMG"
SCAN_OUTPUT="$("$RECOVERCTL" scan "$VOLUME_IMG")"
printf '%s\n' "$SCAN_OUTPUT"

if ! printf '%s\n' "$SCAN_OUTPUT" | grep -q "正在扫描 (exFAT)"; then
	echo "  [FAIL] 没有走 exFAT 分支，可能被误判成了 NTFS/FAT32 或者没识别出来"
	FAIL=1
fi

if ! printf '%s\n' "$SCAN_OUTPUT" | grep -q "文件名=SECRET.TXT"; then
	echo "  [FAIL] 没有扫到完整文件名 SECRET.TXT（exFAT 删除应该保留完整文件名，不像 FAT32 首字符被破坏）"
	FAIL=1
fi

if printf '%s\n' "$SCAN_OUTPUT" | grep -q "HELLO"; then
	echo "  [FAIL] 不应该把正常存在的 HELLO.TXT 也判定成已删除"
	FAIL=1
fi

if ! printf '%s\n' "$SCAN_OUTPUT" | grep -q "路径=SUBDIR\\\\DEEP.TXT"; then
	echo "  [FAIL] 没有在子目录 SUBDIR 里扫到已删除的 DEEP.TXT（路径重建应该是 SUBDIR\\DEEP.TXT，文件名完整保留）"
	FAIL=1
fi

if printf '%s\n' "$SCAN_OUTPUT" | grep -q "STILL"; then
	echo "  [FAIL] 不应该把子目录里正常存在的 STILL.TXT 也判定成已删除"
	FAIL=1
fi

if ! printf '%s\n' "$SCAN_OUTPUT" | grep -q "测试文件.txt"; then
	echo "  [FAIL] 没有扫到正确解码的中文文件名（非 ASCII 转码应该是正确的 UTF-8，不是垃圾字节）"
	FAIL=1
fi

if ! printf '%s\n' "$SCAN_OUTPUT" | grep -q "存储=碎片化(跟随FAT链恢复)  文件名=FRAG.BIN"; then
	echo "  [FAIL] 没有扫到碎片化文件 FRAG.BIN，或者没有正确标记为碎片化存储（验证 no_fat_chain 透传）"
	FAIL=1
fi

if ! printf '%s\n' "$SCAN_OUTPUT" | grep -q "路径=SUBDIR\\\\SUBSUBDIR\\\\DEEP2.TXT"; then
	echo "  [FAIL] 没有在第二层子目录 SUBDIR\\SUBSUBDIR 里扫到已删除的 DEEP2.TXT（验证递归不是只支持固定一层）"
	FAIL=1
fi

if printf '%s\n' "$SCAN_OUTPUT" | grep -q "STILL2"; then
	echo "  [FAIL] 不应该把第二层子目录里正常存在的 STILL2.TXT 也判定成已删除"
	FAIL=1
fi

if ! printf '%s\n' "$SCAN_OUTPUT" | grep -q "路径=BIGDIR\\\\DEEP3.TXT"; then
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
echo "[2/3] 恢复 SECRET.TXT（候选项序号 0）、中文文件名（候选项序号 1）、碎片化的 FRAG.BIN（候选项序号 2）、"
echo "      子目录里的 DEEP.TXT（候选项序号 3）、第二层子目录里的 DEEP2.TXT（候选项序号 4）"
echo "      和多簇目录 BIGDIR 第二簇里的 DEEP3.TXT（候选项序号 5）并校验哈希"
"$RECOVERCTL" recover "$VOLUME_IMG" 0 "$OUT_DIR/SECRET.TXT"

EXPECTED="$(shasum -a 256 "$FIXTURE_DIR/SECRET.TXT.original" | awk '{print $1}')"
ACTUAL="$(shasum -a 256 "$OUT_DIR/SECRET.TXT" | awk '{print $1}')"

if [ "$EXPECTED" = "$ACTUAL" ]; then
	echo "  [PASS] SECRET.TXT 哈希一致 ($ACTUAL)"
else
	echo "  [FAIL] SECRET.TXT 哈希不一致：期望 $EXPECTED，实际 $ACTUAL"
	FAIL=1
fi

"$RECOVERCTL" recover "$VOLUME_IMG" 1 "$OUT_DIR/chinese.txt"

EXPECTED_CN="$(shasum -a 256 "$FIXTURE_DIR/CHINESE_NAME.original" | awk '{print $1}')"
ACTUAL_CN="$(shasum -a 256 "$OUT_DIR/chinese.txt" | awk '{print $1}')"

if [ "$EXPECTED_CN" = "$ACTUAL_CN" ]; then
	echo "  [PASS] 中文文件名文件哈希一致 ($ACTUAL_CN)"
else
	echo "  [FAIL] 中文文件名文件哈希不一致：期望 $EXPECTED_CN，实际 $ACTUAL_CN"
	FAIL=1
fi

"$RECOVERCTL" recover "$VOLUME_IMG" 2 "$OUT_DIR/FRAG.BIN"

EXPECTED_FRAG="$(shasum -a 256 "$FIXTURE_DIR/FRAG.BIN.original" | awk '{print $1}')"
ACTUAL_FRAG="$(shasum -a 256 "$OUT_DIR/FRAG.BIN" | awk '{print $1}')"

if [ "$EXPECTED_FRAG" = "$ACTUAL_FRAG" ]; then
	echo "  [PASS] 碎片化文件 FRAG.BIN 哈希一致（跟随 FAT 链恢复成功）: $ACTUAL_FRAG"
else
	echo "  [FAIL] FRAG.BIN 哈希不一致：期望 $EXPECTED_FRAG，实际 $ACTUAL_FRAG"
	FAIL=1
fi

"$RECOVERCTL" recover "$VOLUME_IMG" 3 "$OUT_DIR/DEEP.TXT"

EXPECTED_SUB="$(shasum -a 256 "$FIXTURE_DIR/DEEP.TXT.original" | awk '{print $1}')"
ACTUAL_SUB="$(shasum -a 256 "$OUT_DIR/DEEP.TXT" | awk '{print $1}')"

if [ "$EXPECTED_SUB" = "$ACTUAL_SUB" ]; then
	echo "  [PASS] SUBDIR\\DEEP.TXT 哈希一致 ($ACTUAL_SUB)"
else
	echo "  [FAIL] SUBDIR\\DEEP.TXT 哈希不一致：期望 $EXPECTED_SUB，实际 $ACTUAL_SUB"
	FAIL=1
fi

"$RECOVERCTL" recover "$VOLUME_IMG" 4 "$OUT_DIR/DEEP2.TXT"

EXPECTED_DEEP2="$(shasum -a 256 "$FIXTURE_DIR/DEEP2.TXT.original" | awk '{print $1}')"
ACTUAL_DEEP2="$(shasum -a 256 "$OUT_DIR/DEEP2.TXT" | awk '{print $1}')"

if [ "$EXPECTED_DEEP2" = "$ACTUAL_DEEP2" ]; then
	echo "  [PASS] SUBDIR\\SUBSUBDIR\\DEEP2.TXT 哈希一致 ($ACTUAL_DEEP2)"
else
	echo "  [FAIL] SUBDIR\\SUBSUBDIR\\DEEP2.TXT 哈希不一致：期望 $EXPECTED_DEEP2，实际 $ACTUAL_DEEP2"
	FAIL=1
fi

"$RECOVERCTL" recover "$VOLUME_IMG" 5 "$OUT_DIR/DEEP3.TXT"

EXPECTED_DEEP3="$(shasum -a 256 "$FIXTURE_DIR/DEEP3.TXT.original" | awk '{print $1}')"
ACTUAL_DEEP3="$(shasum -a 256 "$OUT_DIR/DEEP3.TXT" | awk '{print $1}')"

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
	echo " 全部通过：exFAT Walking Skeleton 验收成功"
	echo "=========================================="
	exit 0
else
	echo
	echo "=========================================="
	echo " 存在失败项，exFAT Walking Skeleton 验收未通过"
	echo "=========================================="
	exit 1
fi
