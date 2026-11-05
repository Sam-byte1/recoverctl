#!/bin/sh
# D3+D4 端到端验证（P4 PhotoRec兜底雕刻引擎，2026-11-05 第28步排期）：
# 不是测 engine/shell/photorec_invoker.c 本身（那是 photorec_carve_test.sh
# 的职责），而是测完整的 recoverctl CLI 命令（recover-carve /
# recover-carve-partition）——参数解析、D3 的分区字节范围切片
# （复用 extract_partition_to_temp_file）、D1/D2 串起来之后，打印给
# 用户看的结果是不是正确、恢复出的文件内容是不是正确。
#
# 和 photorec_carve_test.sh 同样的态度：本机没装 photorec 时优雅跳过，
# 不拖垮整条 e2e 套件。
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../../.." && pwd)"
RECOVERCTL="$ROOT_DIR/cli/recoverctl"
FIXTURE_DIR="$ROOT_DIR/engine/tests/fixtures/photorec-carve"
TEST_IMAGE="$FIXTURE_DIR/test_image.dd"
EMBEDDED_JPEG="$FIXTURE_DIR/embedded.jpg"
OUT_DIR="$(mktemp -d)"
FAIL=0

cleanup() {
	rm -rf "$OUT_DIR"
}
trap cleanup EXIT

echo "=========================================="
echo " recoverctl recover-carve / recover-carve-partition 端到端验证"
echo "=========================================="

PHOTOREC_BIN="$(command -v photorec || true)"
if [ -z "$PHOTOREC_BIN" ] && [ -x "/opt/homebrew/bin/photorec" ]; then
	PHOTOREC_BIN="/opt/homebrew/bin/photorec"
fi
if [ -z "$PHOTOREC_BIN" ]; then
	echo "本机未安装 photorec，跳过这条 e2e 验证（不影响 P4 其它已完成部分）"
	exit 0
fi

if [ ! -x "$RECOVERCTL" ]; then
	echo "[FAIL] 找不到 $RECOVERCTL，先跑一次 cli/build.sh"
	exit 1
fi
if [ ! -f "$TEST_IMAGE" ]; then
	echo "[FAIL] 缺少 fixture：$TEST_IMAGE"
	exit 1
fi

echo "--- 测试 recover-carve（整个镜像当一整块盘处理）---"
CARVE_OUT="$OUT_DIR/carve_whole"
"$RECOVERCTL" recover-carve "$PHOTOREC_BIN" "$TEST_IMAGE" "$CARVE_OUT" > "$OUT_DIR/whole_output.txt" 2>&1
cat "$OUT_DIR/whole_output.txt"

RECUP_DIR="$(ls -d "$CARVE_OUT".* 2>/dev/null | head -1)"
if [ -z "$RECUP_DIR" ] || [ ! -f "$RECUP_DIR/f0000100.jpg" ]; then
	echo "[FAIL] recover-carve 没有在预期目录下产出恢复文件"
	FAIL=1
else
	EXPECTED_HASH="$(shasum -a 256 "$EMBEDDED_JPEG" | cut -d' ' -f1)"
	ACTUAL_HASH="$(shasum -a 256 "$RECUP_DIR/f0000100.jpg" | cut -d' ' -f1)"
	if [ "$EXPECTED_HASH" = "$ACTUAL_HASH" ]; then
		echo "[OK] recover-carve 恢复出的文件哈希匹配：$ACTUAL_HASH"
	else
		echo "[FAIL] recover-carve 哈希不匹配：期望 $EXPECTED_HASH，实际 $ACTUAL_HASH"
		FAIL=1
	fi
fi

echo "--- 测试 recover-carve-partition（先按字节范围切片，再雕刻）---"
IMG_SIZE="$(wc -c < "$TEST_IMAGE" | tr -d ' ')"
CARVE_PART_OUT="$OUT_DIR/carve_partition"
"$RECOVERCTL" recover-carve-partition "$PHOTOREC_BIN" "$TEST_IMAGE" 0 "$IMG_SIZE" "$CARVE_PART_OUT" > "$OUT_DIR/partition_output.txt" 2>&1
cat "$OUT_DIR/partition_output.txt"

RECUP_PART_DIR="$(ls -d "$CARVE_PART_OUT".* 2>/dev/null | head -1)"
if [ -z "$RECUP_PART_DIR" ] || [ ! -f "$RECUP_PART_DIR/f0000100.jpg" ]; then
	echo "[FAIL] recover-carve-partition 没有在预期目录下产出恢复文件"
	FAIL=1
else
	EXPECTED_HASH="$(shasum -a 256 "$EMBEDDED_JPEG" | cut -d' ' -f1)"
	ACTUAL_HASH="$(shasum -a 256 "$RECUP_PART_DIR/f0000100.jpg" | cut -d' ' -f1)"
	if [ "$EXPECTED_HASH" = "$ACTUAL_HASH" ]; then
		echo "[OK] recover-carve-partition 恢复出的文件哈希匹配：$ACTUAL_HASH"
	else
		echo "[FAIL] recover-carve-partition 哈希不匹配：期望 $EXPECTED_HASH，实际 $ACTUAL_HASH"
		FAIL=1
	fi
fi

echo "=========================================="
if [ "$FAIL" -eq 0 ]; then
	echo " 全部通过"
	exit 0
else
	echo " 存在失败项"
	exit 1
fi
