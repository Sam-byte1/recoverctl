/*
 * functional core：解析 FAT32 启动扇区（BPB，BIOS Parameter Block）。
 * 纯函数：输入是一段已经读到内存里的字节（512 字节的扇区内容），
 * 不做任何磁盘 I/O，不依赖任何第三方库——这是本项目第一次完全自研的
 * 文件系统解析代码（libfsfat 只覆盖"读取完好文件"这部分，不覆盖判定
 * 已删除记录，所以 FAT32 的已删除文件识别逻辑整条链路都是自己写的，
 * 详见 docs/开发日志.md 里 2026-11-04 关于 libfsfat 能力核实的记录）。
 *
 * 范围说明（这一轮刻意收窄）：只支持 FAT32（不支持 FAT12/FAT16/exFAT），
 * 字段按 Microsoft 官方 FAT 规范（fatgen103）解析。
 */
#ifndef ENGINE_CORE_FAT_BOOT_SECTOR_H
#define ENGINE_CORE_FAT_BOOT_SECTOR_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
	uint16_t bytes_per_sector;
	uint8_t sectors_per_cluster;
	uint16_t reserved_sector_count;
	uint8_t number_of_fats;
	uint32_t fat_size_sectors;       /* BPB_FATSz32，一份 FAT 表占用的扇区数 */
	uint32_t total_sectors;          /* BPB_TotSec32 */
	uint32_t root_cluster;           /* BPB_RootClus，根目录起始簇号 */
	int is_valid;                    /* 1 = 解析出来的字段像是合法的 FAT32 引导扇区 */
}
fat_boot_sector_t;

/* 解析一段 512 字节的引导扇区数据。sector_data_size 必须 >= 512。
 * 返回 1 成功（out->is_valid 还要另外看，1 不代表一定是合法 FAT32，
 * 只代表解析过程本身没有越界），-1 表示输入参数不合法（比如太短）。
 */
int fat_boot_sector_parse(
     const uint8_t *sector_data,
     size_t sector_data_size,
     fat_boot_sector_t *out );

/* 数据区（簇 2 开始的区域）起始扇区号 */
uint32_t fat_boot_sector_data_start_sector(
          const fat_boot_sector_t *bs );

/* 把簇号换算成这个卷里的绝对扇区号（簇号从 2 开始，0、1 是保留号） */
uint32_t fat_boot_sector_cluster_to_sector(
          const fat_boot_sector_t *bs,
          uint32_t cluster );

/* 第一份 FAT 表在卷里的起始扇区号 */
uint32_t fat_boot_sector_fat_start_sector(
          const fat_boot_sector_t *bs );

#ifdef __cplusplus
}
#endif

#endif /* ENGINE_CORE_FAT_BOOT_SECTOR_H */
