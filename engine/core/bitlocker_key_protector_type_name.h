/*
 * functional core（B1，2026-11-05 任务拆解排期）：把 libbde 的
 * LIBBDE_KEY_PROTECTION_TYPE_* 数值枚举翻译成人话，供CLI展示。
 *
 * 明确标注哪些类型"能用、现在支持/排期中"，哪些类型"libbde本身就不
 * 支持解锁"（TPM/TPM_AND_PIN——这是libyal libbde这个第三方库自身的
 * 能力边界，不是我们没做，第26步调研已经确认记录过），避免用户看到
 * "检测到一个保护器"就误以为这个工具一定能用它解锁。
 */
#ifndef ENGINE_CORE_BITLOCKER_KEY_PROTECTOR_TYPE_NAME_H
#define ENGINE_CORE_BITLOCKER_KEY_PROTECTOR_TYPE_NAME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 返回一个静态字符串（不需要调用方释放），对未知的 type 数值返回一个
 * 包含该数值本身的占位字符串，而不是空字符串/NULL——保证调用方总能
 * 展示点什么，不会因为libbde以后新增类型就显示空白。 */
const char *bitlocker_key_protector_type_name(
             uint16_t type );

#ifdef __cplusplus
}
#endif

#endif /* ENGINE_CORE_BITLOCKER_KEY_PROTECTOR_TYPE_NAME_H */
