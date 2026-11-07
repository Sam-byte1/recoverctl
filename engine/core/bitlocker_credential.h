/*
 * functional core（B4，2026-11-05 任务拆解排期）：BitLocker 敏感凭据的
 * 定长缓冲区类型 + 纯函数处理逻辑（格式规范化、十六进制解码）。
 *
 * 为什么用定长结构体，不用 malloc/realloc：`realloc` 扩容时可能把旧
 * 内存原样拷到新地址，旧地址那份明文副本如果不重新清零就会留在堆上，
 * 这是处理敏感字符串的经典坑。固定缓冲区从源头避免这个问题，不需要
 * 在"要不要清零旧副本"这件事上操心。512字节对密码/恢复密钥/FVEK+TWEAK
 * 十六进制文本这几种场景都绰绰有余。
 *
 * 这个文件本身不调用任何清零/I/O相关的平台API（那是shell层
 * engine/shell/secure_memory.c 和各个凭据来源模块的职责）——这里只是
 * 纯粹的数据结构定义和字符串/字节转换逻辑，不依赖任何外部状态，可以
 * 完整做 TDD 覆盖。
 */
#ifndef ENGINE_CORE_BITLOCKER_CREDENTIAL_H
#define ENGINE_CORE_BITLOCKER_CREDENTIAL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BITLOCKER_CREDENTIAL_MAX_LENGTH 512

typedef struct
{
	uint8_t data[ BITLOCKER_CREDENTIAL_MAX_LENGTH ];
	size_t length;
}
secure_credential_t;

/* 规范化恢复密钥输入：先去掉用户输入里所有非数字字符（空格、换行、
 * 用户自己敲的`-`等任意非 [0-9] 字符），再按每6位数字重新插入一个
 * `-`分隔符，拼成libbde真正要的格式。
 *
 * B9真机验证发现过一次真实的设计错误：这里最早的实现以为libbde要的
 * 是纯数字（去掉所有分隔符），实际上libbde的
 * `libbde_utf8_recovery_password_calculate_hash`内部是按`-`切成8段、
 * 每段6位数字分别做%11校验（见third_party/lgpl-dynamic/libbde/libbde/
 * libbde_recovery.c），传纯数字进去会被切成1段，直接判定格式不对，
 * 返回"unable to set recovery password hash"——这个bug在Mac单元测试里
 * 测不出来（没有真实libbde调用），只有真机对接libbde才会暴露，所以
 * 这正是B9"真机验证"这一步存在的意义。
 *
 * 不做48位长度/逐段%11校验和检查——libbde 自己的
 * `set_utf8_recovery_password` 会做这些格式校验，这里只负责"把人类
 * 习惯的任意分隔符/空白输入转成库要的`XXXXXX-XXXXXX-...`格式"，重复
 * 校验没有意义。
 *
 * 返回 1 成功，-1 失败（参数不合法、out_size 装不下结果）。
 */
int bitlocker_credential_normalize_recovery_key(
     const char *input,
     char *out,
     size_t out_size );

/* 把一段十六进制文本（FVEK/TWEAK 密钥的人类可读表示形式）解码成原始
 * 字节，写进 out（调用方保证 out_capacity 足够大，函数会在装不下时
 * 返回失败而不是截断）。不区分大小写。空字符串是合法输入，解码成
 * 0字节（对应"没有TWEAK密钥"的AES-CBC场景）。
 *
 * 返回 1 成功（*out_length 是实际解码出的字节数），-1 失败（长度为
 * 奇数、出现非十六进制字符、out_capacity 不够）。
 */
int bitlocker_credential_hex_decode(
     const char *hex_string,
     uint8_t *out,
     size_t out_capacity,
     size_t *out_length );

#ifdef __cplusplus
}
#endif

#endif /* ENGINE_CORE_BITLOCKER_CREDENTIAL_H */
