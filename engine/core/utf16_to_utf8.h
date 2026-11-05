/*
 * functional core：把一段 UTF-16LE 编码的字符数据转成 UTF-8。
 *
 * 背景：exFAT 的文件名（File Name 目录项）和 FAT32 的 VFAT 长文件名目录项
 * 都用 UTF-16LE 存储文件名，两边原来都只实现了"每个 UTF-16 码元取低字节"
 * 这种简化处理——这对 ASCII 范围内的字符凑巧是对的（因为 ASCII 字符的
 * UTF-16LE 编码高字节恰好是 0x00），但对中文、日文这类码点超过 0x7F 的
 * 字符会直接截断出一堆垃圾字节，不是"简化"而是真实的 bug（2026-11-05
 * 小树裁决第13步核查时确认 exFAT 这边已经实际存在这个问题）。这里实现
 * 一个正确的转换器，两边共用，不再各自维护一份简化/错误的版本。
 *
 * 支持范围：BMP（基本多语言平面，码点 0x0000-0xFFFF，覆盖中文/日文/韩文/
 * 西里尔文等绝大多数实际会遇到的文字）+ 代理对（surrogate pair，覆盖
 * emoji 等码点超过 0xFFFF 的字符）——完整支持 UTF-16 规范，不是只做
 * BMP 这一半。
 */
#ifndef ENGINE_CORE_UTF16_TO_UTF8_H
#define ENGINE_CORE_UTF16_TO_UTF8_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 把 num_utf16_units 个 UTF-16LE 码元（每个 2 字节，utf16le_bytes 总长度
 * 应该是 num_utf16_units*2 字节）转成 UTF-8，写进 out_utf8（以 \0 结尾）。
 *
 * 遇到孤立的代理项（不成对的 surrogate）或者结果装不下 out_utf8_size 时，
 * 不会越界写入或者崩溃：孤立代理项会被替换成 U+FFFD（Unicode 规范推荐的
 * "替换字符"，不是我们发明的处理方式），结果太长会在能装下的最后一个
 * 完整字符处截断（不会切出半个 UTF-8 字符的残缺字节）。
 *
 * 返回写入 out_utf8 的字节数（不含结尾 \0），-1 表示参数不合法
 * （utf16le_bytes 为 NULL，或者 out_utf8_size 为 0 装不下哪怕一个 \0）。
 */
int utf16le_to_utf8(
     const uint8_t *utf16le_bytes,
     size_t num_utf16_units,
     char *out_utf8,
     size_t out_utf8_size );

#ifdef __cplusplus
}
#endif

#endif /* ENGINE_CORE_UTF16_TO_UTF8_H */
