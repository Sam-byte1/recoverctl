/*
 * functional core：对一段目录区域的原始字节（按 32 字节一条）做全量扫描，
 * 收集"已删除但可能可以恢复"的候选项；另外提供"找出这个目录里有哪些
 * 正常存在的子目录"的能力，供调用方（shell 层）决定要不要继续往下递归。
 *
 * 范围说明：
 * - 子目录遍历（2026-11-05 小树裁决第12步）：只验证一层子目录（根目录 ->
 *   子目录），不递归到孙子目录，更深层级留作后续增量；子目录本身假设
 *   只占 1 个簇，和根目录的既有假设一致，不跟随 FAT 链
 * - VFAT 长文件名解析（2026-11-05 小树裁决第13步，**已支持**）：一个文件
 *   如果有长文件名，紧邻它的短名条目前面会有若干条 VFAT 长文件名条目
 *   （属性字节 0x0F），本模块在扫描过程中会缓冲这些条目、按"物理相邻 +
 *   倒序拼接"的方式重建完整文件名（不依赖条目里的序号字节，原因见
 *   `engine/core/fat_dir_entry.h` 的说明），并做正确的 UTF-16LE→UTF-8
 *   转码（含非 ASCII 字符，复用 `engine/core/utf16_to_utf8.c`）。没有
 *   长文件名的条目继续用 8.3 短名
 */
#ifndef ENGINE_CORE_FAT_UNDELETE_SCANNER_H
#define ENGINE_CORE_FAT_UNDELETE_SCANNER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
	char name[ 256 ];   /* 优先用重组好的 VFAT 长文件名（UTF-8）；没有长文件名
	                      * 就用 8.3 短文件名。和 NTFS/exFAT 候选项的 name 字段
	                      * 用同样的 256 字节大小；极端的超长中日韩文件名可能
	                      * 装不下，`utf16le_to_utf8` 会在字符边界截断，不会
	                      * 越界或者产生半个字符的乱码 */
	uint32_t first_cluster;
	uint32_t file_size;

	/* 这个候选项所在的目录相对路径，根目录传入空字符串时这里也是空字符串；
	 * 子目录递归扫描时由调用方（shell 层）传入子目录名，不含文件名本身。
	 * 这一轮只验证一层，所以这里最多是"SUBDIR"这样的单层名字，不会是
	 * 多级路径——多级拼接可以复用 engine/core/path_builder.c（通用字符串
	 * 工具，NTFS 路径重建那一轮已经验证过），留给以后支持更深层级时用 */
	char directory_path[ 64 ];
}
fat_undelete_candidate_t;

typedef struct
{
	fat_undelete_candidate_t *candidates;
	int count;
	int capacity;
}
fat_undelete_scan_result_t;

/* 一个正常存在（未删除）的子目录的基本信息，供调用方决定要不要递归进去扫描 */
typedef struct
{
	char name[ 256 ];   /* 同上，优先用重组好的 VFAT 长文件名，没有就用短名 */
	uint32_t first_cluster;
}
fat_subdirectory_t;

typedef struct
{
	fat_subdirectory_t *entries;
	int count;
	int capacity;
}
fat_subdirectory_list_t;

/* 扫描一段目录区域（根目录或者某个子目录的那一个簇），把判定为"已删除"的
 * 短文件名条目收集进 out。VFAT 长文件名条目、卷标条目会被跳过，
 * 遇到"目录结束"标记就停止扫描（后面不会再有有效条目）。
 *
 * directory_path 是这段目录区域对应的相对路径（根目录传 ""，子目录传
 * 子目录名），会原样写进每个候选项的 directory_path 字段——这是一个
 * 纯粹的"调用方告诉我这是哪，我原样标记"的参数，不影响扫描逻辑本身，
 * 所以这个函数依然是确定性的纯函数，可以继续用同样的方式做 TDD。
 *
 * 返回 1 成功，-1 失败（内存分配失败、或者 directory_path 太长装不下等）。
 */
int fat_undelete_scanner_scan_directory(
     const uint8_t *directory_data,
     size_t directory_data_size,
     const char *directory_path,
     fat_undelete_scan_result_t *out );

/* 扫描一段目录区域，找出里面所有正常存在（未删除）的子目录——"." 和 ".."
 * 这两个特殊条目会被跳过（它们不是真正的子目录，是目录自引用/父引用）。
 * 返回 1 成功，-1 失败（内存分配失败等）。
 */
int fat_undelete_scanner_find_subdirectories(
     const uint8_t *directory_data,
     size_t directory_data_size,
     fat_subdirectory_list_t *out );

void fat_undelete_scan_result_free(
      fat_undelete_scan_result_t *result );

void fat_subdirectory_list_free(
      fat_subdirectory_list_t *list );

/* 把 src 里的所有候选项追加拷贝进 dest（dest 原有内容保留，不清空）。
 * 用于 shell 层把子目录扫描结果合并进根目录扫描结果。不会释放 src，
 * 调用方自己负责 src 的生命周期。
 * 返回 1 成功，-1 失败（内存分配失败）。
 */
int fat_undelete_scan_result_append_all(
     fat_undelete_scan_result_t *dest,
     const fat_undelete_scan_result_t *src );

#ifdef __cplusplus
}
#endif

#endif /* ENGINE_CORE_FAT_UNDELETE_SCANNER_H */
