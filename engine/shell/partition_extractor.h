/*
 * imperative shell：把一段分区（给定设备路径+字节偏移+大小）拷贝到一个
 * 临时文件——原来是`cli/main.c`内部的`static`函数（`extract_partition_
 * to_temp_file`/`get_partition_temp_file_path`），2026-11-06小树裁决从
 * `cli/main.c`抽出来，供`recover-carve-partition`命令和GUI的
 * `CarveDialog`"按分区范围雕刻"分支共享同一份实现，不是重新发明一遍。
 *
 * 平台相关的部分收窄到只剩"怎么打开/定位一个裸设备句柄"（Windows
 * `CreateFileA`+`ReadFile`，POSIX `fopen`+`fread`），真正的"扇区对齐+
 * 坏道容忍+循环拷贝"逻辑复用`aligned_device_reader_copy_to_file`，和
 * `fat_reader.c`/`exfat_reader.c`同一套实现，不是另起一份。
 */
#ifndef ENGINE_SHELL_PARTITION_EXTRACTOR_H
#define ENGINE_SHELL_PARTITION_EXTRACTOR_H

#include <stddef.h>
#include <stdint.h>

#include "../core/bad_sector_log.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 把系统临时目录+固定文件名拼成一个绝对路径——不用相对路径，因为
 * libfsntfs打开卷时会把传入路径解析成绝对路径，依赖"当前工作目录"
 * 是一个能被正常查询到的本地路径，这个前提在"双击运行/计划任务/
 * 网络共享盘脚本启动"这些场景下不总是成立（真实撞上过的坑，详见
 * cli/main.c历史记录）。
 *
 * 返回 1 成功，-1 失败（系统临时目录查询失败，或者 out_path_size
 * 装不下结果）。
 */
int partition_extractor_get_temp_file_path(
     char *out_path,
     size_t out_path_size );

/* 从device_path指向的设备/镜像、偏移offset处开始，拷贝size字节到
 * temp_path。out_bad_sector_log非NULL时启用坏道容忍（重试耗尽后用0
 * 填充该段、记一笔坏道日志、继续拷贝，不中断整次拷贝），为NULL时
 * 遇到读取失败直接返回-1。
 *
 * 返回 1 成功，-1 失败（设备打不开、定位失败、或未启用坏道容忍时
 * 读取/写入失败）。
 */
int partition_extractor_extract_to_temp_file(
     const char *device_path,
     long offset,
     uint64_t size,
     bad_sector_log_t *out_bad_sector_log,
     const char *temp_path );

#ifdef __cplusplus
}
#endif

#endif /* ENGINE_SHELL_PARTITION_EXTRACTOR_H */
