/*
 * functional core：解析 exFAT 根目录里的一个"目录项集合"（entry set）。
 *
 * exFAT 和 FAT32 最大的结构性差异就在这里：FAT32 一个 32 字节目录项就是
 * 一个完整文件的全部信息，可以逐条独立处理；exFAT 一个文件至少占 3 条
 * 连续的 32 字节目录项（File 目录项 + Stream Extension 目录项 + 至少 1 条
 * File Name 目录项），必须按 File 目录项里的 SecondaryCount 字段知道
 * 要往后再读几条才能拼出完整信息，本模块因此是"消费 N 条、返回消费了几条"
 * 这种游标式接口，不是 FAT32 那种"一条进一条出"。
 *
 * exFAT 的删除机制（和 FAT32 不同，范围说明见下）：删除时只清掉 entry set
 * 里每一条目录项 EntryType 字节的最高位（InUse 位，0x80），其余字节（文件名、
 * 大小、起始簇号）原样保留——不像 FAT32 会永久破坏文件名第一个字符。这是
 * exFAT 删除恢复成功率通常更高的原因，不是我们代码额外做了什么。
 *
 * 范围说明（这一轮刻意收窄）：
 * - 只支持文件名 <= 15 个 UTF-16 字符（正好 1 条 File Name 目录项装得下）；
 *   需要多条 File Name 目录项拼接的超长文件名不在这一轮范围内，遇到就
 *   跳过当作"本轮不处理"，不会误判或崩溃
 * - 非 ASCII 字符（中文/日文等）的正确转码 **2026-11-05 小树裁决第13步
 *   已修复**：原来"每个 UTF-16 码元取低字节"的简化处理对非 ASCII 字符
 *   会产生垃圾字节，是真实 bug 不是合理简化；现在用
 *   `engine/core/utf16_to_utf8.c` 做正确的 UTF-16LE→UTF-8 转换，和
 *   FAT32 的 VFAT 长文件名共用同一份转换逻辑
 * - 位图（0x81）、大写表（0x82）、卷标（0x83）这些系统目录项按"消费 1 条、
 *   跳过"处理，不展开解析内容——我们的恢复逻辑不需要这些信息
 */
#ifndef ENGINE_CORE_EXFAT_DIR_ENTRY_H
#define ENGINE_CORE_EXFAT_DIR_ENTRY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
	EXFAT_DIR_ENTRY_SET_KIND_END_OF_DIRECTORY = 0,  /* EntryType 字节 0x00：之后不会再有条目 */
	EXFAT_DIR_ENTRY_SET_KIND_DELETED_FILE     = 1,  /* File 目录项的 InUse 位被清掉 */
	EXFAT_DIR_ENTRY_SET_KIND_NORMAL_FILE      = 2,  /* 正常、未删除的文件/目录 */
	EXFAT_DIR_ENTRY_SET_KIND_SKIPPED          = 3   /* 位图/大写表/卷标/超出本轮范围的条目，
	                                                  * 调用方应该跳过 consumed_entries 条继续扫描 */
}
exfat_dir_entry_set_kind_t;

typedef struct
{
	exfat_dir_entry_set_kind_t kind;
	char name[ 256 ];      /* 只在 NORMAL_FILE/DELETED_FILE 时有效，正确的 UTF-8（见上方范围说明） */
	uint32_t first_cluster;
	uint64_t file_size;
	int is_directory;
	int no_fat_chain;      /* Stream Extension 的 NoFatChain 标志位：1 表示数据连续存储，
	                         * 不需要跟踪 FAT 链——这个 fixture 和本轮范围内恢复的文件
	                         * 都假设是这种情况 */
	size_t consumed_entries;  /* 这次解析消费了多少条 32 字节目录项，调用方据此推进游标，
	                            * 至少是 1（即使是 SKIPPED/END_OF_DIRECTORY 也一样） */
}
exfat_dir_entry_set_t;

/* 从 entries_data 的第 entry_index 条（每条 32 字节）开始解析一个目录项集合。
 * entries_data_size 是整个缓冲区的大小（字节），用于防止因为 SecondaryCount
 * 声称的条目数超出缓冲区范围而越界读取。
 * 返回 1 成功（成功不代表 kind 是 NORMAL_FILE，看 out->kind），
 * -1 表示输入参数不合法，或者 SecondaryCount/条目结构明显损坏导致无法安全解析。
 */
int exfat_dir_entry_set_parse(
     const uint8_t *entries_data,
     size_t entries_data_size,
     size_t entry_index,
     exfat_dir_entry_set_t *out );

#ifdef __cplusplus
}
#endif

#endif /* ENGINE_CORE_EXFAT_DIR_ENTRY_H */
