/*
 * functional core：解析 NTFS 的 64 位"文件引用"（file reference）。
 *
 * NTFS 规范里，一个文件引用把两件事打包进同一个 64 位整数：低 48 位是
 * MFT 条目号（record number），高 16 位是序列号（sequence number，每次
 * 这个 MFT 槽位被重新分配给新文件时递增，用来检测"引用的是不是已经
 * 被别的文件复用过的旧槽位"）。$FILE_NAME 属性里记录的"父目录引用"
 * 就是这种格式，要先解包出纯粹的 MFT 索引号才能拿去调用
 * `libfsntfs_volume_get_file_entry_by_index`（那个 API 只接受纯索引号，
 * 不接受打包了序列号的引用）。
 */
#ifndef ENGINE_CORE_NTFS_FILE_REFERENCE_H
#define ENGINE_CORE_NTFS_FILE_REFERENCE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* NTFS 卷的根目录永远是 MFT 索引 5——这是 NTFS 规范里的固定值，不是
 * 某个具体卷的巧合。根目录的"父目录引用"字段指向它自己（形成自环），
 * 所以沿父目录链往上走时，必须在走到索引 5 的时候主动停下来，不能
 * 再往下处理，否则会死循环。 */
#define NTFS_ROOT_DIRECTORY_MFT_INDEX 5

/* 从打包的 64 位文件引用里解出纯粹的 MFT 条目索引号（低 48 位）。
 * 纯算术运算，不做任何校验（调用方传什么都有确定的返回值）。
 */
uint64_t ntfs_file_reference_to_mft_index( uint64_t file_reference );

#ifdef __cplusplus
}
#endif

#endif /* ENGINE_CORE_NTFS_FILE_REFERENCE_H */
