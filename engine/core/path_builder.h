/*
 * functional core：把路径片段逐个拼成一个 Windows 风格的相对路径字符串
 * （反斜杠分隔）。提供两个方向的拼接：
 * - "从叶子往根走"场景（NTFS 路径重建用）：沿父目录链一层层往上走，
 *   每发现一层父目录名就拼到已有路径前面
 * - "从根往叶子走"场景（FAT32/exFAT 子目录递归用，2026-11-05 小树裁决
 *   第14步追加）：从根目录开始往下递归，每进入一层子目录就把子目录名
 *   拼到已有路径后面
 *
 * 之所以单独抽出这两个纯函数：路径重建/目录递归的整体流程天然需要真实
 * 的卷 I/O（每一跳都要打开一个新的目录），没法整体做成纯函数去 TDD；
 * 但"把一层目录名拼接到已有路径"这个字符串操作本身是纯逻辑，可以、也
 * 应该独立出来用已知输入输出做 TDD 覆盖，不用依赖真实卷就能验证"拼接
 * 顺序对不对、边界情况处理得对不对"。
 */
#ifndef ENGINE_CORE_PATH_BUILDER_H
#define ENGINE_CORE_PATH_BUILDER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 把 segment 拼到 path 当前内容的前面，中间用反斜杠分隔，结果写回 path。
 * 用于"从叶子往根走"场景：每发现一层父目录名，就把它拼到已经拼好的
 * 路径最前面（比如已有 "sub\\file.txt"，新发现父目录是 "a_directory"，
 * 拼完变成 "a_directory\\sub\\file.txt"）。
 *
 * path 既是输入也是输出（原地修改），path_buffer_size 是这块内存的
 * 总容量。如果 path 原本是空字符串，拼接后就只是 segment 本身，不会
 * 多出一个多余的反斜杠。
 *
 * 返回 1 成功，-1 表示参数不合法（path/segment 为 NULL）或者拼接后
 * 会超出 path_buffer_size（这种情况下 path 内容不会被破坏性地截断，
 * 保持调用前的样子，调用方可以选择报告"路径太长，只显示文件名"）。
 */
int path_builder_prepend_segment(
     char *path,
     size_t path_buffer_size,
     const char *segment );

/* 把 segment 拼到 path 当前内容的后面，中间用反斜杠分隔，结果写回 path。
 * 用于"从根往叶子走"场景：每进入一层子目录，就把子目录名拼到已经拼好
 * 的路径最后面（比如已有 "SUBDIR"，新进入的子目录是 "DEEPER"，拼完
 * 变成 "SUBDIR\\DEEPER"）。
 *
 * path 既是输入也是输出（原地修改），path_buffer_size 是这块内存的
 * 总容量。如果 path 原本是空字符串，拼接后就只是 segment 本身，不会
 * 多出一个多余的反斜杠（和 prepend 版本的边界行为一致）。
 *
 * 返回 1 成功，-1 表示参数不合法（path/segment 为 NULL）或者拼接后
 * 会超出 path_buffer_size（这种情况下 path 内容不会被破坏性地截断，
 * 保持调用前的样子）。
 */
int path_builder_append_segment(
     char *path,
     size_t path_buffer_size,
     const char *segment );

#ifdef __cplusplus
}
#endif

#endif /* ENGINE_CORE_PATH_BUILDER_H */
