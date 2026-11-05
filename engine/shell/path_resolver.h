/*
 * imperative shell：沿 NTFS 父目录链一层层往上走，重建一个 MFT 条目的
 * 完整相对路径。和 file_writer.c/disk_enumerator.c 的定位一致——这里
 * 做的是真实的卷 I/O（每一跳都要打开一个新的 file_entry），不是纯函数，
 * 不走 TDD，靠端到端脚本 + 真实 fixture 的已知目录结构验证（详见
 * engine/tests/fixtures/ntfs-basic-undelete/README.md 里
 * a_directory\another_file 这个真实存在的嵌套路径场景）。
 *
 * 实际拼接路径字符串这个纯逻辑部分已经抽到 engine/core/path_builder.{h,c}
 * 单独做了 TDD 覆盖，这里只负责"走哪一步、什么时候停"这个真实 I/O 编排。
 *
 * 范围限制（刻意收窄，和这个项目一贯的风格一致）：
 * - 只处理"沿途每一层父目录都还能正常打开、还能读到 $FILE_NAME"的情况；
 *   任何一环失败（父目录本身也被删除、MFT 记录已被复用等）就停止并把
 *   out_is_complete 标成 0，返回已经拼到的部分路径，不是整体失败
 * - 有一个硬编码的最大深度限制（防御真实损坏数据导致的环形引用），
 *   超过这个深度也会停止并标记 is_complete=0
 * - 一个文件如果有多个 $FILE_NAME 属性（比如系统同时保留了 Win32 长名和
 *   8.3 短名），只取遇到的第一个——和 undelete_detector.c 对叶子文件名的
 *   处理方式保持一致，不是遗漏
 */
#ifndef ENGINE_SHELL_PATH_RESOLVER_H
#define ENGINE_SHELL_PATH_RESOLVER_H

#include <stddef.h>
#include <stdint.h>

#include <libfsntfs.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 从 parent_mft_index（通常是某个已删除文件自己的父目录索引，undelete_candidate_t
 * 里已经有这个字段）开始，一路往上走到 NTFS 根目录（MFT 索引 5），把沿途
 * 每一层的目录名拼成一个相对路径（比如 "a_directory"、"a\\b\\c"），不包含
 * 最终文件自己的名字——那个调用方已经有了，自己拼到这个函数返回的路径
 * 后面即可。如果 parent_mft_index 本身就是根目录（文件直接在根目录下），
 * 返回的 out_path 是空字符串，out_is_complete 是 1。
 *
 * 返回 1 表示函数本身执行完成（不代表路径一定完整，看 out_is_complete），
 * -1 表示参数不合法。
 */
int path_resolver_reconstruct_ancestor_path(
     libfsntfs_volume_t *volume,
     uint64_t parent_mft_index,
     char *out_path,
     size_t out_path_size,
     int *out_is_complete,
     libfsntfs_error_t **error );

#ifdef __cplusplus
}
#endif

#endif /* ENGINE_SHELL_PATH_RESOLVER_H */
