/*
 * imperative shell：对一个"已经定位到正确起始偏移"的读取句柄，按扇区
 * 对齐的方式循环读取 total_size 字节，写到 output_path。
 *
 * 这是从 `fat_reader_recover_file`/`exfat_reader_recover_file` 里抽出来的
 * 共享实现——两者原来各自维护一份几乎一模一样的循环，现在改成都调用
 * 这一个。真正的"读"这个动作通过 read_fn 函数指针注入，不在这个文件里
 * 硬编码 CreateFileA/fopen：生产代码里传真实的 Windows ReadFile 或 fread
 * 包装，单元测试里传一个模拟"真实设备只接受扇区对齐长度"这个限制的
 * 内存 mock——这让这整条"扇区对齐读取并写出文件"的逻辑第一次能在 Mac
 * 本地的单元测试/CI 里被真正跑到，而不是只能靠虚拟机真机测试覆盖
 * （详见 docs/开发日志.md 2026-11-05 第 9/11 步的相关记录）。
 *
 * 这个文件本身不含任何 `#ifdef _WIN32` 分支，是完全跨平台的通用代码——
 * 平台相关的部分已经被收窄到只剩"怎么打开/关闭一个句柄、怎么真正调用
 * ReadFile 还是 fread"，由调用方（fat_reader.c/exfat_reader.c）通过
 * read_fn 注入进来。
 *
 * P5 坏道保护（2026-11-05 小树裁决第17步）：新增 max_retries +
 * out_bad_sector_log 两个参数。max_retries=0 且 out_bad_sector_log=NULL
 * 时行为和原来完全一样（读失败立刻返回 -1）——这是所有已有调用方不想
 * 要坏道容忍能力时应该传的值，保证不破坏任何已经验证过的行为。只有
 * out_bad_sector_log 不是 NULL 时，重试次数用完后才会"跳过这一块（用
 * 0 填充，保持文件大小/后续偏移不变）+记一笔坏道日志+继续"，而不是让
 * 整次恢复直接失败——这是否要容忍坏道完全由这个指针是不是 NULL 决定，
 * 不是靠 max_retries 是不是 0 这种容易搞混的隐含规则。
 */
#ifndef ENGINE_SHELL_ALIGNED_DEVICE_READER_H
#define ENGINE_SHELL_ALIGNED_DEVICE_READER_H

#include <stddef.h>
#include <stdint.h>

#include "../core/bad_sector_log.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 从 context 代表的句柄里读取最多 requested_size 字节到 buffer，返回
 * 实际读到的字节数（0 表示到达末尾，或者——这正是我们要适配的真实
 * 设备限制——请求长度不是扇区大小整数倍时可能直接返回 0）。
 */
typedef size_t (*aligned_device_read_fn)( void *context, void *buffer, size_t requested_size );

/* 调用方需要已经把底层句柄 seek 到正确的起始偏移，这个函数只负责
 * "按扇区对齐的方式把 total_size 字节读出来、写进 output_path"这一件事。
 *
 * sector_size == 0 表示不需要做扇区对齐（调用方确定这不是一个有扇区
 * 对齐限制的真实设备，比如普通镜像文件）。
 *
 * max_retries：每一块读取失败后最多重试几次，配合 out_bad_sector_log
 * 使用（见上方文件头注释）。不需要坏道容忍能力的调用方传 0。
 *
 * out_bad_sector_log：非 NULL 时启用坏道容忍——重试耗尽后跳过这一块
 * （写 0 填充，保持 total_size 不变）并追加一条记录，不中断整个拷贝；
 * 为 NULL 时，重试耗尽直接返回 -1（和原来的行为一致）。
 *
 * 返回 1 成功，-1 失败（写入失败，或者未启用坏道容忍时读取失败）。
 */
int aligned_device_reader_copy_to_file(
     aligned_device_read_fn read_fn,
     void *context,
     size_t sector_size,
     uint64_t total_size,
     uint32_t max_retries,
     bad_sector_log_t *out_bad_sector_log,
     const char *output_path );

/* 读一整块固定大小的数据到调用方提供的 buffer（不负责分配/释放），
 * 用于"一次性读一整块"的场景（比如读一个 FAT/exFAT 簇），和上面
 * `aligned_device_reader_copy_to_file` 面向"流式拷贝一大段数据到文件"
 * 的场景不同——这里没有内部分块循环，调用方自己保证 block_size 在
 * 一次 read_fn 调用里能处理完（簇大小天然是扇区大小的整数倍，不需要
 * 额外的扇区对齐计算）。失败重试+跳过的决策逻辑复用同一套
 * `bad_sector_policy_decide`，不另起一套（2026-11-05 小树裁决第18步，
 * P5 坏道保护推广到"单次读一块"这类场景，例如簇读取）。
 *
 * block_offset_for_log：这次读取在调用方语境里的"偏移"，含义由调用方
 * 决定（可以是文件内偏移，也可以是设备绝对偏移），这个函数只负责原样
 * 记进坏道日志，不解读它的含义。
 *
 * 返回 1 成功（buffer 里是真实数据；或者——仅当 out_bad_sector_log 非
 * NULL 且重试耗尽时——是 0 填充的占位内容，且已经记了一笔坏道日志）；
 * 返回 -1 表示彻底失败（参数不合法，或者 out_bad_sector_log 为 NULL
 * 时读取失败）。
 */
int aligned_device_reader_read_block(
     aligned_device_read_fn read_fn,
     void *context,
     uint8_t *buffer,
     size_t block_size,
     uint32_t max_retries,
     uint64_t block_offset_for_log,
     bad_sector_log_t *out_bad_sector_log );

#ifdef __cplusplus
}
#endif

#endif /* ENGINE_SHELL_ALIGNED_DEVICE_READER_H */
