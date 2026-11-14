/*
 * functional core：枚举一个 NTFS file_entry 上有哪些具名备用数据流
 * （Alternate Data Streams, ADS）。
 *
 * 和 undelete_detector.c 查 $FILE_NAME 属性是同一类纯函数边界：输入是
 * 调用方已经通过 libfsntfs_volume_get_file_entry_by_index 拿到的
 * file_entry，本函数自己不做任何磁盘 I/O、不修改任何状态——libfsntfs
 * 内部当然会按需读磁盘来解析属性列表，但那是库内部实现细节，不是本函数
 * 自己发起的新 I/O 边界，这和 undelete_detector_inspect 调用
 * libfsntfs_file_entry_get_size 时的边界判断完全一致。
 *
 * 2026-11-14（J1）真机验证确认过的前提（见
 * engine/tests/fixtures/ntfs-ads-undelete/README.md）：已删除文件
 * （is_allocated()==0）的 MFT 记录上，具名 $DATA 属性（ADS）和
 * $FILE_NAME 属性一样完整保留，这个函数对已删除候选项和正常存在的文件
 * 用的是同一套逻辑，不需要也不应该区分"是不是已删除"。
 */
#ifndef ENGINE_CORE_ADS_ENUMERATOR_H
#define ENGINE_CORE_ADS_ENUMERATOR_H

#include <libfsntfs.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ADS_ENUMERATOR_NAME_MAX_SIZE 256

typedef struct
{
	/* UTF-8，以 \0 结尾。不含主数据流（主数据流没有名字，不会出现在
	 * 这个枚举结果里——libfsntfs 的 alternate_data_stream 系列 API
	 * 本来就只枚举具名的 $DATA 属性，这是库自己的语义，不是本函数
	 * 额外过滤出来的）。 */
	char name[ ADS_ENUMERATOR_NAME_MAX_SIZE ];

	/* 这个流的大小，字节数。只有 has_size == 1 时才有意义——读取失败时
	 * 必须保持 has_size == 0，不编造假大小，和 undelete_candidate_t 里
	 * file_size/has_file_size 的"诚实标注限制"纪律一致。 */
	uint64_t size;

	int has_size;
}
ads_stream_info_t;

typedef struct
{
	ads_stream_info_t *streams;
	int count;
	int capacity;
}
ads_stream_list_t;

/* 枚举 file_entry 上的全部具名备用数据流。
 *
 * out 需要调用方先 memset 清零，或保证 streams == NULL、count == 0、
 * capacity == 0。
 *
 * 返回 1 表示枚举流程本身成功完成——即使 out->count == 0 也是成功，
 * 代表这个文件压根没有任何具名备用数据流，这是绝大多数文件的正常情况，
 * 不是错误。返回 -1 表示出现了无法处理的错误（out 会被清空、不会有
 * 悬挂的部分状态）。
 */
int ads_enumerator_list_streams(
     libfsntfs_file_entry_t *file_entry,
     ads_stream_list_t *out,
     libfsntfs_error_t **error );

/* 释放 ads_enumerator_list_streams 分配的内存 */
void ads_stream_list_free(
      ads_stream_list_t *list );

#ifdef __cplusplus
}
#endif

#endif /* ENGINE_CORE_ADS_ENUMERATOR_H */
