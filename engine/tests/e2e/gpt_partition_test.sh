#!/bin/sh
# 第 3 步（分区表解析）GPT 部分的验收脚本：对手工构造的 GPT 镜像，
# recoverctl partitions 应该能正确解析出分区数量/偏移/大小/类型。
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../../.." && pwd)"
FIXTURE_DIR="$ROOT_DIR/engine/tests/fixtures/gpt-basic"
RECOVERCTL="$ROOT_DIR/cli/recoverctl"
VOLUME_IMG="$FIXTURE_DIR/volume.img"
FAIL=0

echo "=========================================="
echo " GPT 分区表解析验证"
echo "=========================================="

if [ ! -x "$RECOVERCTL" ]; then
	echo "recoverctl 还没编译，先跑 cli/build.sh"
	exit 1
fi

OUTPUT="$("$RECOVERCTL" partitions "$VOLUME_IMG")"
printf '%s\n' "$OUTPUT"

if ! printf '%s\n' "$OUTPUT" | grep -q "找到 2 个分区"; then
	echo "  [FAIL] 应该正好找到 2 个分区"
	FAIL=1
fi

if ! printf '%s\n' "$OUTPUT" | grep -q "偏移=17408"; then
	echo "  [FAIL] 分区 1 偏移应该是 17408"
	FAIL=1
fi

if ! printf '%s\n' "$OUTPUT" | grep -q "偏移=1065984"; then
	echo "  [FAIL] 分区 2 偏移应该是 1065984"
	FAIL=1
fi

if ! printf '%s\n' "$OUTPUT" | grep -q "大小=1.0MB"; then
	echo "  [FAIL] 分区 1 大小应该是 1.0MB"
	FAIL=1
fi

if ! printf '%s\n' "$OUTPUT" | grep -q "大小=2.0MB"; then
	echo "  [FAIL] 分区 2 大小应该是 2.0MB"
	FAIL=1
fi

if [ "$FAIL" -eq 0 ]; then
	echo
	echo "=========================================="
	echo " 全部通过：GPT 分区表解析验证成功"
	echo "=========================================="
	exit 0
else
	echo
	echo "=========================================="
	echo " 存在失败项"
	echo "=========================================="
	exit 1
fi
