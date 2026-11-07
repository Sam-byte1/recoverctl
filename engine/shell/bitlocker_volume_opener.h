/*
 * imperative shell：BitLocker（B1，2026-11-05 任务拆解排期）诊断能力——
 * 打开一个设备/卷，不碰任何解锁凭据，只读出它上面挂了哪些"密钥保护器"
 * （key protector，BitLocker 卷元数据里记录的、可以用来解开这个卷的
 * 方式——密码/恢复密钥/启动密钥/TPM 等）。这一步纯粹是"这块盘能不能用
 * 我们支持的方式解锁"的探测，libbde 的 `libbde_volume_open` 本身就不
 * 需要任何凭据（元数据是明文存储的，只有卷本身的数据才加密），所以
 * B1 完全不涉及敏感信息处理。
 *
 * 这个模块本身也是 B6（真正解锁）要复用的"打开BitLocker卷"基础设施，
 * 不是只服务于诊断命令的一次性代码。
 */
#ifndef ENGINE_SHELL_BITLOCKER_VOLUME_OPENER_H
#define ENGINE_SHELL_BITLOCKER_VOLUME_OPENER_H

#include <stddef.h>
#include <stdint.h>

#include <libbde.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
	/* 对应 LIBBDE_KEY_PROTECTION_TYPE_* 枚举值（definitions.h）：
	 * CLEAR_KEY=0x0000, TPM=0x0100, STARTUP_KEY=0x0200,
	 * TPM_AND_PIN=0x0500, RECOVERY_PASSWORD=0x0800, PASSWORD=0x2000 */
	uint16_t type;

	/* 128位GUID，原样透传给调用方展示，不做任何解读 */
	uint8_t guid[ 16 ];
}
bitlocker_key_protector_info_t;

typedef struct
{
	bitlocker_key_protector_info_t *entries;
	int count;
	int capacity;
}
bitlocker_key_protector_list_t;

/* 打开 device_path 指向的卷，枚举它上面的全部密钥保护器，结果写进
 * out（调用方必须先清零初始化，函数内部按需扩容）。不碰任何凭据。
 *
 * base_offset（B9真机验证+2026-11-05小树裁决新增）：锁定的BitLocker卷
 * 不能通过卷盘符设备对象（`\\.\G:`）直接读取（Windows fvevol.sys过滤
 * 驱动会拒绝，真机验证确认过），要改传物理盘路径（`\\.\PhysicalDriveN`）
 * +这个分区在物理盘上的字节偏移——和 engine/shell/ntfs_bfio_adapter.h
 * 的 base_offset 是同一个参数、同一套语义，直接透传给
 * `ntfs_bfio_adapter_initialize()`。传0等价于"直接传卷路径"这个旧
 * 用法（未锁定/未加密的卷可以继续这样用，行为不变）。
 *
 * 返回 1 成功（即使 count==0 也算成功——"这个设备不是BitLocker卷/
 * 没有保护器"是一个合法结果，不是错误），-1 失败（打开失败、不是
 * BitLocker卷、内存分配失败等，具体原因看 error）。
 */
int bitlocker_volume_opener_list_key_protectors(
     const char *device_path,
     uint64_t base_offset,
     bitlocker_key_protector_list_t *out,
     libbde_error_t **error );

void bitlocker_key_protector_list_free(
      bitlocker_key_protector_list_t *list );

#ifdef __cplusplus
}
#endif

#endif /* ENGINE_SHELL_BITLOCKER_VOLUME_OPENER_H */
