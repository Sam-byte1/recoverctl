/*
 * functional core：一份"坏道日志"——记录恢复/扫描过程中跳过了哪些区域
 * （相对恢复目标起始位置的偏移 + 长度），供调用方（CLI）在恢复结束后
 * 汇总展示给用户，而不是让这些信息随着跳过动作本身就悄悄消失。
 *
 * 和项目里其它动态数组（fat_undelete_scan_result_t 等）同样的增长策略，
 * 不是重新发明一套，保持代码库风格一致。
 */
#ifndef ENGINE_CORE_BAD_SECTOR_LOG_H
#define ENGINE_CORE_BAD_SECTOR_LOG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
	uint64_t offset;  /* 相对这次恢复/读取起点的字节偏移，不是绝对磁盘偏移——
	                    * 调用方如果需要绝对偏移，自己加上起始位置 */
	uint64_t length;  /* 跳过的字节数 */
}
bad_sector_record_t;

typedef struct
{
	bad_sector_record_t *records;
	int count;
	int capacity;
}
bad_sector_log_t;

/* 追加一条坏道记录。返回 1 成功，-1 失败（内存分配失败）。
 * log 使用前必须先 memset 清零（和项目里其它动态数组结构体一致的约定）。
 */
int bad_sector_log_append(
     bad_sector_log_t *log,
     uint64_t offset,
     uint64_t length );

/* 跳过的总字节数，方便调用方做汇总展示，不用自己再遍历一遍 records */
uint64_t bad_sector_log_total_bytes_skipped(
          const bad_sector_log_t *log );

void bad_sector_log_free(
      bad_sector_log_t *log );

#ifdef __cplusplus
}
#endif

#endif /* ENGINE_CORE_BAD_SECTOR_LOG_H */
