/*
 * imperative shell（B3，2026-11-05 任务拆解排期）：跨平台的"敏感内存
 * 主动清零"原语——世杰拍板"内存敏感数据现在就要主动清零"，这个模块是
 * 整条BitLocker凭据处理链路里唯一需要调用它的基础设施。
 *
 * 为什么不能直接用 `memset(ptr, 0, len)`：编译器的死代码消除可能会把
 * "写了之后再也没人读"的内存写操作整个优化掉——如果一段敏感数据写完
 * 清零之后，变量马上就要被释放/离开作用域，编译器完全有理由认为这次
 * `memset` 调用"没有可观察的效果"，直接删掉。这不是理论上的担心，是
 * C标准允许编译器做的合法优化，很多安全库（OpenSSL的
 * `OPENSSL_cleanse`、libsodium的`sodium_memzero`）都专门绕开过这个坑。
 *
 * 选型（按平台）：
 * - Windows: `SecureZeroMemory()`——Windows API自己保证不会被优化掉，
 *   MinGW交叉编译环境下`<windows.h>`里也能拿到这个声明。
 * - macOS: `memset_s()`——C11 Annex K，Apple libc提供，苹果官方推荐的
 *   "安全清零"写法；没有选Linux/BSD系的`explicit_bzero`，是因为macOS
 *   的libc本身不提供这个函数，不是我们偏好哪个，是平台现实决定的。
 * - 两者都不可用时的兜底：`volatile unsigned char *`逐字节写循环——
 *   C标准保证编译器不能把对`volatile`限定指针的写操作当成无意义代码
 *   优化掉，这是业界公认的最后防线写法。
 */
#ifndef ENGINE_SHELL_SECURE_MEMORY_H
#define ENGINE_SHELL_SECURE_MEMORY_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 把 ptr 指向的 len 字节全部清零，保证不会被编译器优化掉。
 * ptr 为 NULL 或 len 为 0 时安全地什么都不做（不是错误，调用方不需要
 * 每次都先判断是否为空）。 */
void secure_memory_zero(
      void *ptr,
      size_t len );

#ifdef __cplusplus
}
#endif

#endif /* ENGINE_SHELL_SECURE_MEMORY_H */
