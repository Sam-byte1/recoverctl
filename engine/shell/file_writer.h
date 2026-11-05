/*
 * imperative shell：把一个 file_entry 的内容写到本地磁盘文件。
 * 这是真正的 I/O 操作，不走 TDD，靠端到端跑一遍 + 哈希校验来验证
 * （见 Task #7 / engine/tests/fixtures 下的 fixture）。
 */
#ifndef ENGINE_SHELL_FILE_WRITER_H
#define ENGINE_SHELL_FILE_WRITER_H

#include <libfsntfs.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 把 file_entry 默认数据流（无名 $DATA 属性）的内容写到 output_path。
 * 返回 1 成功，-1 失败（具体原因看 error）。
 */
int file_writer_write_entry_content(
     libfsntfs_file_entry_t *file_entry,
     const char *output_path,
     libfsntfs_error_t **error );

#ifdef __cplusplus
}
#endif

#endif /* ENGINE_SHELL_FILE_WRITER_H */
