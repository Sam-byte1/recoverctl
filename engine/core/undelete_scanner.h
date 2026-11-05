/*
 * functional core：对整个卷按 MFT 索引做全表扫描，找出所有
 * "已删除但可恢复"的候选项。不依赖目录遍历/索引，这是关键设计点
 * （见 engine/tests/fixtures/ntfs-basic-undelete/README.md 的说明）。
 *
 * 内部就是对每个索引调用 undelete_detector_inspect，本身不引入
 * 新的判断逻辑，只负责"遍历 + 收集"。
 */
#ifndef ENGINE_CORE_UNDELETE_SCANNER_H
#define ENGINE_CORE_UNDELETE_SCANNER_H

#include <libfsntfs.h>

#include "undelete_detector.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
	undelete_candidate_t *candidates;
	int count;
	int capacity;
}
undelete_scan_result_t;

/* 扫描整个卷，把找到的候选项收集到 out 里（out 需要先用 memset 清零或保证
 * candidates == NULL、count == 0、capacity == 0）。
 * 返回 1 表示扫描流程本身成功完成，-1 表示出现无法处理的错误。
 */
int undelete_scanner_scan_volume(
     libfsntfs_volume_t *volume,
     undelete_scan_result_t *out,
     libfsntfs_error_t **error );

/* 释放 undelete_scanner_scan_volume 分配的内存 */
void undelete_scan_result_free(
      undelete_scan_result_t *result );

#ifdef __cplusplus
}
#endif

#endif /* ENGINE_CORE_UNDELETE_SCANNER_H */
