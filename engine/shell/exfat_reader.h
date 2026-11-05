/*
 * imperative shell：真正读磁盘/镜像文件，把字节喂给 engine/core 里的
 * exFAT 纯函数。和 fat_reader.{h,c} 的定位完全一致（不走 TDD，靠端到端
 * 脚本 + 哈希校验验证）。
 *
 * 范围限制（和这一轮的整体范围保持一致，必须在文档里写清楚，不能是隐藏假设）：
 * - 只支持文件名 <= 15 个 UTF-16 字符、正确的 UTF-16LE→UTF-8 转码
 * - 恢复文件内容：2026-11-05 小树裁决第16步之前是"假设 NoFatChain（数据
 *   连续存储），不跟踪 FAT 链"；第16步起消费 candidate 的 no_fat_chain
 *   标志位区分处理——true 走原有的连续读取路径，false 跟着
 *   engine/core/exfat_fat_chain.c 走出来的簇链逐簇读取。这个区分之所以
 *   可靠，是因为第15步已经在真实 exFAT 设备上验证过：删除文件不会清空
 *   FAT 表项，碎片文件的簇链信息在删除后依然完整保留
 */
#ifndef ENGINE_SHELL_EXFAT_READER_H
#define ENGINE_SHELL_EXFAT_READER_H

#include <stdint.h>
#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include "../core/bad_sector_log.h"
#include "../core/exfat_boot_sector.h"
#include "../core/exfat_undelete_scanner.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
	/* 真实的 \\.\PhysicalDriveN / \\.\<盘符>: 是裸设备，CRT 的 fopen 在这种
	 * 路径上实测会打不开或读不到东西（FAT32 那一轮在真实磁盘上第一次测试
	 * 就撞上了这个，详见 docs/开发日志.md）。exFAT 这边直接应用那次的教训，
	 * 从一开始就用 CreateFileA+ReadFile，不用再踩一次同样的坑。 */
#ifdef _WIN32
	HANDLE file;
#else
	FILE *file;
#endif
	exfat_boot_sector_t boot_sector;
	long base_offset;   /* 这个 exFAT 卷在底层设备/镜像里的起始字节偏移，
	                      * 非 0 代表"这是磁盘里的某个分区，不是独立的卷文件" */
}
exfat_reader_t;

/* 打开一个 exFAT 镜像/卷，解析主引导扇区。等价于 base_offset=0 的
 * exfat_reader_open_at_offset。
 * 返回 1 成功，-1 失败（打开失败，或者解析出来发现不是合法 exFAT）。
 */
int exfat_reader_open(
     const char *path,
     exfat_reader_t *out );

int exfat_reader_open_at_offset(
     const char *path,
     long base_offset,
     exfat_reader_t *out );

void exfat_reader_close(
      exfat_reader_t *reader );

/* 扫描根目录（假设只占 1 个簇），收集已删除候选项 */
int exfat_reader_scan_root_directory(
     exfat_reader_t *reader,
     exfat_undelete_scan_result_t *out );

/* 扫描根目录 + 任意深度子目录，收集到的候选项按来源目录分别标记了
 * directory_path 字段。某一层子目录读取失败不会拖垮整体扫描。
 *
 * P5 坏道保护（2026-11-05 小树裁决第18步）：out_bad_sector_log 非 NULL
 * 时，目录簇读取失败会先重试，重试耗尽后把这个（子）目录当成空目录
 * 处理（和 fat_reader_scan_directory_tree 同样的设计，见那边的注释），
 * 并记一笔坏道日志；传 NULL 表示不需要这个能力，行为和原来完全一样。 */
int exfat_reader_scan_directory_tree(
     exfat_reader_t *reader,
     bad_sector_log_t *out_bad_sector_log,
     exfat_undelete_scan_result_t *out );

/* 从 first_cluster 开始恢复 file_size 字节的内容，写到 output_path。
 * no_fat_chain 为真（非 0）：数据连续存储，直接顺序读取（原有路径）；
 * no_fat_chain 为假（0）：数据碎片化存储，跟着 FAT 表的簇链逐簇读取
 * （2026-11-05 小树裁决第16步新增）。链读取失败（FAT 表项被破坏、簇号
 * 超出范围、走到的簇数和 file_size 声称的不一致）会返回 -1，不会写出
 * 部分/错误的内容。
 *
 * P5 坏道保护（2026-11-05 小树裁决第18步）：out_bad_sector_log 非 NULL
 * 时，两条分支（连续存储/碎片化）都会在单块读取失败时先重试，重试耗尽
 * 后用 0 填充该区域、记一笔坏道日志、继续恢复剩余内容，而不是直接判定
 * 整个文件恢复失败；传 NULL 表示不需要这个能力，行为和原来完全一样。 */
int exfat_reader_recover_file(
     exfat_reader_t *reader,
     uint32_t first_cluster,
     uint64_t file_size,
     int no_fat_chain,
     bad_sector_log_t *out_bad_sector_log,
     const char *output_path );

#ifdef __cplusplus
}
#endif

#endif /* ENGINE_SHELL_EXFAT_READER_H */
