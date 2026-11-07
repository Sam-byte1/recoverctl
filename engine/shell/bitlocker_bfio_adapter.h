/*
 * imperative shell（B7，2026-11-05 任务拆解排期）：给 libfsntfs 提供一个
 * 自定义的 libbfio I/O 句柄，让它读到的不是物理设备的原始（加密）字节，
 * 而是 libbde 解锁之后重建出来的明文卷内容。
 *
 * 架构完全照抄 `vshadow_bfio_adapter.c`——这正是第26步调研时就已经
 * 确认过的结论："解锁成功之后"这一层（把`libbde_volume_t`包成bfio
 * handle喂给libfsntfs）和libvshadow的`vshadow_bfio_adapter`是同一套
 * 形状：libbde 同样提供 `libbde_volume_read_buffer_at_offset`/
 * `seek_offset`/`get_size` 这一组完整的、类似"打开一个文件就能读"的
 * 接口（已核对头文件确认，不是凭印象），核心的"膨胀对齐+重试+坏道
 * 容忍"算术（`ntfs_bfio_read_core`）原样复用、一行不改。
 *
 * 调用方（cli/main.c）负责先用 B6 的 `bitlocker_unlock_volume` 拿到一个
 * 已经解锁成功的 `libbde_volume_t*`，这个模块不获取它的所有权——用完
 * 调用 `libbfio_handle_free` 之后，调用方仍然要自己负责
 * `libbde_volume_close`/`libbde_volume_free`。
 */
#ifndef ENGINE_SHELL_BITLOCKER_BFIO_ADAPTER_H
#define ENGINE_SHELL_BITLOCKER_BFIO_ADAPTER_H

#include <stdint.h>

#include <libfsntfs.h>
#include <libbde.h>

#include "ntfs_bfio_adapter.h"
#include "../core/bad_sector_log.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 构造一个包装了已经解锁的 libbde volume 的自定义 libbfio 句柄。
 *
 * volume：调用方已经用 B6 `bitlocker_unlock_volume` 解锁成功拿到手的
 * volume 对象，这个函数不获取它的所有权。
 *
 * max_retries/out_bad_sector_log：和 ntfs_bfio_adapter_initialize /
 * vshadow_bfio_adapter_initialize 同样的约定。
 *
 * 返回 1 成功（*out_handle 可以直接传给
 * libfsntfs_volume_open_file_io_handle），-1 失败。
 */
int bitlocker_bfio_adapter_initialize(
     libbde_volume_t *volume,
     uint32_t max_retries,
     bad_sector_log_t *out_bad_sector_log,
     libbfio_handle_t **out_handle,
     libfsntfs_error_t **error );

#ifdef __cplusplus
}
#endif

#endif /* ENGINE_SHELL_BITLOCKER_BFIO_ADAPTER_H */
