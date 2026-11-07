/*
 * I2（BitLocker自动识别，2026-11-06小树裁决）：把一个盘符映射到它在
 * 物理盘上的编号+字节偏移——BitLocker卷必须按"物理盘路径+偏移"这种
 * 形式访问（见scan_source.h头注释里B9的既有限制：锁定的BitLocker卷
 * 不能走卷盘符设备对象读取），这个模块就是用来自动算出这两个值，
 * 不需要用户自己在BitLocker向导里手填。
 *
 * imperative shell：Windows专属。真机验证过（I0，见docs/开发日志.md
 * 对应记录）：这个查询在卷被BitLocker锁定、完全读不出文件系统内容的
 * 情况下依然能正常调用成功，返回和解锁状态完全相同的结果——卷管理器
 * 层面的元数据，不需要先解锁、CreateFileA的dwDesiredAccess传0就够用。
 */
#ifndef ENGINE_SHELL_VOLUME_DISK_EXTENTS_H
#define ENGINE_SHELL_VOLUME_DISK_EXTENTS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
	/* 1 = 这个卷只跨一个物理盘的一段连续区间（绝大多数场景，简单分区/
	 *     VHD/U盘都是这样）；0 = 跨多个物理盘区间（比如条带化的动态
	 *     磁盘卷，罕见）——下面的disk_number/starting_offset只在
	 *     is_single_extent为1时有意义，为0时调用方应该回退到手动输入，
	 *     不要假装能自动算出一个对的值 */
	int is_single_extent;

	/* \\.\PhysicalDriveN里的N */
	uint32_t disk_number;

	/* 这个卷在物理盘上的起始字节偏移——和ScanSource::baseOffset、
	 * BitLockerWizardDialog里"分区偏移"字段是同一个概念 */
	uint64_t starting_offset;

	uint64_t extent_length;
}
volume_disk_extents_t;

/* 查询一个盘符对应的物理盘位置信息。
 * 返回1成功（哪怕is_single_extent是0，只要查询本身成功就算1，调用方
 * 自己看is_single_extent决定能不能用）；返回-1表示查询失败（盘符不
 * 存在、打不开卷句柄、IOCTL调用失败等）。
 */
int volume_disk_extents_query(
     char drive_letter,
     volume_disk_extents_t *out );

#ifdef __cplusplus
}
#endif

#endif /* ENGINE_SHELL_VOLUME_DISK_EXTENTS_H */
