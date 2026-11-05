/*
 * functional core：给定 FAT32 FAT 表的原始字节和一个起始簇号，走出完整的
 * 簇链（哪个簇后面接哪个簇，直到遇到 EOF 标记）。
 *
 * 背景（2026-11-05 第21步，"目录单簇假设"修复）：原来 fat_reader.c 扫描
 * 目录（根目录或任意子目录）时只读第一个簇，目录内容超过一个簇能装下的
 * 条目数时，后面的已删除候选会被静默漏掉。这个模块只负责"给定 FAT 表
 * 字节，走出簇号序列"这个纯计算，不做任何磁盘 I/O（真实读取 FAT 表、
 * 按簇号读数据是 shell 层 fat_reader.c 的职责）——和 exFAT 版本
 * （exfat_fat_chain.h）结构完全一致，但 FAT32 没有 exFAT 那种
 * "连续存储可以不查表"的 NoFatChain 优化：FAT32 的簇永远要靠 FAT 表
 * 链式结构表示，所以这里不需要任何分支判断，调用方（fat_reader.c）对
 * 所有目录（包括根目录）统一走这一条路径。
 *
 * 第15步"FAT32碎片化文件恢复不可行"的结论（删除时 FAT 链被清零，无法
 * 复原）在这里不适用：那个限制针对的是已删除文件，这里走的是现存
 * （未删除）目录的簇链，FAT 表项完整可读，不存在这个问题。
 */
#ifndef ENGINE_CORE_FAT_FAT_CHAIN_H
#define ENGINE_CORE_FAT_FAT_CHAIN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
	FAT_FAT_CHAIN_WALK_OK          = 1,  /* 正常走到 EOF 标记（>= 0x0FFFFFF8）结束 */
	FAT_FAT_CHAIN_WALK_HIT_FREE    = 2,  /* 走到一个标记为空闲（0x00000000）的簇，
	                                       * 链提前中断——数据可能已经被部分覆盖/损坏 */
	FAT_FAT_CHAIN_WALK_BAD_CLUSTER = 3,  /* 走到一个标记为坏簇（0x0FFFFFF7）的值，
	                                       * 数据损坏 */
	FAT_FAT_CHAIN_WALK_OUT_OF_RANGE = 4, /* 走到一个超出卷簇号范围的值，数据损坏 */
	FAT_FAT_CHAIN_WALK_TOO_LONG    = 5   /* 达到 max_clusters 兜底上限还没遇到 EOF，
	                                       * 防御环形引用/损坏数据导致死循环 */
}
fat_fat_chain_walk_status_t;

/* 从 start_cluster 开始，沿着 FAT 表走簇链，把经过的簇号依次写进
 * out_clusters（调用方提供的数组，容量至少是 max_clusters）。
 *
 * total_data_clusters 是这个卷的数据簇总数，用于判断簇号是否超出合法
 * 范围（合法簇号是 [2, total_data_clusters+1]）。max_clusters 是防御性的
 * 最大簇数兜底，和 exfat_fat_chain_walk 同样的设计哲学。
 *
 * FAT32 的 FAT 表项是 32 位小端，但只有低 28 位有意义（高 4 位保留，
 * 读取时会被屏蔽掉，不参与 EOF/坏簇判断）。
 *
 * 返回 1 表示函数本身执行成功（走到哪一步、为什么停，看 *out_status 和
 * *out_cluster_count，不代表链一定完整），-1 表示参数不合法。
 */
int fat_fat_chain_walk(
     const uint8_t *fat_table_data,
     size_t fat_table_data_size,
     uint32_t start_cluster,
     uint32_t total_data_clusters,
     uint32_t max_clusters,
     uint32_t *out_clusters,
     uint32_t *out_cluster_count,
     fat_fat_chain_walk_status_t *out_status );

#ifdef __cplusplus
}
#endif

#endif /* ENGINE_CORE_FAT_FAT_CHAIN_H */
