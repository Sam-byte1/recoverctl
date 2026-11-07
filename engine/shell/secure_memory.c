#include "secure_memory.h"

#ifdef _WIN32

#include <windows.h>

void secure_memory_zero(
      void *ptr,
      size_t len )
{
	if( ( ptr == NULL )
	 || ( len == 0 ) )
	{
		return;
	}
	SecureZeroMemory( ptr, len );
}

#elif defined( __APPLE__ )

/* 必须在 #include <string.h> 之前定义这个宏，macOS libc 才会声明
 * memset_s（C11 Annex K 的可选扩展，默认不暴露）。 */
#define __STDC_WANT_LIB_EXT1__ 1
#include <string.h>

void secure_memory_zero(
      void *ptr,
      size_t len )
{
	if( ( ptr == NULL )
	 || ( len == 0 ) )
	{
		return;
	}
	memset_s( ptr, len, 0, len );
}

#else

/* 兜底路径：既不是Windows也不是macOS时使用。volatile 限定指针保证
 * 编译器不能把这个写循环当成无意义代码优化掉（C标准对volatile访问的
 * 保证），这是在没有平台专用"安全清零"原语时业界公认的最后防线写法
 * （libsodium的sodium_memzero在找不到平台专用原语时也是这个兜底）。 */
void secure_memory_zero(
      void *ptr,
      size_t len )
{
	volatile unsigned char *volatile_ptr = (volatile unsigned char *) ptr;
	size_t i;

	if( ( ptr == NULL )
	 || ( len == 0 ) )
	{
		return;
	}
	for( i = 0; i < len; i++ )
	{
		volatile_ptr[ i ] = 0;
	}
}

#endif
