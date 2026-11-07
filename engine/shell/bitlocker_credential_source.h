/*
 * imperative shell（B5，2026-11-05 任务拆解排期）：从外部渠道拿到一份
 * BitLocker解锁凭据，灌进 `secure_credential_t`/相关缓冲区。这一版只
 * 实现 `source=file`（世杰拍板"文件读取优先先做"），命令行参数（B10）
 * 和交互式隐藏输入（B11）渠道留到后面叠加，接口形状已经为它们预留了
 * 位置（`bitlocker_resolved_credential_t` 这个产出类型和渠道无关）。
 *
 * 四种凭据类型各自的文件格式约定（第28步BitLocker任务拆解里定下的）：
 * - 密码文件：文本，第一行即明文密码本身，不trim（密码里理论上允许
 *   有前后空格，不应该被这个读取层悄悄改掉）。
 * - 恢复密钥文件：文本，第一行是48位数字（允许带`-`分隔符），读出来
 *   之后交给 engine/core/bitlocker_credential.c 的规范化函数处理。
 * - 启动密钥文件：官方 .BEK 二进制文件本身——这个类型我们完全不读取
 *   文件内容，直接把路径原样转给 `libbde_volume_read_startup_key`
 *   （它自己打开文件读取密钥），我们不经手密钥字节。
 * - FVEK文件：文本，格式 `<FVEK十六进制>:<TWEAK十六进制>`，冒号后面
 *   的TWEAK部分可以省略（对应AES-CBC场景没有独立TWEAK密钥）。
 *
 * 读取过程中用到的临时缓冲区，处理完立刻用 engine/shell/secure_memory.c
 * 清零——这是世杰"现在就要主动清零"这条拍板决策在凭据读取这一层的
 * 具体落地。
 */
#ifndef ENGINE_SHELL_BITLOCKER_CREDENTIAL_SOURCE_H
#define ENGINE_SHELL_BITLOCKER_CREDENTIAL_SOURCE_H

#include <stddef.h>
#include <stdint.h>

#include "../core/bitlocker_credential.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
	BITLOCKER_CREDENTIAL_TYPE_PASSWORD = 1,
	BITLOCKER_CREDENTIAL_TYPE_RECOVERY_KEY,
	BITLOCKER_CREDENTIAL_TYPE_STARTUP_KEY,
	BITLOCKER_CREDENTIAL_TYPE_FVEK
}
bitlocker_credential_type_t;

#define BITLOCKER_FVEK_MAX_LENGTH 64
#define BITLOCKER_STARTUP_KEY_PATH_MAX_LENGTH 1024

/* 已经从某个渠道（当前只有file）拿到、可以直接喂给B6解锁层的凭据。
 * 调用方根据 type 字段决定看哪些字段——不是每种类型都会填满所有字段。
 */
typedef struct
{
	bitlocker_credential_type_t type;

	/* PASSWORD / RECOVERY_KEY 用这个（RECOVERY_KEY存的是规范化之后的
	 * 纯数字字符串，不是文件原始内容） */
	secure_credential_t text;

	/* STARTUP_KEY 用这个——只是路径字符串，不是密钥内容本身 */
	char startup_key_file_path[ BITLOCKER_STARTUP_KEY_PATH_MAX_LENGTH ];

	/* FVEK 用这两个 */
	uint8_t fvek[ BITLOCKER_FVEK_MAX_LENGTH ];
	size_t fvek_length;
	uint8_t tweak[ BITLOCKER_FVEK_MAX_LENGTH ];
	size_t tweak_length;
}
bitlocker_resolved_credential_t;

/* 返回 1 成功，-1 失败（文件打不开、格式不对、内容超出缓冲区容量等，
 * 具体原因打到 stderr——这一层直接用 fprintf 而不是走 libbde_error_t，
 * 因为这些错误（文件读取失败、格式校验失败）都发生在调用任何libbde
 * API之前，不是libbde报出来的错误）。
 *
 * 成功或失败都不会在 *out 里留下任何未清零的中间读取缓冲区残留——
 * 读取用的栈上临时区域处理完立刻清零，不依赖调用方记得清理。
 */
int bitlocker_credential_source_load_from_file(
     bitlocker_credential_type_t type,
     const char *file_path,
     bitlocker_resolved_credential_t *out );

/* B10（2026-11-05 任务拆解排期）：命令行参数渠道——argument_value是
 * 凭据内容本身（不是路径），直接解析，不碰任何文件I/O。STARTUP_KEY
 * 类型不支持这个渠道（返回-1），因为libbde的`read_startup_key` API
 * 本身只接受文件路径，不接受密钥内容字符串。
 *
 * 安全提示（不是这个函数自己能解决的，调用方/文档必须明确告知用户）：
 * 这个渠道下，凭据会短暂出现在shell历史记录和系统进程列表里——这是
 * "命令行参数"这个渠道本身固有的暴露面，调用方在argv用完之后原地
 * 覆写对应槽位只能缩短暴露窗口，不能消除"曾经可见过"这个事实。
 */
int bitlocker_credential_source_load_from_cli_argument(
     bitlocker_credential_type_t type,
     const char *argument_value,
     bitlocker_resolved_credential_t *out );

/* B11c（2026-11-05 任务拆解排期）：交互式隐藏输入渠道——没有
 * argument_value参数，内容本身就是从终端现场、不回显地读出来的（见
 * engine/shell/hidden_input_reader.c）。STARTUP_KEY类型不支持这个
 * 渠道（返回-1），和B10命令行参数渠道同样的理由：libbde的
 * `read_startup_key` API本身只接受文件路径，不接受密钥内容字符串
 * ——而且文件路径本身也不是需要"隐藏"的敏感内容，用这个渠道去问一个
 * 路径没有意义。
 *
 * 返回 1 成功，0 表示用户在输入过程中按Ctrl+C主动取消（不是错误，
 * *out不会被填充任何内容，调用方可以直接当成"用户放弃"处理），-1
 * 表示失败（STARTUP_KEY类型、终端读取系统错误、格式校验失败等）。
 */
int bitlocker_credential_source_load_from_interactive(
     bitlocker_credential_type_t type,
     bitlocker_resolved_credential_t *out );

#ifdef __cplusplus
}
#endif

#endif /* ENGINE_SHELL_BITLOCKER_CREDENTIAL_SOURCE_H */
