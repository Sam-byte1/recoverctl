/*
 * functional core：扫描类函数共用的进度回调类型（G2，GUI开发排期，
 * 2026-11-05）。目的是让 engine/shell 现有的几个 scan 函数能在扫描
 * 过程中周期性地汇报"扫到哪了"，供 GUI 侧（Qt的 QThread + 信号槽）
 * 用来画进度条——CLI 场景不需要进度条，传 NULL 就是零行为变化。
 *
 * 只有一个共用类型，不是每个扫描函数各自发明一个回调签名：NTFS全表
 * 扫描（undelete_scanner_scan_volume）知道total（MFT条目总数，扫描
 * 前就能查到），FAT32/exFAT目录树遍历不知道total（递归遍历子目录，
 * 不预先知道会访问多少个目录项）——total传0表示"总数未知，current
 * 只是一个单调递增的计数器"，调用方（GUI）据此决定画百分比进度条还是
 * 画一个"已处理N项"的计数器，这个判断不应该由被调用的扫描函数自己
 * 猜测或强行编造一个total出来。
 *
 * current/total的单位由具体扫描函数决定（NTFS是"MFT索引"，FAT/exFAT
 * 可能是"已访问的目录项数"），这个头文件不规定单位本身，只规定"调用
 * 频率里程碑越来越近是什么意思"这件事——current应该单调递增，total
 * （非0时）应该是current的上界。
 */
#ifndef ENGINE_CORE_SCAN_PROGRESS_CALLBACK_H
#define ENGINE_CORE_SCAN_PROGRESS_CALLBACK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* current：已经处理到第几项（从0开始计数）。
 * total：总共有多少项，0表示未知/不适用（调用方不应该把0当成"扫描了
 * 0项"误解——那种情况下current本身才是有意义的进度信号）。
 * user_data：调用方在注册回调时提供的任意上下文指针，原样透传，扫描
 * 函数本身不解释这个指针指向什么。
 */
typedef void (*scan_progress_callback_t)(
     uint64_t current,
     uint64_t total,
     void *user_data );

#ifdef __cplusplus
}
#endif

#endif /* ENGINE_CORE_SCAN_PROGRESS_CALLBACK_H */
