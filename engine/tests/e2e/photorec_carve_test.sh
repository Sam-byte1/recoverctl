#!/bin/sh
# D2 端到端验证（P4 PhotoRec兜底雕刻引擎，2026-11-05 第28步排期）：
# 真实拉起一次 photorec 子进程（`engine/shell/photorec_invoker.c`），
# 对一个嵌了真实 JPEG 的裸镜像跑 `/cmd ... partition_none,search`，解析
# 它自己生成的 report.xml（`engine/core/photorec_report_parser.c`），
# 确认雕刻出来的文件和嵌入前的原始文件哈希完全一致。
#
# 这条脚本验证的不是"PhotoRec 这个工具本身靠不靠谱"（那是第27步调研
# 已经用手工命令实测过的结论，见 docs/开发日志.md），而是"我们自己写的
# 子进程封装代码 + report.xml 解析代码，接起来之后是不是真的能跑通"。
#
# 这台机器上如果没有装 photorec（比如一台从没跑过本项目 P4 相关调研的
# 新开发机），直接跳过（exit 0），不让整条 e2e 套件因为一个可选的本地
# 工具没装而变红——这和 vshadow_offline_test.sh 对"这份公开镜像本身没有
# 已删除文件"的诚实处理是同一种态度：环境限制如实说明，不伪造通过。
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../../.." && pwd)"
ENGINE_DIR="$ROOT_DIR/engine"
FIXTURE_DIR="$ENGINE_DIR/tests/fixtures/photorec-carve"
TEST_IMAGE="$FIXTURE_DIR/test_image.dd"
EMBEDDED_JPEG="$FIXTURE_DIR/embedded.jpg"
OUT_DIR="$(mktemp -d)"
FAIL=0

cleanup() {
	rm -rf "$OUT_DIR"
}
trap cleanup EXIT

echo "=========================================="
echo " PhotoRec 雕刻集成 D2 端到端验证"
echo "=========================================="

PHOTOREC_BIN="$(command -v photorec || true)"
if [ -z "$PHOTOREC_BIN" ] && [ -x "/opt/homebrew/bin/photorec" ]; then
	PHOTOREC_BIN="/opt/homebrew/bin/photorec"
fi

if [ -z "$PHOTOREC_BIN" ]; then
	echo "本机未安装 photorec（比如 brew install testdisk），跳过这条 e2e 验证"
	echo "注意：这只是跳过'我们的封装代码接得对不对'这一步的真机验证，不影响"
	echo "P4 其它已经完成的部分；要跑这条验证，装一个 photorec 可执行文件即可。"
	exit 0
fi
echo "使用 photorec: $PHOTOREC_BIN"

if [ ! -f "$TEST_IMAGE" ]; then
	echo "[FAIL] 缺少 fixture：$TEST_IMAGE（先跑一次 make_fixture.py）"
	exit 1
fi

HARNESS="$OUT_DIR/photorec_carve_harness"
cc -std=c11 \
  -I "$ENGINE_DIR/core" \
  -I "$ENGINE_DIR/shell" \
  "$SCRIPT_DIR/photorec_carve_harness.c" \
  "$ENGINE_DIR/shell/photorec_invoker.c" \
  "$ENGINE_DIR/core/photorec_command_builder.c" \
  "$ENGINE_DIR/core/photorec_report_parser.c" \
  -o "$HARNESS"

HARNESS_OUTPUT="$OUT_DIR/harness_output.txt"
"$HARNESS" "$PHOTOREC_BIN" "$TEST_IMAGE" "$OUT_DIR/carve_out" > "$HARNESS_OUTPUT"
cat "$HARNESS_OUTPUT"

EXIT_CODE_LINE="$(grep '^EXIT_CODE=' "$HARNESS_OUTPUT")"
if [ "$EXIT_CODE_LINE" != "EXIT_CODE=0" ]; then
	echo "[FAIL] photorec 子进程退出码不是 0：$EXIT_CODE_LINE"
	FAIL=1
fi

ENTRY_COUNT_LINE="$(grep '^ENTRY_COUNT=' "$HARNESS_OUTPUT")"
if [ "$ENTRY_COUNT_LINE" != "ENTRY_COUNT=1" ]; then
	echo "[FAIL] 应该恰好雕刻出 1 个文件，实际：$ENTRY_COUNT_LINE"
	FAIL=1
fi

ENTRY_LINE="$(grep '^ENTRY|' "$HARNESS_OUTPUT" || true)"
if [ -z "$ENTRY_LINE" ]; then
	echo "[FAIL] 没有解析出任何 ENTRY 行"
	FAIL=1
else
	RECUP_DIR="$(grep '^RECUP_DIR=' "$HARNESS_OUTPUT" | sed 's/^RECUP_DIR=//')"
	RECOVERED_NAME="$(printf '%s\n' "$ENTRY_LINE" | cut -d'|' -f2)"
	RECOVERED_PATH="$RECUP_DIR/$RECOVERED_NAME"

	if [ ! -f "$RECOVERED_PATH" ]; then
		echo "[FAIL] report.xml 里记录的恢复文件实际不存在：$RECOVERED_PATH"
		FAIL=1
	else
		EXPECTED_HASH="$(shasum -a 256 "$EMBEDDED_JPEG" | cut -d' ' -f1)"
		ACTUAL_HASH="$(shasum -a 256 "$RECOVERED_PATH" | cut -d' ' -f1)"
		if [ "$EXPECTED_HASH" != "$ACTUAL_HASH" ]; then
			echo "[FAIL] 恢复出的文件哈希不匹配：期望 $EXPECTED_HASH，实际 $ACTUAL_HASH"
			FAIL=1
		else
			echo "[OK] 恢复出的文件哈希和嵌入前的原始 JPEG 完全一致：$ACTUAL_HASH"
		fi
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
