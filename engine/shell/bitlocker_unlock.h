/*
 * imperative shell（B6，2026-11-05 任务拆解排期）：拿到一份已解析的
 * 凭据（来自B5文件渠道，后续B10/B11渠道也会产出同样的类型），真正去
 * 调用libbde尝试解锁一个BitLocker卷。
 *
 * 调用顺序是严格的（照抄libbde官方 `bdetools/mount_handle.c` 的真实
 * 用法，不是凭头文件注释猜的）：`libbde_volume_initialize` →
 * 按凭据类型调用对应的 `libbde_volume_set_utf8_password` /
 * `set_utf8_recovery_password` / `read_startup_key` / `set_keys`
 * （这几个函数必须在 open 之前调用，libbde头文件原文写明"needs to be
 * used before one of the open functions"）→ 打开卷 →
 * `libbde_volume_is_locked` 核实是不是真的解开了（返回1表示"仍然锁着"，
 * 这种情况下`open`本身不会报错——它是一次"打开元数据结构"的操作，
 * 凭据到底对不对要靠`is_locked`另外确认，不能只看`open`的返回值）。
 *
 * B9真机验证发现：不能用`libbde_volume_open(path)`这个简单方式打开
 * （内部走通用`libcfile`文件抽象层，对真实Windows裸卷设备路径——
 * 尤其BitLocker锁定状态下的卷——查询文件大小会失败，用libbde官方
 * `bdeinfo`工具对真实锁定卷复现过同样的失败，不是我们代码的bug）。
 * 改用`ntfs_bfio_adapter_initialize`构造自定义libbfio句柄+
 * `libbde_volume_open_file_io_handle`，和`bitlocker_volume_opener.c`
 * （B1）同样的解法，见该文件头注释完整说明。
 *
 * 这个bfio_handle必须和*out_volume活得一样久（B7的
 * `bitlocker_bfio_adapter.c`后续读取解密数据时，底层仍然要靠这个
 * handle去读原始加密字节），所以也通过*out_bfio_handle返回给调用方，
 * 调用方要在`libbde_volume_close`之后、和*out_volume同一个生命周期里
 * 用`libbfio_handle_free`释放它。
 *
 * base_offset（B9真机验证+2026-11-05小树裁决新增）：和
 * `bitlocker_volume_opener.h`的同名参数同一套语义，直接透传给
 * `ntfs_bfio_adapter_initialize()`——锁定的BitLocker卷不能通过卷盘符
 * 设备对象读取，要传物理盘路径+分区在物理盘上的字节偏移。传0等价于
 * 旧的"直接传卷路径"用法。
 *
 * 敏感数据清零（世杰拍板"现在就要主动清零"）：这个函数拿到的
 * `credential` 参数不是const——调用成功或失败，所有路径最终都会把
 * `credential`里的密码/恢复密钥文本、FVEK、TWEAK字段清零（启动密钥
 * 的文件路径字符串不是密钥内容本身，不需要清零，见B5的说明）。调用方
 * 用完这个函数之后不需要、也不应该再自己对credential做一次清零。
 */
#ifndef ENGINE_SHELL_BITLOCKER_UNLOCK_H
#define ENGINE_SHELL_BITLOCKER_UNLOCK_H

#include <libbde.h>

#include "bitlocker_credential_source.h"
#include "ntfs_bfio_adapter.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 返回 1 成功（*out_volume 是一个已经初始化+打开+解锁成功的
 * libbde_volume_t，*out_bfio_handle 是打开它用的自定义bfio句柄，
 * 调用方拥有两者，用完要先 libbde_volume_close + libbde_volume_free，
 * 再 libbfio_handle_free），-1 失败（*out_volume 和 *out_bfio_handle
 * 保证为NULL；如果 *error 为NULL，说明是"凭据格式校验未通过/解锁后
 * 仍是锁住状态"这种非系统性失败——也就是凭据不对或者凭据类型选错了；
 * 如果*error非NULL，说明是真实的系统/IO错误，调用方可以打印它）。
 */
int bitlocker_unlock_volume(
     const char *device_path,
     uint64_t base_offset,
     bitlocker_resolved_credential_t *credential,
     libbde_volume_t **out_volume,
     libbfio_handle_t **out_bfio_handle,
     libbde_error_t **error );

#ifdef __cplusplus
}
#endif

#endif /* ENGINE_SHELL_BITLOCKER_UNLOCK_H */
