/*
 * functional core：拼出喂给 PhotoRec `/cmd <目标> <命令串>` 的命令串本身。
 *
 * 背景（2026-11-05 第27→28步，P4 调研+小树裁决）：第27步实测确认命令串
 * 必须显式带上分区表类型 token（没有可识别分区表/我们自己已经切出单个
 * 分区字节范围时用 `partition_none`；目标本身还是一整块带 MBR/GPT 的盘
 * 时传对应 token，比如 `partition_i386`），不能只传 `search` 指望它跳过
 * 分区表确认这一步——这不是 PhotoRec 的缺陷，是一个用法纪律问题，这个
 * 模块的职责就是把这条纪律用代码固定下来，不依赖调用方每次记得拼对。
 *
 * 只负责拼"命令串"这一个字符串，不负责拼完整的进程参数列表（那是
 * shell 层 `photorec_invoker.c` 的职责，因为完整参数列表还涉及可执行
 * 文件路径、目标路径、输出目录这些需要真实文件系统信息的部分）。
 */
#ifndef ENGINE_CORE_PHOTOREC_COMMAND_BUILDER_H
#define ENGINE_CORE_PHOTOREC_COMMAND_BUILDER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* arch_type_token 为 NULL 或空字符串时，表示调用方明确选择不传分区表
 * 类型 token（目前项目内部不会这样调用——小树裁决要求两种情况都必须
 * 显式带 token——这里仍然保留"不传"的行为，只拼出裸的 "search"，是为了
 * 这个纯函数本身的可测试性/诚实性：它只负责"按输入拼接"，要不要传
 * token 这个决策权在调用方，不应该在这一层悄悄加隐藏默认值）。
 *
 * 返回 1 成功，-1 表示 out_buf 为 NULL 或装不下结果。
 */
int photorec_command_builder_build_cmd_run(
     const char *arch_type_token,
     char *out_buf,
     size_t out_buf_size );

#ifdef __cplusplus
}
#endif

#endif /* ENGINE_CORE_PHOTOREC_COMMAND_BUILDER_H */
