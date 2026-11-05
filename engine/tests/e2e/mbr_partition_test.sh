#!/bin/sh
# 第 3 步（分区表解析）MBR 部分的验收脚本，和 gpt_partition_test.sh 同等标准。
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../../.." && pwd)"
FIXTURE_DIR="$ROOT_DIR/engine/tests/fixtures/mbr-basic"
RECOVERCTL="$ROOT_DIR/cli/recoverctl"
VOLUME_IMG="$FIXTURE_DIR/volume.img"
FAIL=0

echo "=========================================="
echo " MBR 分区表解析验证"
echo "=========================================="

if [ ! -x "$RECOVERCTL" ]; then
	echo "recoverctl 还没编译，先跑 cli/build.sh"
	exit 1
fi

OUTPUT="$("$RECOVERCTL" partitions "$VOLUME_IMG")"
printf '%s\n' "$OUTPUT"

if ! printf '%s\n' "$OUTPUT" | grep -q "分区表类型: MBR"; then
	echo "  [FAIL] 应该被识别为 MBR（不是误判成 GPT）"
	FAIL=1
fi

if ! printf '%s\n' "$OUTPUT" | grep -q "找到 2 个分区"; then
	echo "  [FAIL] 应该正好找到 2 个分区"
	FAIL=1
fi

if ! printf '%s\n' "$OUTPUT" | grep -q "偏移=32256"; then
	echo "  [FAIL] 分区 1 偏移应该是 32256"
	FAIL=1
fi

if ! printf '%s\n' "$OUTPUT" | grep -q "类型代码=0x07"; then
	echo "  [FAIL] 分区 1 类型代码应该是 0x07"
	FAIL=1
fi

if ! printf '%s\n' "$OUTPUT" | grep -q "偏移=1080832"; then
	echo "  [FAIL] 分区 2 偏移应该是 1080832"
	FAIL=1
fi

if ! printf '%s\n' "$OUTPUT" | grep -q "类型代码=0x0c"; then
	echo "  [FAIL] 分区 2 类型代码应该是 0x0c"
	FAIL=1
fi

if [ "$FAIL" -eq 0 ]; then
	echo
	echo "=========================================="
	echo " 全部通过：MBR 分区表解析验证成功"
	echo "=========================================="
	exit 0
else
	echo
	echo "=========================================="
	echo " 存在失败项"
	echo "=========================================="
	exit 1
fi
