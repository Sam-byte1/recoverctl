/*
 * imperative shell（B11b，BitLocker交互式隐藏输入渠道，2026-11-05任务
 * 拆解排期）：真正"怎么不回显地从终端读一个字符"的平台相关实现，喂给
 * B11a `engine/core/hidden_input_core.c` 的纯状态机去解释。
 *
 * 两套完全不同的平台实现：
 * - Windows：`<conio.h>` 的 `_getch()`——本身就不回显，不需要改任何
 *   终端/控制台属性。扩展键（方向键等，`_getch()`先返回0或0xE0这个
 *   前缀字节）对密码输入没有意义，直接连同紧跟着的第二个字节一起吞掉
 *   丢弃，不喂给状态机，避免在输入流里留下一个会被状态机当成垃圾
 *   字节拒绝、但其实是半个按键的残留。
 * - POSIX：`tcgetattr`/`tcsetattr` 关掉 `ECHO`（回显）+`ICANON`（行
 *   缓冲，否则要等一整行才能逐字符处理退格）+`ISIG`（否则Ctrl+C会
 *   被终端驱动直接转成SIGINT杀掉/中断进程，状态机永远收不到0x03这个
 *   字节，CANCELLED分支就是死代码）。读取用`read()`逐字节读，不用
 *   `fgetc`（标准I/O的内部缓冲在原始模式下行为不是我们需要的）。无论
 *   成功、失败还是中途任何错误路径，都要在返回前把终端属性恢复成
 *   进入函数之前的原样——这是这个函数不能放过的责任，调用方没有能力
 *   替它做。
 *
 * Ctrl+C在Windows下的实际行为还没有在真机上确认过（Windows控制台的
 * Ctrl+C默认处理方式和POSIX的SIGINT机制不是同一套东西，`_getch()`
 * 在默认控制台模式下收到Ctrl+C时具体表现为"返回0x03这个字节"还是
 * "触发控制台的Ctrl+C处理器、`_getch()`调用被打断"，这两种可能都有
 * 文档支持，没有在真机上实际按过键确认是哪一种）——这正是B12里提前
 * 写明的"唯一一个无法用`prlctl exec`脚本化验证、必须在VM控制台里实际
 * 敲键盘确认"的环节，这里先按"假设_getch()会返回0x03"实现，真机验证
 * 时如果发现实际行为不同，再按实测结果调整，不是遗漏。
 */
#ifndef ENGINE_SHELL_HIDDEN_INPUT_READER_H
#define ENGINE_SHELL_HIDDEN_INPUT_READER_H

#include "../core/hidden_input_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* prompt_message非NULL时先原样打印（不带换行，让用户输入紧跟在提示
 * 后面），然后逐字符读取、不回显，直到用户按回车或Ctrl+C。
 *
 * 返回1成功（*out填充了hidden_input_state_t，length/buffer是用户输入
 * 的明文内容——调用方用完之后必须自己调用engine/shell/secure_memory.c
 * 的secure_memory_zero()清零，这个函数不会替调用方做，和B5/B10的既有
 * 约定一致）；0表示用户按Ctrl+C主动取消（*out已经内部清零过，调用方
 * 不需要再清零一次，可以直接当成"用户放弃"处理，不是错误）；-1表示
 * 系统级错误（终端属性读取/设置失败等）。
 */
int hidden_input_reader_read(
     const char *prompt_message,
     hidden_input_state_t *out );

#ifdef __cplusplus
}
#endif

#endif /* ENGINE_SHELL_HIDDEN_INPUT_READER_H */
