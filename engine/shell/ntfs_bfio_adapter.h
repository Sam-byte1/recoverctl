/*
 * imperative shell：给 libfsntfs 提供一个自定义的 libbfio I/O 句柄，让 NTFS
 * 卷的真正读取走我们自己的代码（能接入坏道容忍），而不是 libfsntfs 内置的
 * `libbfio_file` 实现（2026-11-05 小树裁决第20步，P5 桶③执行）。
 *
 * 背景：`libfsntfs_volume_open(path)` 这个最简单的打开方式，内部是用
 * libfsntfs 自带的 libbfio_file 实现去真正读磁盘，我们完全插不进任何
 * 坏道容忍逻辑。第19步调研确认 libfsntfs **公开支持**换一种打开方式——
 * `libfsntfs_volume_open_file_io_handle(volume, file_io_handle, ...)`，
 * 接受调用方自己构造的 `libbfio_handle_t`，而构造这个句柄需要的
 * `libbfio_handle_initialize` 本身就是一套标准的依赖注入接口（给一组
 * open/close/read/write/seek 等函数指针）——这正是本项目从第11步起一直
 * 在用的同一个模式，不是新发明的技术路线。
 *
 * 一个值得记录的实现细节：libfsntfs.h 里 `libfsntfs_volume_open_file_io_handle`
 * 的公开声明被 `LIBFSNTFS_HAVE_BFIO` 宏保护，这个宏的设计意图是"只有系统上
 * 装了独立的 libbfio 开发包才暴露这组接口"——但我们只 vendor 了 libfsntfs
 * 自带的内部私有 libbfio 拷贝（没有独立的 `libbfio.h`/`libbfio/extern.h`
 * 这类公开头文件），常规方式打开这个宏反而会因为缺文件编译失败。这里改成
 * 在 .c 文件里手写这几个函数/类型的声明，不依赖 libfsntfs.h 的条件编译，也
 * 不 include 任何 vendored 的 libbfio 内部头文件——类型用 libyal 全系列统一
 * 的"公开类型其实都是 `intptr_t` 不透明句柄"约定（`libfsntfs_volume_t`/
 * `libfsntfs_error_t` 已经是这样），已经用 `nm` 核实这几个符号真实存在于
 * 当前链接的 libfsntfs 动态库里，也用一个独立的最小化探针程序实际链接+
 * 调用验证过可以正常工作，不是纸面推测。
 *
 * 范围刻意不包含：NTFS 分区拷贝路径（`extract_partition_to_temp_file`，
 * 这条路径第18步已经接入坏道容忍，拷出来的临时文件本身是普通文件，
 * libfsntfs 用默认的 `libfsntfs_volume_open` 打开临时文件完全没问题，
 * 不需要这个自定义句柄）；也不包含"探测这是不是 NTFS"的 probe 调用
 * （那里不做真实数据恢复 I/O，不需要坏道容忍）。
 *
 * base_offset（B9真机验证+2026-11-05小树裁决新增）：BitLocker锁定卷
 * 不能通过卷盘符设备对象（`\\.\G:`）直接读取——Windows `fvevol.sys`
 * 过滤驱动会在卷管理器层面直接拒绝（`FVE_E_LOCKED_VOLUME`），真机验证
 * 确认过，必须改走"物理盘路径+分区字节偏移"（`\\.\PhysicalDriveN`+
 * `Get-Partition`查到的偏移）。这个适配器因此加一个`base_offset`参数：
 * 调用方眼里的"逻辑偏移0"对应真实设备上的"物理偏移base_offset"，所有
 * seek/read/get_size 都在 物理偏移=base_offset+逻辑偏移 上操作，对
 * libfsntfs/libbde 来说卷依然感觉"从0开始"。这和 fat_reader_open_at_offset
 * 已经验证半年的"偏移感知读取"是同一个思路，不是新发明；没有用
 * "拷贝整个分区到临时文件"那条路（NTFS扫描历史上走过这条路，是当年
 * libfsntfs还不支持自定义libbfio句柄时的历史遗留，不是范式）——BitLocker
 * 加密的是整盘，常见到几百GB~几TB，拷贝需要等量空闲磁盘空间且极慢，
 * 不现实。base_offset=0时行为和之前完全一样（NTFS直接scan/recover两处
 * 调用点就是这样用的，零行为变化）。
 */
#ifndef ENGINE_SHELL_NTFS_BFIO_ADAPTER_H
#define ENGINE_SHELL_NTFS_BFIO_ADAPTER_H

#include <stdint.h>

#include <libfsntfs.h>

#include "aligned_device_reader.h"
#include "../core/bad_sector_log.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 和 libfsntfs_volume_t/libfsntfs_error_t 同样的"不透明 intptr_t 句柄"
 * 约定，见上方文件头注释 */
typedef intptr_t libbfio_handle_t;

/* `ntfs_bfio_read_core` 单次调用能处理的请求上限（含lead_in）——
 * 2026-11-07 新增：原来只在 ntfs_bfio_adapter.c 内部用的私有常量，现在
 * 提升为公开常量，因为所有调用 `ntfs_bfio_read_core` 的"命令式外壳"
 * （`ntfs_bfio_read`/`vshadow_bfio_read`）都必须知道这个上限，自己把
 * 超限的大请求拆成不超过这个大小的若干块分别调用——不这样做、让超限
 * 请求绕过这套"膨胀对齐+重试+坏道容忍"机制直接摸真实设备，是真机测试
 * 实测过的静默数据损坏缺口（见 ntfs_bfio_adapter.c 里 `ntfs_bfio_read`
 * 的实现注释）。 */
#define NTFS_BFIO_ALIGNED_BUFFER_SIZE 65536

/* 手写声明（理由见上方文件头注释），调用方（cli/main.c）需要这两个符号
 * 才能分别"把构造好的句柄交给 libfsntfs"和"用完释放句柄" */
extern int libfsntfs_volume_open_file_io_handle(
     libfsntfs_volume_t *volume,
     libbfio_handle_t *file_io_handle,
     int access_flags,
     libfsntfs_error_t **error );

extern int libbfio_handle_free(
     libbfio_handle_t **handle,
     libfsntfs_error_t **error );

/* 构造一个包装了 volume_path 的自定义 libbfio 句柄。
 *
 * max_retries/out_bad_sector_log：和项目里其它坏道容忍接口同样的约定——
 * out_bad_sector_log 非 NULL 时才会在重试耗尽后走"0 填充+记日志+返回
 * 成功"这条路径，为 NULL 时读取失败就是真失败（原样把错误交还给
 * libfsntfs，不会有任何行为变化）。max_retries 在 out_bad_sector_log 为
 * NULL 时不起作用。
 *
 * 返回 1 成功（*out_handle 可以直接传给
 * libfsntfs_volume_open_file_io_handle），-1 失败。调用方用完之后要调用
 * libbfio_handle_free 释放（会连带调用内部的 free_io_handle 回调，释放
 * 这个函数自己分配的上下文结构体和底层设备句柄，调用方不需要再额外
 * 调用任何清理函数）。
 */
int ntfs_bfio_adapter_initialize(
     const char *volume_path,
     uint64_t base_offset,
     uint32_t max_retries,
     bad_sector_log_t *out_bad_sector_log,
     libbfio_handle_t **out_handle,
     libfsntfs_error_t **error );

/* 纯函数：根据libbfio的seek约定（whence=SEEK_SET/SEEK_CUR/SEEK_END，
 * offset可正可负）+ 当前逻辑偏移 + 逻辑卷大小，算出新的逻辑偏移。
 * 刻意不知道base_offset这件事——"逻辑"这个词本身就是"调用方看到的、
 * 从卷头0开始计数的坐标系"。
 *
 * B9真机验证第二轮（2026-11-05小树裁决）之后，这个函数的结果不再被
 * 拿去调用真实的`SetFilePointerEx`/`fseek`——真实的`ntfs_bfio_seek_offset`
 * 回调现在只用这个结果更新`context->current_offset`这个纯粹的
 * bookkeeping状态，不碰任何真实设备。真正的物理定位挪到了
 * `ntfs_bfio_read_adapter`里：每次真正读取之前，现场用
 * `base_offset+current_offset`算出物理目标、现场对齐、现场seek，不
 * 依赖"上一次seek_offset调用在真实设备上留下的游标位置"这个假设——
 * 原因是裸物理盘设备对象（`\\.\PhysicalDriveN`）连seek的*起点*本身都
 * 必须扇区对齐，这个对齐工作只有紧邻真正读取之前现算才可靠，参见
 * `engine/core/sector_aligned_read.h`里
 * `sector_aligned_read_compute_aligned_seek`的说明。
 *
 * logical_size：SEEK_END要用的逻辑卷大小（不是设备物理大小——真实
 * seek_offset回调查到物理大小后要先减掉base_offset再传进来），传-1
 * 表示未知（查询失败），这种情况下任何SEEK_END请求直接判定失败，不
 * 猜一个值。
 *
 * 返回1成功（*out_new_logical_offset是新的逻辑偏移），-1失败
 * （whence不合法、SEEK_END但logical_size<0、或者算出的新偏移是负数——
 * 卷内不存在负偏移）。
 */
int ntfs_bfio_seek_offset_core(
     int64_t current_logical_offset,
     int64_t offset,
     int whence,
     int64_t logical_size,
     int64_t *out_new_logical_offset );

/* 读取回调的核心逻辑，接受注入的 read_fn，单元测试用（2026-11-05 小树
 * 裁决第20步 N3/N7）：和 aligned_device_reader.c 同样的依赖注入模式，
 * 让"膨胀对齐+重试+裁剪+坏道容忍"这段算术能用内存 mock 完整 TDD 覆盖。
 * 真实的 libbfio read 回调（ntfs_bfio_adapter.c 内部的 static 函数）只是
 * 用真实设备读取函数调用这个函数，自己不含值得单独测试的逻辑。
 *
 * lead_in（B9真机验证第二轮，2026-11-05小树裁决新增）：调用方（真实
 * `ntfs_bfio_read`）已经用`sector_aligned_read_compute_aligned_seek`
 * 算出"这次要读的物理起点对齐到扇区边界之后，前面垫了几个字节的
 * 冗余"，传进来之后这个函数会：①膨胀请求长度时把lead_in也算进去
 * （`lead_in+size`再向上取整到扇区边界）；②从读到的数据里先跳过
 * lead_in个字节，再裁出调用方真正要的`size`字节。lead_in==0时行为和
 * 加这个参数之前完全一样（这是最重要的回归保障——vshadow走的是类
 * 文件API、COW重映射在库内部完成，永远传0）。
 *
 * offset_for_log：调用方传入当前卷内偏移，仅用于坏道日志记录，这个
 * 函数本身不关心它的含义。
 *
 * 返回实际可用字节数（可能是真实数据，也可能是坏道容忍场景下的 0
 * 填充），-1 表示彻底失败（含`lead_in+size`超过内部对齐缓冲区上限且
 * lead_in>0这种明确拒绝的场景，见.c文件里的实现注释）。 */
ssize_t ntfs_bfio_read_core(
     aligned_device_read_fn read_fn,
     void *read_context,
     uint8_t *buffer,
     size_t size,
     size_t lead_in,
     uint32_t max_retries,
     uint64_t offset_for_log,
     bad_sector_log_t *out_bad_sector_log );

#ifdef __cplusplus
}
#endif

#endif /* ENGINE_SHELL_NTFS_BFIO_ADAPTER_H */
