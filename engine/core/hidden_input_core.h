/*
 * functional core（B11a，BitLocker交互式隐藏输入渠道，2026-11-05任务
 * 拆解排期）：一个字符一个字符地喂给这个状态机，它决定"这个字符该不该
 * 被接受、接受之后该采取什么动作"，不碰任何终端/平台API（不知道
 * "怎么不回显地读一个字符"这件事怎么做——那是 B11b
 * engine/shell/hidden_input_reader.c 的职责，Windows用`_getch()`、
 * POSIX用`tcgetattr`改终端属性，两套完全不同的平台实现）。这个拆分
 * 让"输入该怎么被解释"这个逻辑能完整TDD覆盖，不需要真实终端就能验证
 * 边界情况（缓冲区满、连续退格、空缓冲区退格等），这些场景在真实终端
 * 上手动触发反而麻烦、还容易漏测。
 *
 * 用定长缓冲区、不用 malloc/realloc——和 engine/core/bitlocker_credential.h
 * 的 secure_credential_t 同样的理由（realloc挪动旧内存可能留下明文
 * 残留的堆拷贝，固定缓冲区从源头避免这个问题）。
 *
 * 清零责任：这个文件不调用任何清零API（和 bitlocker_credential.c
 * 同样的"functional core不碰平台API"原则）——B11b的终端外壳用完这个
 * 状态机之后，要自己调用 engine/shell/secure_memory.c 的
 * secure_memory_zero() 清零 state->buffer，不能指望这个文件帮它做。
 */
#ifndef ENGINE_CORE_HIDDEN_INPUT_CORE_H
#define ENGINE_CORE_HIDDEN_INPUT_CORE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HIDDEN_INPUT_MAX_LENGTH 512

typedef struct
{
	char   buffer[ HIDDEN_INPUT_MAX_LENGTH ];
	size_t length;
}
hidden_input_state_t;

typedef enum
{
	/* 字符被正常接受（或者是退格/已经是空缓冲区这种无副作用的情况），
	 * 继续等下一个字符，不是终止信号 */
	HIDDEN_INPUT_ACTION_CONTINUE = 0,

	/* 遇到回车（'\r' 或 '\n'，两种都认，覆盖Windows `_getch()`和POSIX
	 * 终端两种不同习惯）——输入到此结束，state->buffer/length就是最终
	 * 结果，调用方应该停止继续喂字符 */
	HIDDEN_INPUT_ACTION_COMPLETE = 1,

	/* 遇到 Ctrl+C（0x03）——用户主动放弃这次输入，调用方应该把这次操作
	 * 当成"用户取消"处理（不是错误），并清零state->buffer */
	HIDDEN_INPUT_ACTION_CANCELLED = 2,

	/* 字符没有被接受：缓冲区已经满了又来一个可打印字符，或者是这个状态
	 * 机不认识的控制字符（除了退格/回车/Ctrl+C之外的其它控制字符）。
	 * state不会被这次调用改变，调用方可以选择原样忽略继续等下一个
	 * 字符，不是致命错误 */
	HIDDEN_INPUT_ACTION_REJECTED = -1
}
hidden_input_action_t;

/* 初始化/重置状态机：length归零。不做secure zero（这时候buffer里还
 * 没有真实输入过的敏感内容，不需要）。 */
void hidden_input_core_init(
      hidden_input_state_t *state );

/* 喂一个字符（已经是该平台终端层拿到的、未回显的原始字符/控制码，
 * 转换规则由B11b的终端外壳负责，这里只认'\b'/0x7F退格、'\r'/'\n'回车、
 * 0x03 Ctrl+C、0x20~0x7E可打印ASCII这几类，不认其它字节）。
 *
 * 返回对应的 hidden_input_action_t，调用方据此决定下一步动作。
 */
hidden_input_action_t hidden_input_core_feed(
      hidden_input_state_t *state,
      int c );

#ifdef __cplusplus
}
#endif

#endif /* ENGINE_CORE_HIDDEN_INPUT_CORE_H */
