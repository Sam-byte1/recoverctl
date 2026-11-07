/*
 * functional core：计算"对真实设备做扇区对齐读取"需要的请求长度，以及
 * 读回来之后真正有用的字节数。
 *
 * 背景（详见 docs/开发日志.md 2026-11-05 FAT32/exFAT 真实设备验证的记录）：
 * 对着真实的 Windows 裸卷句柄（\\.\F: 这种）直接 ReadFile 时，Windows
 * 要求请求的读取长度必须是扇区大小的整数倍——哪怕没有加
 * FILE_FLAG_NO_BUFFERING。请求一个不对齐的长度（比如恢复一个 53 字节的
 * 小文件）会直接返回 0 字节，不报错也不说明原因。这个计算逻辑之前在
 * `engine/shell/fat_reader.c` 和 `engine/shell/exfat_reader.c` 里各自
 * 写了一份几乎一模一样的代码，本身是平台无关的纯算术，却因为被
 * `#ifdef _WIN32` 包裹的真实 I/O 代码包着，在 Mac 本地的单元测试/CI
 * 里从来没有被真正跑到过——这正是本项目这几轮里反复在"第一次真机测试"
 * 才发现 bug 的结构性原因（2026-11-05 第9步文档收尾时识别出的问题）。
 * 这里把这部分算术抽成独立的纯函数，两处调用方改成共用它，并且用
 * TDD 覆盖，以后类似的边界条件改动能在 Mac 上直接跑单测验证，不用
 * 等虚拟机里第一次真实恢复小文件才暴露。
 */
#ifndef ENGINE_CORE_SECTOR_ALIGNED_READ_H
#define ENGINE_CORE_SECTOR_ALIGNED_READ_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 给定这次想读的字节数 requested、设备的扇区大小 sector_size、以及
 * 缓冲区实际容量 buffer_capacity，计算应该真正向底层设备发起的读取
 * 请求长度（向上取整到 sector_size 的整数倍，但不超过 buffer_capacity）。
 *
 * sector_size == 0 时（比如调用方还没能确定扇区大小）视为"不需要对齐"，
 * 直接返回 requested 本身——这对应我们目前唯一这么用的场景
 * （cli/main.c 的分区拷贝函数在文件系统类型确认之前还不知道扇区大小）。
 */
size_t sector_aligned_read_compute_request_size(
        size_t requested,
        size_t sector_size,
        size_t buffer_capacity );

/* 给定调用方真正想要的字节数 requested、以及这次读取实际成功读到的
 * 字节数 actually_read（可能因为向上取整多读了一些，也可能因为到了
 * 文件/设备末尾而比请求的还少），计算这次读取里有多少字节是调用方
 * 真正能用的——取两者较小值，多读到的部分（本来就是紧跟着的后续
 * 字节，不是垃圾数据，只是这次不需要）由调用方自己丢弃。
 */
size_t sector_aligned_read_usable_bytes(
        size_t requested,
        size_t actually_read );

/* B9真机验证（BitLocker，2026-11-05小树裁决）发现的第二层对齐问题：
 * 上面两个函数只处理"读取*长度*"的对齐，裸物理盘设备对象
 * （`\\.\PhysicalDriveN`）连读取*起点*本身也必须是扇区整数倍——这和
 * `\\.\<盘符>:`卷设备对象不同（卷管理器自带缓存垫片，能容忍任意字节
 * 偏移）。给定想要开始读取的真实物理偏移 physical_target，向下取整到
 * 不超过它的最近一个 sector_size 整数倍，作为真正应该发起 seek 的
 * 物理偏移 *out_aligned_physical_target，差值（0 到 sector_size-1 之间）
 * 作为 *out_lead_in——读到的数据里要跳过这么多字节，才是调用方真正
 * 要的内容从哪开始。
 *
 * sector_size == 0 时视为"不需要对齐"：*out_aligned_physical_target
 * 就是 physical_target 本身，*out_lead_in 是 0——和
 * sector_aligned_read_compute_request_size 同样的防御性约定。
 */
void sector_aligned_read_compute_aligned_seek(
      uint64_t physical_target,
      size_t sector_size,
      uint64_t *out_aligned_physical_target,
      size_t *out_lead_in );

/* 和 sector_aligned_read_usable_bytes 类似，但多考虑了"读到的数据前面
 * 垫了lead_in个字节、调用方不要"这件事：actually_read 如果还没覆盖到
 * lead_in之后的内容（比如设备提前耗尽），可用字节数是 0，不是负数；
 * 否则是"跳过lead_in之后剩下的、调用方要的部分"和requested取较小值。
 */
size_t sector_aligned_read_usable_bytes_with_lead_in(
        size_t lead_in,
        size_t requested,
        size_t actually_read );

#ifdef __cplusplus
}
#endif

#endif /* ENGINE_CORE_SECTOR_ALIGNED_READ_H */
