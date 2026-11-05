/*
 * imperative shell：用 libvsgpt 解析一个磁盘镜像/设备的 GPT 分区表，
 * 绕开操作系统的卷管理——这是"未挂载/丢失分区识别"的地基。
 *
 * 和 FAT32 那部分不同：libvsgpt 的 README 虽然也写"only focuses on
 * the analysis of the format"，但它的公开 API（get_number_of_partitions/
 * get_partition_by_index/get_volume_offset/get_size/get_type_identifier）
 * 已经完全覆盖"列出分区边界/类型/大小"这个需求，不存在 libfsfat 那种
 * 能力缺口，所以这里直接复用 libvsgpt，不需要像 FAT32 那样自研底层解析
 * （这个判断已经在 2026-11-04 的开发日志里记录过，提前核实过 API 再动手）。
 *
 * 范围说明（这一轮先做 GPT，MBR 用 libvsmbr 的同等支持留作下一个小增量，
 * 两边 API 形状几乎一样，不是遗漏，是刻意分两步交付）。
 */
#ifndef ENGINE_SHELL_PARTITION_READER_H
#define ENGINE_SHELL_PARTITION_READER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
	int index;
	uint64_t offset;          /* 分区在整块磁盘里的字节偏移 */
	uint64_t size;             /* 分区大小，字节 */
	uint8_t type_guid[ 16 ];   /* GPT 分区类型 GUID（原始 16 字节，混合端序） */
}
gpt_partition_info_t;

typedef struct
{
	gpt_partition_info_t *partitions;
	int count;
	int capacity;
}
gpt_partition_scan_result_t;

/* 打开一个磁盘镜像/设备路径，解析它的 GPT 分区表。
 * 返回 1 成功，-1 失败（打不开，或者解析出来发现不是合法 GPT）。
 */
int partition_reader_scan_gpt(
     const char *device_path,
     gpt_partition_scan_result_t *out );

void gpt_partition_scan_result_free(
      gpt_partition_scan_result_t *result );

typedef struct
{
	int index;
	uint64_t offset;
	uint64_t size;
	uint8_t type;   /* MBR 分区类型是单字节代码（不是 GUID），比如 0x07=NTFS，0x0C=FAT32(LBA) */
}
mbr_partition_info_t;

typedef struct
{
	mbr_partition_info_t *partitions;
	int count;
	int capacity;
}
mbr_partition_scan_result_t;

/* 打开一个磁盘镜像/设备路径，解析它的 MBR 分区表。
 * 返回 1 成功，-1 失败。
 */
int partition_reader_scan_mbr(
     const char *device_path,
     mbr_partition_scan_result_t *out );

void mbr_partition_scan_result_free(
      mbr_partition_scan_result_t *result );

#ifdef __cplusplus
}
#endif

#endif /* ENGINE_SHELL_PARTITION_READER_H */
