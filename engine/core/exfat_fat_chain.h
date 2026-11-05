/*
 * functional core：给定 exFAT FAT 表的原始字节和一个起始簇号，走出完整的
 * 簇链（哪个簇后面接哪个簇，直到遇到 EOF 标记）。
 *
 * 背景（2026-11-05 小树裁决第15步真机验证+第16步裁决）：exFAT 的
 * StreamExtension 目录项有一个 NoFatChain 标志位，为 1 时数据连续存储，
 * 不需要查 FAT 表（`exfat_reader_recover_file` 原有的"假设连续"逻辑覆盖
 * 这种情况）；为 0 时数据是碎片化存储，必须跟着 FAT 表里的链式指针逐簇
 * 找到完整内容。第15步真机实测已经证实：exFAT 删除文件时只清目录项的
 * InUse 位，FAT 表项完全不受影响，碎片文件的完整簇链在删除后依然原样
 * 保留——这是这个模块能够工作的前提，不是假设。
 *
 * 这里只负责"给定 FAT 表字节，走出簇号序列"这个纯计算，不做任何磁盘 I/O
 * （真实读取 FAT 表、按簇号读数据这些是 shell 层 exfat_reader.c 的职责）。
 */
#ifndef ENGINE_CORE_EXFAT_FAT_CHAIN_H
#define ENGINE_CORE_EXFAT_FAT_CHAIN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
	EXFAT_FAT_CHAIN_WALK_OK          = 1,  /* 正常走到 EOF 标记（0xFFFFFFFF）结束 */
	EXFAT_FAT_CHAIN_WALK_HIT_FREE    = 2,  /* 走到一个标记为空闲（0x00000000）的簇，
	                                         * 链提前中断——数据可能已经被部分覆盖/损坏 */
	EXFAT_FAT_CHAIN_WALK_OUT_OF_RANGE = 3, /* 走到一个超出卷簇号范围的值，数据损坏 */
	EXFAT_FAT_CHAIN_WALK_TOO_LONG    = 4   /* 达到 max_clusters 兜底上限还没遇到 EOF，
	                                         * 防御环形引用/损坏数据导致死循环 */
}
exfat_fat_chain_walk_status_t;

/* 从 start_cluster 开始，沿着 FAT 表走簇链，把经过的簇号依次写进
 * out_clusters（调用方提供的数组，容量至少是 max_clusters）。
 *
 * cluster_count 是这个卷的数据簇总数（来自引导扇区 ClusterCount 字段），
 * 用于判断簇号是否超出合法范围（合法簇号是 [2, cluster_count+1]）。
 * max_clusters 是防御性的最大簇数兜底，和项目里其它地方的
 * *_MAX_RECURSION_DEPTH 同样的设计哲学——不是真实场景的硬限制，纯粹防止
 * 损坏数据/理论上的环形引用导致无限循环。
 *
 * 返回 1 表示函数本身执行成功（走到哪一步、为什么停，看 *out_status 和
 * *out_cluster_count，不代表链一定完整), -1 表示参数不合法。
 */
int exfat_fat_chain_walk(
     const uint8_t *fat_table_data,
     size_t fat_table_data_size,
     uint32_t start_cluster,
     uint32_t cluster_count,
     uint32_t max_clusters,
     uint32_t *out_clusters,
     uint32_t *out_cluster_count,
     exfat_fat_chain_walk_status_t *out_status );

#ifdef __cplusplus
}
#endif

#endif /* ENGINE_CORE_EXFAT_FAT_CHAIN_H */
