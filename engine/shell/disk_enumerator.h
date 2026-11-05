/*
 * imperative shell：枚举 Windows 上的逻辑磁盘（盘符）。
 * 这是 Windows 专属代码（用到 <windows.h>），只在 Windows 构建里编译，
 * Mac 侧的 build.sh 不引用这个文件。
 *
 * 目的：让产品不需要用户手动输入 `\\.\E:` 这种设备路径——真实产品里
 * 用户只会看到"你的 U 盘/硬盘"这种列表，设备路径是程序自己拼出来的。
 */
#ifndef ENGINE_SHELL_DISK_ENUMERATOR_H
#define ENGINE_SHELL_DISK_ENUMERATOR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
	char drive_letter;          /* 'C', 'D', ... */
	char drive_type_name[ 32 ]; /* "Fixed"、"Removable"、"Network"、"CD-ROM"、"RAM"、"Unknown" */
	char file_system[ 32 ];     /* "NTFS"、"FAT32" 等，取不到就是空字符串 */
	char volume_label[ 256 ];   /* 卷标，取不到就是空字符串 */
	uint64_t total_bytes;
	uint64_t free_bytes;
}
disk_entry_t;

typedef struct
{
	disk_entry_t *entries;
	int count;
	int capacity;
}
disk_enum_result_t;

typedef struct
{
	int disk_number;         /* 对应 \\.\PhysicalDriveN 里的 N */
	uint64_t size_bytes;
	int is_removable;        /* 1 = 可移动介质（U盘/SD卡等），0 = 固定硬盘，-1 = 取不到 */
	char model[ 256 ];       /* 设备型号，取不到就是空字符串 */
}
physical_disk_entry_t;

typedef struct
{
	physical_disk_entry_t *entries;
	int count;
	int capacity;
}
physical_disk_enum_result_t;

/* 枚举系统里所有的逻辑磁盘（盘符）。
 * 返回 1 成功，-1 失败（out 需要调用方先清零）。
 */
int disk_enumerator_list_logical_drives(
     disk_enum_result_t *out );

void disk_enum_result_free(
      disk_enum_result_t *result );

/* 枚举系统里所有的物理磁盘（\\.\PhysicalDrive0 .. N）。
 * 返回 1 成功，-1 失败（out 需要调用方先清零）。
 * 枚举到打不开的编号就停止——物理磁盘编号是连续分配的，不会有跳号。
 */
int disk_enumerator_list_physical_drives(
     physical_disk_enum_result_t *out );

void physical_disk_enum_result_free(
      physical_disk_enum_result_t *result );

#ifdef __cplusplus
}
#endif

#endif /* ENGINE_SHELL_DISK_ENUMERATOR_H */
