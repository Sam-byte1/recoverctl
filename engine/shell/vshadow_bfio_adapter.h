/*
 * imperative shell：给 libfsntfs 提供一个自定义的 libbfio I/O 句柄，让它
 * 读到的不是一块活卷的原始字节，而是 libvshadow 解析出来的某个历史卷影
 * 副本（store）的重建内容（2026-11-05 小树裁决第25步，P3-libvshadow离线
 * 解析）。
 *
 * 背景（第24→25步调研纠偏过程，详见 docs/开发日志.md）：第一次调研时
 * grep 公开头文件用的正则只匹配返回值是 int 的函数，漏看了
 * `libvshadow_store_read_buffer_at_offset`/`seek_offset`/`get_size` 这组
 * 返回 ssize_t/off64_t 的函数——这组函数组合起来就是一套完整的、类似
 * "打开一个文件就能读"的接口，libvshadow 内部已经自己实现好了"这段数据
 * 有没有被覆盖过、该从写时复制(COW)差异区读还是从卷原始位置读"这整套
 * 地址重映射逻辑，调用方完全不需要自己解析 `libvshadow_block_get_values`
 * 之类的数据块列表。独立探测程序+真实公开测试镜像（vss.raw）验证过：
 * 在偏移0和偏移10MB两处都能正确读出字节，不是只在"碰巧没被覆盖"的
 * 简单位置凑巧能用。
 *
 * 这正是这个文件存在的原因：把 `libvshadow_store_t` 包装成和
 * `ntfs_bfio_adapter.c` 里 `ntfs_bfio_context_t` 同样形状的 libbfio
 * 句柄，核心的"膨胀对齐+重试+坏道容忍"算术（`ntfs_bfio_read_core`）
 * 原样复用、一行不改——这次真正新增的代码只是"怎么把字节读出来"这一步
 * 换了个来源（从 `ReadFile`/`fread` 换成 `libvshadow_store_read_buffer_at_offset`），
 * 和 NTFS 直接坏道容忍（第20步）比，工作量级别更接近 VSS 最小可用版
 * （第22步，V1-V7），不是另起一套独立的大战线。
 *
 * 调用方（cli/main.c）负责用 `libvshadow_volume_open`+`libvshadow_volume_get_store`
 * 先拿到一个已经打开的 `libvshadow_store_t*`，这个模块不负责管理它的
 * 生命周期（不在 initialize 里打开、不在 free 时关闭），和 NTFS 版本里
 * "path 在 open 回调里才真正打开"的做法不同——因为这里的"卷"本身要先
 * 经过 libvshadow 解析出 store 列表、让调用方选一个，这个选择发生在构造
 * 这个适配器之前，不是适配器自己的职责。
 */
#ifndef ENGINE_SHELL_VSHADOW_BFIO_ADAPTER_H
#define ENGINE_SHELL_VSHADOW_BFIO_ADAPTER_H

#include <stdint.h>

#include <libfsntfs.h>
#include <libvshadow.h>

#include "ntfs_bfio_adapter.h"
#include "../core/bad_sector_log.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 构造一个包装了已经打开的 libvshadow store 的自定义 libbfio 句柄。
 *
 * store：调用方已经用 libvshadow_volume_open+libvshadow_volume_get_store
 * 拿到手的 store 对象，这个函数不获取它的所有权——调用方在这个适配器
 * 用完、调用 libbfio_handle_free 之后，仍然要自己负责调用
 * libvshadow_store_free/libvshadow_volume_close/libvshadow_volume_free。
 *
 * max_retries/out_bad_sector_log：和 ntfs_bfio_adapter_initialize 同样的
 * 约定——out_bad_sector_log 非 NULL 时才会在重试耗尽后走"0 填充+记日志+
 * 返回成功"这条路径。
 *
 * 返回 1 成功（*out_handle 可以直接传给
 * libfsntfs_volume_open_file_io_handle），-1 失败。
 */
int vshadow_bfio_adapter_initialize(
     libvshadow_store_t *store,
     uint32_t max_retries,
     bad_sector_log_t *out_bad_sector_log,
     libbfio_handle_t **out_handle,
     libfsntfs_error_t **error );

#ifdef __cplusplus
}
#endif

#endif /* ENGINE_SHELL_VSHADOW_BFIO_ADAPTER_H */
