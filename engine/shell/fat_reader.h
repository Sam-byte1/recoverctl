/*
 * imperative shell：真正读磁盘/镜像文件，把字节喂给 engine/core 里的
 * FAT32 纯函数。不走 TDD，靠端到端脚本 + 哈希校验验证（和 file_writer.c
 * 对 NTFS 的定位完全一致）。
 *
 * 范围限制（和这一轮的整体范围保持一致，必须在文档里写清楚，不能是隐藏假设）：
 * - 只支持 FAT32
 * - 只扫根目录，且假设根目录只占 1 个簇（真实大容量 FAT32 卷如果根目录文件
 *   很多、超过 1 个簇，这个版本会读不全，需要后续增量支持"跟着 FAT 链继续读
 *   下一个簇"）
 * - 恢复文件内容时假设数据是连续存储的（从起始簇开始、按文件大小顺序读取），
 *   不跟踪 FAT 表里的簇链——这是所有 FAT 数据恢复工具公认的局限，不是我们
 *   独有的缺陷：文件被删除时 FAT 表项已经被清空，后续簇号信息永久丢失，
 *   只能靠"大概率是连续存储"这个假设去赌
 */
#ifndef ENGINE_SHELL_FAT_READER_H
#define ENGINE_SHELL_FAT_READER_H

#include <stdint.h>
#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include "../core/bad_sector_log.h"
#include "../core/fat_boot_sector.h"
#include "../core/fat_undelete_scanner.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
	/* Windows 下真实的 \\.\PhysicalDriveN / \\.\<盘符>: 是裸设备，不是普通
	 * 文件——CRT 的 fopen/fseek/fread 这套在这种路径上实测会打不开或者读
	 * 不到东西（NTFS 分区偏移扫描那边第一次在真实磁盘上测试就撞上过同一个
	 * 坑，参见 cli/main.c 的 extract_partition_to_temp_file）。这里用和那边
	 * 一样的 CreateFileA+ReadFile，Mac 开发机只对着自己手造的镜像文件测试，
	 * 继续用 fopen 就够了。 */
#ifdef _WIN32
	HANDLE file;
#else
	FILE *file;
#endif
	fat_boot_sector_t boot_sector;
	long base_offset;   /* 这个 FAT32 卷在底层设备/镜像里的起始字节偏移，
	                      * 非 0 代表"这是磁盘里的某个分区，不是独立的卷文件"——
	                      * 这就是分区表解析（第 3 步）和文件系统扫描串起来的接口 */
}
fat_reader_t;

/* 打开一个 FAT32 镜像/卷，解析引导扇区。等价于 base_offset=0 的
 * fat_reader_open_at_offset。
 * 返回 1 成功，-1 失败（打开失败，或者解析出来发现不是合法 FAT32）。
 */
int fat_reader_open(
     const char *path,
     fat_reader_t *out );

/* 和 fat_reader_open 一样，但允许指定这个卷在底层文件/设备里的起始字节偏移
 * （比如：从 recoverctl partitions 解析出来的某个分区偏移）。这样就不需要
 * 先把分区内容单独切出来一个文件，可以直接对着整块磁盘镜像/设备、在
 * 正确的偏移处开始当 FAT32 卷来扫描。
 */
int fat_reader_open_at_offset(
     const char *path,
     long base_offset,
     fat_reader_t *out );

void fat_reader_close(
      fat_reader_t *reader );

/* 扫描根目录（假设只占 1 个簇），收集已删除候选项 */
int fat_reader_scan_root_directory(
     fat_reader_t *reader,
     fat_undelete_scan_result_t *out );

/* 扫描根目录 + 任意深度子目录，收集到的候选项按来源目录分别标记了
 * directory_path 字段。某一层子目录读取失败不会拖垮整体扫描，会跳过
 * 那一层继续处理其它结果。
 *
 * P5 坏道保护（2026-11-05 小树裁决第18步）：out_bad_sector_log 非 NULL
 * 时，目录簇读取失败会先重试，重试耗尽后把这个（子）目录当成空目录
 * 处理（全 0 字节按 FAT 目录项格式解析出来就是"目录结束"），并记一笔
 * 坏道日志——效果上和原来"跳过这一层"完全一致，只是现在有了可见的
 * 坏道记录；传 NULL 表示不需要这个能力，行为和原来完全一样。 */
int fat_reader_scan_directory_tree(
     fat_reader_t *reader,
     bad_sector_log_t *out_bad_sector_log,
     fat_undelete_scan_result_t *out );

/* 假设数据连续存储，从 first_cluster 开始读 file_size 字节，写到 output_path。
 *
 * P5 坏道保护（2026-11-05 小树裁决第17步，第一片落地在 FAT32 这条路径）：
 * out_bad_sector_log 非 NULL 时，读取某一块失败会先重试几次（见
 * `engine/shell/aligned_device_reader.c` 里的固定重试次数），重试耗尽后
 * 跳过这一块（写 0 填充）、记一笔坏道日志、继续恢复剩余内容，而不是让
 * 整个恢复直接失败；传 NULL 表示不需要这个能力，行为和原来完全一样。*/
int fat_reader_recover_file(
     fat_reader_t *reader,
     uint32_t first_cluster,
     uint32_t file_size,
     bad_sector_log_t *out_bad_sector_log,
     const char *output_path );

#ifdef __cplusplus
}
#endif

#endif /* ENGINE_SHELL_FAT_READER_H */
