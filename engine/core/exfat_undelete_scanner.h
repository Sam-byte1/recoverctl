/*
 * functional core：对一段 exFAT 目录区域的原始字节做全量扫描，收集
 * "已删除但可能可以恢复"的候选项；另外提供"找出这个目录里有哪些正常
 * 存在的子目录"的能力，供调用方（shell 层）决定要不要继续往下递归。
 *
 * 范围说明：
 * - VFAT 长文件名、非 ASCII 转码依然不在这一轮范围内
 *
 * （原范围收窄说明"只验证一层子目录"和"子目录本身假设只占 1 个簇"已经
 * 分别在第14步、第21步解除——现在支持任意深度递归和任意簇数的目录，
 * 详见 docs/开发日志.md 对应记录）
 */
#ifndef ENGINE_CORE_EXFAT_UNDELETE_SCANNER_H
#define ENGINE_CORE_EXFAT_UNDELETE_SCANNER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
	char name[ 256 ];
	uint32_t first_cluster;
	uint64_t file_size;

	/* 这个候选项所在的目录相对路径，根目录传入空字符串时这里也是空字符串；
	 * 子目录递归扫描时由调用方（shell 层）传入子目录名。和 FAT32 版本
	 * 同样的设计，这一轮只验证一层，不是多级路径 */
	char directory_path[ 64 ];

	/* Stream Extension 的 NoFatChain 标志位透传（2026-11-05 小树裁决第16步）：
	 * 1 表示数据连续存储，恢复时直接从 first_cluster 顺序读 file_size 字节；
	 * 0 表示碎片化存储，必须跟着 FAT 表的簇链走（见 exfat_fat_chain.h）。
	 * 第15步真机验证已确认 exFAT 删除不会破坏 FAT 表项，这个标志位和对应
	 * 的簇链信息在删除后依然可靠 */
	int no_fat_chain;
}
exfat_undelete_candidate_t;

typedef struct
{
	exfat_undelete_candidate_t *candidates;
	int count;
	int capacity;
}
exfat_undelete_scan_result_t;

/* 一个正常存在（未删除）的子目录的基本信息，供调用方决定要不要递归进去扫描。
 *
 * no_fat_chain/file_size（2026-11-05 第21步，"目录单簇假设"修复）：子目录
 * 本身的 StreamExtension 的 NoFatChain 标志位和声明大小，透传给调用方
 * （shell 层），用来判断子目录内容占用了几个簇、该不该跟着 FAT 表走链——
 * 和 exfat_undelete_candidate_t 里同名字段完全一样的语义，子目录在 exFAT
 * 里本质上也是一种"文件"（attributes 标了目录位而已），遵循同一套
 * StreamExtension 规则。 */
typedef struct
{
	char name[ 256 ];
	uint32_t first_cluster;
	uint64_t file_size;
	int no_fat_chain;
}
exfat_subdirectory_t;

typedef struct
{
	exfat_subdirectory_t *entries;
	int count;
	int capacity;
}
exfat_subdirectory_list_t;

/* 扫描一段目录区域（通常是根目录或者某个子目录那个簇的原始字节），把判定为
 * "已删除"的文件收集进 out。位图/大写表/卷标/超出本轮范围（长文件名等）
 * 的条目会被跳过，遇到"目录结束"标记就停止扫描。
 *
 * directory_path 原样写进每个候选项的 directory_path 字段（根目录传 ""，
 * 子目录传子目录名），不影响扫描逻辑本身，函数依然是确定性纯函数。
 *
 * 返回 1 成功，-1 失败（内存分配失败等）。
 */
int exfat_undelete_scanner_scan_directory(
     const uint8_t *directory_data,
     size_t directory_data_size,
     const char *directory_path,
     exfat_undelete_scan_result_t *out );

/* 扫描一段目录区域，找出里面所有正常存在（未删除）的子目录 */
int exfat_undelete_scanner_find_subdirectories(
     const uint8_t *directory_data,
     size_t directory_data_size,
     exfat_subdirectory_list_t *out );

void exfat_undelete_scan_result_free(
      exfat_undelete_scan_result_t *result );

void exfat_subdirectory_list_free(
      exfat_subdirectory_list_t *list );

/* 把 src 里的所有候选项追加拷贝进 dest（dest 原有内容保留，不清空）。
 * 不会释放 src，调用方自己负责 src 的生命周期。
 * 返回 1 成功，-1 失败（内存分配失败）。
 */
int exfat_undelete_scan_result_append_all(
     exfat_undelete_scan_result_t *dest,
     const exfat_undelete_scan_result_t *src );

#ifdef __cplusplus
}
#endif

#endif /* ENGINE_CORE_EXFAT_UNDELETE_SCANNER_H */
