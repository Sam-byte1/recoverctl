#!/bin/sh
# P3-libvshadow离线解析 验收脚本（2026-11-05 第25步）。
#
# 范围提醒：这个脚本验证的是"离线解析一个镜像文件里记录的历史卷影副本"
# 这条能力本身（list-shadows-offline/scan-shadow-offline/recover-shadow-offline），
# 不是"VSS最小可用版"（list-shadows/scan-shadow/recover-shadow，第22步，
# 只能在Windows上查当前活系统的卷影副本）——两者命令名用 -offline 后缀
# 区分，技术路径也完全不同（这个走 libvshadow 解析镜像里的VSS控制结构，
# 不依赖任何Windows系统API，跨平台）。
#
# 测试镜像 vss.raw 来源见 engine/tests/fixtures/vshadow-offline/README.md
# （log2timeline/dfvfs项目的公开测试数据，Apache-2.0，不是我们自己构造的）。
#
# 如实记录一个局限：这个公开镜像的两个store都不包含已删除文件（已经
# 确认过，不是凑巧没测到）——所以这个脚本只能验证"打开镜像→枚举
# store→用libvshadow重建出的字节喂给libfsntfs→扫描不报错"这条机制链路
# 本身是可靠的，**不能**验证"真的找到并恢复出一个历史快照里的已删除
# 文件"这个更完整的端到端场景。真实找到+恢复过已删除候选项的验证，
# 是用真实Windows虚拟机的C盘卷（本来就因为长期使用、经历过大量
# 文件创建删除而有真实的卷影副本历史数据）做的，详见
# docs/开发日志.md 第25步执行记录，不在这个Mac侧持久化脚本里
# （VM上的验证是一次性的手工过程，不是每次回归都要重新对真机做的
# 日常测试）。
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../../.." && pwd)"
FIXTURE_DIR="$ROOT_DIR/engine/tests/fixtures/vshadow-offline"
RECOVERCTL="$ROOT_DIR/cli/recoverctl"
IMAGE="$FIXTURE_DIR/vss.raw"
FAIL=0

echo "=========================================="
echo " P3-libvshadow离线解析 端到端验证"
echo "=========================================="

if [ ! -x "$RECOVERCTL" ]; then
	echo "recoverctl 还没编译，先跑 cli/build.sh"
	exit 1
fi

if [ ! -f "$IMAGE" ]; then
	echo "测试镜像不存在: $IMAGE（见 $FIXTURE_DIR/README.md 的下载方式）"
	exit 1
fi

echo
echo "[1/2] 列出镜像里的历史卷影副本: $IMAGE"
LIST_OUTPUT="$("$RECOVERCTL" list-shadows-offline "$IMAGE")"
printf '%s\n' "$LIST_OUTPUT"

if ! printf '%s\n' "$LIST_OUTPUT" | grep -q "\[0\] 创建时间=2021-05-01"; then
	echo "  [FAIL] 没有正确列出 store 0 的创建时间（应该是 2021-05-01 开头）"
	FAIL=1
fi

if ! printf '%s\n' "$LIST_OUTPUT" | grep -q "\[1\] 创建时间=2021-05-01"; then
	echo "  [FAIL] 没有正确列出 store 1 的创建时间（应该是 2021-05-01 开头）"
	FAIL=1
fi

echo
echo "[2/2] 扫描两个 store，确认 libvshadow→libfsntfs 这条链路不报错"
for STORE_INDEX in 0 1; do
	SCAN_OUTPUT="$("$RECOVERCTL" scan-shadow-offline "$IMAGE" "$STORE_INDEX")"
	printf '%s\n' "$SCAN_OUTPUT"

	if ! printf '%s\n' "$SCAN_OUTPUT" | grep -q "正在扫描 (NTFS)"; then
		echo "  [FAIL] store $STORE_INDEX 没有被正确识别成NTFS卷（libvshadow重建出的字节喂给libfsntfs应该能正常解析出NTFS结构）"
		FAIL=1
	fi
done

echo
echo "[3/3] 汇总结果"

if [ "$FAIL" -eq 0 ]; then
	echo
	echo "=========================================="
	echo " 全部通过：P3-libvshadow离线解析 验收成功"
	echo " （注：这个公开测试镜像不含已删除文件，本脚本只验证机制链路"
	echo "  可靠，真实找到+恢复候选项的验证见开发日志第25步真机记录）"
	echo "=========================================="
	exit 0
else
	echo
	echo "=========================================="
	echo " 存在失败项，P3-libvshadow离线解析 验收未通过"
	echo "=========================================="
	exit 1
fi
