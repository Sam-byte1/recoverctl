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

/* 把 file_entry 数据流的内容写到 output_path。
 *
 * stream_name == NULL：写主数据流（无名 $DATA 属性），和这个函数改名之前
 * 完全一样的行为。
 * stream_name != NULL：写这个名字对应的具名备用数据流（ADS，2026-11-14
 * J2新增）——用 libfsntfs_file_entry_get_alternate_data_stream_by_utf8_name
 * 拿到 data_stream 后改用 libfsntfs_data_stream_read_buffer 读，不是
 * file_entry_read_buffer。这两条路径复用同一套"打开输出文件+循环读写"
 * 逻辑，不是另外写一套平行实现；坏道容忍/重试机制天然继承（它在
 * engine/shell/ntfs_bfio_adapter.c这一层的自定义libbfio_handle上生效，
 * 不管上层是读主数据流还是读具名数据流，最终都会落到同一个被注入的
 * libbfio_handle上做真正的设备I/O，和K系列验证"稀疏文件零填充天然
 * 继承"是同一个原理）。
 *
 * 返回 1 成功，-1 失败（具体原因看 error；stream_name非NULL但这个
 * 名字的ADS不存在也会返回-1，不会静默改写主数据流）。
 */
int file_writer_write_entry_content(
     libfsntfs_file_entry_t *file_entry,
     const char *stream_name,
     const char *output_path,
     libfsntfs_error_t **error );

#ifdef __cplusplus
}
#endif

#endif /* ENGINE_SHELL_FILE_WRITER_H */
