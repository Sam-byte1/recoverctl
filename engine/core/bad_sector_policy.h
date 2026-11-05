/*
 * functional core：P5 坏道保护的"决策大脑"——给定这次读取已经失败了
 * 第几次、允许重试几次，决定接下来是继续重试还是放弃（交给调用方跳过
 * 这一块、记一笔坏道日志）。不做任何真实 I/O、不碰时间（不读系统时钟、
 * 不 sleep），纯粹是一个确定性的查表判断，方便 TDD 完整覆盖。
 *
 * 背景（2026-11-05 小树裁决第17步）：P5 的解锁条件（FAT 深度增量的
 * 不确定性被消除）已经满足，这是第一片实现——先做"决定重试还是跳过"
 * 这个最小、最独立的判断单元，真正接到真实读取循环里是 shell 层
 * （engine/shell/aligned_device_reader.c）的职责，不在这个模块里。
 */
#ifndef ENGINE_CORE_BAD_SECTOR_POLICY_H
#define ENGINE_CORE_BAD_SECTOR_POLICY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
	BAD_SECTOR_ACTION_RETRY = 1,  /* 还没用完重试次数，应该再试一次 */
	BAD_SECTOR_ACTION_SKIP  = 2   /* 重试次数已经用完，应该放弃这一块，
	                                * 跳过并记录坏道日志，不能让整次
	                                * 扫描/恢复因此中断 */
}
bad_sector_action_t;

/* attempt_number：刚刚失败的是第几次尝试，从 1 开始计数（第一次读取
 * 失败传 1，不是传 0）。
 * max_retries：除了第一次尝试之外，还允许重试几次（0 表示读一次失败
 * 就直接放弃，不重试）。
 *
 * 总允许尝试次数 = 1（首次）+ max_retries（重试），所以判断条件是
 * attempt_number <= max_retries 时还能再试一次。
 */
bad_sector_action_t bad_sector_policy_decide(
     uint32_t attempt_number,
     uint32_t max_retries );

#ifdef __cplusplus
}
#endif

#endif /* ENGINE_CORE_BAD_SECTOR_POLICY_H */
