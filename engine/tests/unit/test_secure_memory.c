/*
 * B3（BitLocker，2026-11-05 任务拆解排期）：secure_memory_zero 的正确性
 * 覆盖。
 *
 * 范围说明（刻意划的边界，不是遗漏）：这里只能验证"调用之后内存内容
 * 确实变成了全0"这个功能正确性，验证不了"不会被编译器优化掉"这个
 * 核心诉求本身——因为这条测试自己在调用之后立刻读取了这段内存去做
 * 断言，这个"写后立刻读"的使用模式恰好是编译器无法判定为死代码的
 * 场景，所以哪怕用普通的 `memset` 也一样能通过这条测试。真正要防的
 * 是"写了之后变量马上被释放、没人再读"这种场景，这种场景在单元测试
 * 里天然无法构造出"编译器会不会优化掉"的可观察差异，只能靠选用平台
 * 保证不优化的原语（SecureZeroMemory/memset_s/volatile循环）来保证，
 * 不能靠测试覆盖率来证明。
 */
#include <string.h>

#include "test_framework.h"
#include "../../shell/secure_memory.h"

static void test_zeros_a_normal_buffer( void )
{
	unsigned char buf[ 32 ];
	int i;
	int all_zero = 1;

	TEST_CASE( "调用之后，缓冲区的每一个字节都应该变成0" );

	memset( buf, 0xAB, sizeof( buf ) );
	secure_memory_zero( buf, sizeof( buf ) );

	for( i = 0; i < 32; i++ )
	{
		if( buf[ i ] != 0 )
		{
			all_zero = 0;
			break;
		}
	}
	TEST_ASSERT( all_zero, "全部32字节都应该是0" );
}

static void test_does_not_overrun( void )
{
	unsigned char buf[ 16 ];
	unsigned char canary = 0x5A;

	TEST_CASE( "只清零指定长度，不应该多清一个字节" );

	memset( buf, 0xFF, sizeof( buf ) );
	secure_memory_zero( buf, 8 );

	TEST_ASSERT( buf[ 7 ] == 0, "第8个字节（索引7）应该被清零" );
	TEST_ASSERT( buf[ 8 ] == 0xFF, "第9个字节（索引8）不应该被动" );
	TEST_ASSERT( canary == 0x5A, "不应该影响缓冲区之外的内存" );
}

static void test_null_pointer_is_safe( void )
{
	TEST_CASE( "ptr为NULL时应该安全地什么都不做，不应该崩溃" );

	secure_memory_zero( NULL, 16 );

	TEST_ASSERT( 1, "没有崩溃就算通过" );
}

static void test_zero_length_is_safe( void )
{
	unsigned char buf[ 4 ] = { 1, 2, 3, 4 };

	TEST_CASE( "len为0时应该安全地什么都不做，不应该动缓冲区内容" );

	secure_memory_zero( buf, 0 );

	TEST_ASSERT( ( buf[ 0 ] == 1 ) && ( buf[ 3 ] == 4 ), "缓冲区内容不应该被改动" );
}

int main( void )
{
	test_zeros_a_normal_buffer();
	test_does_not_overrun();
	test_null_pointer_is_safe();
	test_zero_length_is_safe();

	TEST_SUMMARY();
}
