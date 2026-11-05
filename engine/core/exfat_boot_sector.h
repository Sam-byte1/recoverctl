/*
 * functional core：解析 exFAT 主引导扇区（Main Boot Sector）。
 * 纯函数：输入是一段已经读到内存里的字节（512 字节的扇区内容），
 * 不做任何磁盘 I/O，不依赖任何第三方库——和 FAT32 那一轮一样，libfsfat
 * 不提供"判断已删除记录"的能力，核心逻辑完全自研（详见 docs/开发日志.md
 * 2026-11-04 关于 libfsfat 能力核实的记录，exFAT 和 FAT32 是同一个结论）。
 *
 * 范围说明（这一轮刻意收窄，和 FAT32 那一轮同样的原则）：只解析我们自己
 * 的扫描/恢复逻辑需要的字段，不解析/校验 OEM 参数、Boot Checksum、
 * 位图或大写表的具体内容——这些是给操作系统做一致性检查用的，不影响
 * "读目录项、按簇号找数据"这条恢复路径。
 */
#ifndef ENGINE_CORE_EXFAT_BOOT_SECTOR_H
#define ENGINE_CORE_EXFAT_BOOT_SECTOR_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
	uint64_t volume_length_sectors;          /* VolumeLength */
	uint32_t fat_offset_sectors;              /* FatOffset */
	uint32_t fat_length_sectors;              /* FatLength（每一份 FAT 占用的扇区数） */
	uint32_t cluster_heap_offset_sectors;     /* ClusterHeapOffset：数据区（簇 2 开始）起始扇区号，
	                                            * exFAT 直接存了这个值，不像 FAT32 需要用
	                                            * reserved+numfats*fatsize 推算 */
	uint32_t cluster_count;                   /* ClusterCount */
	uint32_t first_cluster_of_root_directory; /* FirstClusterOfRootDirectory */
	uint32_t bytes_per_sector;                /* 由 BytesPerSectorShift 换算：1 << shift */
	uint32_t sectors_per_cluster;             /* 由 SectorsPerClusterShift 换算：1 << shift */
	uint8_t number_of_fats;
	int is_valid;                             /* 1 = 解析出来的字段像是合法的 exFAT 引导扇区 */
}
exfat_boot_sector_t;

/* 解析一段 512 字节的主引导扇区数据。sector_data_size 必须 >= 512。
 * 返回 1 成功（out->is_valid 还要另外看），-1 表示输入参数不合法（比如太短）。
 */
int exfat_boot_sector_parse(
     const uint8_t *sector_data,
     size_t sector_data_size,
     exfat_boot_sector_t *out );

/* 把簇号换算成这个卷里的绝对扇区号（簇号从 2 开始，0、1 是保留号） */
uint32_t exfat_boot_sector_cluster_to_sector(
          const exfat_boot_sector_t *bs,
          uint32_t cluster );

#ifdef __cplusplus
}
#endif

#endif /* ENGINE_CORE_EXFAT_BOOT_SECTOR_H */
