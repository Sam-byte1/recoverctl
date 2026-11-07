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
#include <stdint.h>

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

/* G8（PhotoRec接入GUI，2026-11-06小树裁决③）：纯加法——不改
 * photorec_invoker_run()一行代码，新增一对平行函数让调用方能在"启动"
 * 和"等待退出"之间插入自己的逻辑（GUI场景：启动后立刻把控制权交还给
 * CarveWorker的事件循环，用户随时可以点"取消"）。
 *
 * 句柄字段用intptr_t存，不在这个跨平台公开头文件里直接放
 * Windows HANDLE/POSIX pid_t类型——调用方（含这份头文件自己的三个
 * 函数）不需要关心具体类型，只在.c内部按平台转换，和
 * gui/scan_worker.h存BitLocker已解锁会话用intptr_t*同样的理由。 */
typedef struct
{
	/* Windows：CreateProcessA产出的进程句柄（HANDLE）；
	 * POSIX：posix_spawn产出的pid_t。 */
	intptr_t process_handle;
#ifdef _WIN32
	/* 只有Windows分支需要——CreateProcessA同时产出一个线程句柄，
	 * 用完要和进程句柄一起CloseHandle */
	intptr_t thread_handle;
	/* 只有Windows分支需要——启动时临时改过的TERM/TERMINFO环境变量，
	 * 要在wait()真正等到退出之后才能安全还原（提前还原的话，如果
	 * PhotoRec还没来得及读取这两个环境变量就可能读到错误的值） */
	char previous_term[ 256 ];
	char previous_terminfo[ 900 ];
	int had_previous_term;
	int had_previous_terminfo;
#endif
	/* 两平台都需要——wait()要用这几个字段扫描输出目录、算出
	 * recup_dir_path，和photorec_invoker_run()内部的逻辑一致 */
	char parent_dir[ 768 ];
	char base_name[ 256 ];
	char output_dir_base[ 1024 ];
	int suffix_before;
}
photorec_invoke_handle_t;

/* 启动PhotoRec子进程但不等待退出，成功时*out_handle可以立刻传给
 * photorec_invoker_wait()（在另一个线程里调用，阻塞到退出为止）或者
 * photorec_invoker_cancel()（可以从第三个线程调用，强制终止）。
 *
 * 返回1成功（子进程已经在跑），-1失败（参数不合法/子进程启动失败），
 * 失败时*out_handle的内容未定义，调用方不应该再传给wait/cancel。
 */
int photorec_invoker_run_async(
     const char *photorec_executable_path,
     const char *target_path,
     const char *arch_type_token,
     const char *output_dir_base,
     photorec_invoke_handle_t *out_handle );

/* 阻塞等待一个已经用run_async()启动的子进程退出，填充out_result（和
 * photorec_invoker_run()给调用方看到的结果形状完全一致）。只能调用
 * 一次——调用之后handle内部持有的进程/线程句柄已经被关闭，不能重复
 * wait，也不能在wait返回之后再调用cancel。
 *
 * 返回1成功等到退出（不代表退出码是0，查out_result->exit_code），
 * -1失败（等待系统调用本身出错，这种情况下out_result的内容未定义）。
 */
int photorec_invoker_wait(
     photorec_invoke_handle_t *handle,
     photorec_invoke_result_t *out_result );

/* 强制终止一个已经用run_async()启动、还没有被wait()等到退出的子
 * 进程——Windows走TerminateProcess，POSIX先发SIGTERM给一个短暂的
 * 优雅退出窗口、超时后SIGKILL兜底。可以从调用run_async()的线程之外
 * 的另一个线程安全调用（这个函数只读handle->process_handle这一个
 * 早就写好、之后不再变化的字段，不修改handle的任何内容）。
 *
 * 调用之后调用方仍然要调用一次photorec_invoker_wait()来回收子进程、
 * 关闭句柄、让wait()按"异常退出"的exit_code给出一个确定的结果——
 * cancel()本身不负责这些收尾工作，职责只是"让子进程尽快死掉"。
 */
void photorec_invoker_cancel(
     photorec_invoke_handle_t *handle );

#ifdef __cplusplus
}
#endif

#endif /* ENGINE_SHELL_PHOTOREC_INVOKER_H */
