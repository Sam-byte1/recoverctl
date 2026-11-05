/*
 * imperative shell：实际把 PhotoRec 当子进程拉起来跑一次
 * `/cmd <目标> <命令串>`，等它跑完，再找出它自己创建的输出目录
 * （`<output_dir_base>.N`，N 是 PhotoRec 自己决定的递增序号——已经在
 * 第27步实测确认：同一个 base 路径反复跑，N 会在已存在的编号基础上
 * 递增，不会覆盖之前的结果）。
 *
 * 这是真正的进程/文件系统 I/O，不走 TDD（命令串怎么拼这个纯计算部分
 * 已经拆到 `engine/core/photorec_command_builder.c` 单独做 TDD 覆盖），
 * 靠端到端跑一遍真实的 `photorec` 二进制 + 哈希校验来验证，和
 * `file_writer.c` 同样的测试哲学。
 *
 * 跨平台现状（2026-11-05 第28步排期 D2，先做 POSIX/Mac）：Windows 上
 * 怎么拉起子进程、等待退出，需要上 VM 用真实 `photorec.exe` 验证之后
 * 才能确认实现正确（不能凭对 CreateProcess API 的理解就当作已完成——
 * 这是项目一贯"不伪造完成"的纪律），放在 D6 真机验证阶段一起做，这里
 * 先用一个明确标注"未实现"的占位，不是悄悄假装能跑。
 */
#ifndef ENGINE_SHELL_PHOTOREC_INVOKER_H
#define ENGINE_SHELL_PHOTOREC_INVOKER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
	/* PhotoRec 这次实际创建、写入恢复结果的目录（`<output_dir_base>.N`） */
	char recup_dir_path[ 1024 ];

	/* 子进程的退出码；0 不代表"找到了文件"，只代表 PhotoRec 自己正常
	 * 跑完退出，有没有恢复出东西要看 recup_dir_path 下的 report.xml
	 * （用 engine/core/photorec_report_parser.c 解析） */
	int exit_code;
}
photorec_invoke_result_t;

/* 跑一次 PhotoRec：
 *   <photorec_executable_path> /log /d <output_dir_base> /cmd <target_path> <cmd_run>
 * cmd_run 由 arch_type_token 通过 photorec_command_builder_build_cmd_run 拼出
 * （本函数内部会调用，调用方不需要自己先拼好）。
 *
 * stdin 固定重定向自系统的"空输入"设备（第27步实测确认的关键前提：
 * 必须保证 PhotoRec 遇到任何意外的交互式等待时会立刻拿到 EOF 而不是
 * 真的挂起），stdout/stderr 不继承调用方终端（避免 PhotoRec 自己的
 * ncurses 转义序列污染调用方日志）。
 *
 * 返回 1 表示子进程成功启动并等到退出（不代表退出码是 0，查
 * out_result->exit_code），-1 表示参数不合法、子进程启动失败、或者
 * （POSIX 实现里）等待退出失败。
 */
int photorec_invoker_run(
     const char *photorec_executable_path,
     const char *target_path,
     const char *arch_type_token,
     const char *output_dir_base,
     photorec_invoke_result_t *out_result );

#ifdef __cplusplus
}
#endif

#endif /* ENGINE_SHELL_PHOTOREC_INVOKER_H */
