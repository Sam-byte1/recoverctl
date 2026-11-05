/*
 * functional core：判断一个 MFT 条目是不是"已删除但数据未被覆盖"的候选项。
 *
 * 这是一个纯函数边界：输入是调用方已经通过
 * libfsntfs_volume_get_file_entry_by_index 拿到的 file_entry，
 * 本函数自己不做任何磁盘 I/O、不修改任何状态，给定同样的 file_entry
 * 永远返回同样的结果——这是它能被 TDD 覆盖的原因。
 */
#ifndef ENGINE_CORE_UNDELETE_DETECTOR_H
#define ENGINE_CORE_UNDELETE_DETECTOR_H

#include <libfsntfs.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UNDELETE_CANDIDATE_NAME_MAX_SIZE 256

typedef struct
{
	/* 1 = 已删除但可恢复（未分配 + 仍有 $FILE_NAME），0 = 不是 */
	int is_deleted_recoverable;

	/* 仅在 is_deleted_recoverable == 1 时有效，UTF-8，以 \0 结尾 */
	char name[ UNDELETE_CANDIDATE_NAME_MAX_SIZE ];

	/* 对应的 MFT 条目索引，原样回传方便调用方定位 */
	uint64_t mft_entry_index;

	/* 父目录的 MFT 条目索引（已经从 $FILE_NAME 属性的打包文件引用里解出
	 * 纯索引号，调用方不需要再处理序列号）。仅在 is_deleted_recoverable
	 * == 1 时有效。用于后续重建完整路径（见 engine/shell/path_resolver.{h,c}）
	 * ——这里只是把"父目录是谁"这个已经顺手拿到的信息记下来，不在这个
	 * 纯函数里做任何真实的路径重建 I/O，保持这个函数的"不做磁盘 I/O"边界。 */
	uint64_t parent_mft_index;
}
undelete_candidate_t;

/* 检查一个 file_entry 是否是"已删除但可恢复"的候选项。
 * 返回 1 表示判断流程本身成功完成（不代表一定是候选项，看 out->is_deleted_recoverable）。
 * 返回 -1 表示判断过程中出现了无法处理的错误。
 */
int undelete_detector_inspect(
     libfsntfs_file_entry_t *file_entry,
     uint64_t mft_entry_index,
     undelete_candidate_t *out,
     libfsntfs_error_t **error );

#ifdef __cplusplus
}
#endif

#endif /* ENGINE_CORE_UNDELETE_DETECTOR_H */
