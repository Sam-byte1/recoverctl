/*
 * functional core：解析单条 32 字节的 FAT 目录项，判断它是正常文件、
 * 已删除但可恢复、目录结束标记、还是 VFAT 长文件名片段/卷标。
 *
 * 这个函数只负责解析"单独一条"目录项，不负责把多条 VFAT 长文件名片段
 * 拼接成完整文件名——那是跨多条目录项的状态（哪些片段属于同一个文件），
 * 这个函数处理不了，由调用方（engine/core/fat_undelete_scanner.c）负责
 * 拼接，见那边的说明。
 *
 * 范围说明：
 * - 只处理短文件名（8.3 格式）的解析，VFAT 长文件名条目（属性字节 == 0x0F）
 *   会被识别出来并把它携带的 13 个 UTF-16LE 字符原样取出（kind ==
 *   LONG_NAME_PART 时 `long_name_part_utf16le` 有效），但"这是第几个
 *   片段"这个序号信息**不使用**（2026-11-05 小树裁决第13步说明：文件被
 *   删除时，长文件名条目的序号字节和短名条目的首字节一样会被 0xE5
 *   覆盖，删除后已经不可靠；调用方改用"物理相邻 + 倒序拼接"的方式重建，
 *   不依赖这个序号）
 * - 已删除条目的**短文件名**第一个字符在删除时已经被 0xE5 覆盖、永久
 *   丢失，这是 FAT 规范本身的限制，不是我们代码的缺陷；本函数用下划线
 *   `_` 占位，这是数据恢复工具的通用约定。**但长文件名条目携带的字符
 *   内容完全不受删除操作影响**（删除只改每条目录项的第 1 个字节，即
 *   长文件名条目的"序号"字节，不碰后面存字符的字节）——这意味着只要
 *   一个被删除的文件原本有长文件名，这个长文件名能够被完整、准确地
 *   恢复，不像短文件名那样永久丢失首字符，这是这次增量里一个值得记录
 *   的真实发现，不是我们代码额外做了什么特殊处理
 */
#ifndef ENGINE_CORE_FAT_DIR_ENTRY_H
#define ENGINE_CORE_FAT_DIR_ENTRY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
	FAT_DIR_ENTRY_KIND_END_OF_DIRECTORY = 0,  /* 首字节 0x00：这条之后不会再有条目 */
	FAT_DIR_ENTRY_KIND_DELETED          = 1,  /* 首字节 0xE5：已删除但可能可恢复 */
	FAT_DIR_ENTRY_KIND_LONG_NAME_PART   = 2,  /* VFAT 长文件名条目，本轮范围之外 */
	FAT_DIR_ENTRY_KIND_VOLUME_LABEL     = 3,  /* 卷标条目，不是真实文件 */
	FAT_DIR_ENTRY_KIND_NORMAL           = 4   /* 正常、未删除的文件/目录条目 */
}
fat_dir_entry_kind_t;

typedef struct
{
	fat_dir_entry_kind_t kind;
	char short_name[ 13 ];   /* 形如 "HELLO.TXT"，只在 DELETED/NORMAL 时有效 */
	uint32_t first_cluster;
	uint32_t file_size;
	int is_directory;

	/* 只在 kind == LONG_NAME_PART 时有效：这条目录项携带的 13 个 UTF-16LE
	 * 字符，原始字节（小端，13*2=26 字节），还没有做任何拼接或转码——
	 * 单条目录项不知道自己在整个长文件名里是第几段、也不知道后面还有
	 * 没有更多段，这些都要调用方跨多条目录项才能判断 */
	uint8_t long_name_part_utf16le[ 26 ];
}
fat_dir_entry_t;

/* 解析一条 32 字节的目录项。entry_data_size 必须 >= 32。
 * 返回 1 成功（成功不代表 kind 是 NORMAL/DELETED，看 out->kind），
 * -1 表示输入参数不合法。
 */
int fat_dir_entry_parse(
     const uint8_t *entry_data,
     size_t entry_data_size,
     fat_dir_entry_t *out );

#ifdef __cplusplus
}
#endif

#endif /* ENGINE_CORE_FAT_DIR_ENTRY_H */
