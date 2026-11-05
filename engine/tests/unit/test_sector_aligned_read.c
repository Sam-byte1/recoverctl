/*
 * 这几条断言直接对应 2026-11-05 在真实 FAT32/exFAT 设备上踩过的坑：
 * 恢复一个 53 字节的小文件时，对真实卷句柄用 ReadFile 请求 53 字节
 * 会直接返回 0（因为不是扇区大小的整数倍）。这里把当时手工排查出来的
 * 具体数值原样搬进单元测试，以后改动这段逻辑能在 Mac 上直接验证，
 * 不用等虚拟机里第一次真实恢复小文件才发现问题。
 */
#include "test_framework.h"
#include "../../core/sector_aligned_read.h"

static void test_rounds_up_to_sector_boundary( void )
{
	TEST_CASE( "请求长度不是扇区整数倍时，应该向上取整到下一个扇区边界" );

	/* 真实撞过的场景：FAT32 卷扇区大小 512，恢复一个 53 字节的文件 */
	TEST_ASSERT( sector_aligned_read_compute_request_size( 53, 512, 4096 ) == 512,
	             "53 字节、512 扇区，应该取整到 512" );

	/* exFAT 那次撞到的场景：请求 35 字节 */
	TEST_ASSERT( sector_aligned_read_compute_request_size( 35, 512, 4096 ) == 512,
	             "35 字节、512 扇区，应该取整到 512" );

	TEST_ASSERT( sector_aligned_read_compute_request_size( 513, 512, 4096 ) == 1024,
	             "513 字节（刚好比一个扇区多 1 字节）应该取整到 1024（两个扇区）" );
}

static void test_already_aligned_request_unchanged( void )
{
	TEST_CASE( "请求长度本来就是扇区整数倍时，不应该多取整" );

	TEST_ASSERT( sector_aligned_read_compute_request_size( 512, 512, 4096 ) == 512,
	             "512 字节、512 扇区，应该保持 512 不变" );
	TEST_ASSERT( sector_aligned_read_compute_request_size( 1024, 512, 4096 ) == 1024,
	             "1024 字节、512 扇区，应该保持 1024 不变" );
}

static void test_clamps_to_buffer_capacity( void )
{
	TEST_CASE( "取整后的结果不应该超过缓冲区实际容量" );

	TEST_ASSERT( sector_aligned_read_compute_request_size( 4000, 512, 4096 ) == 4096,
	             "4000 字节取整到 4096 本来就等于缓冲区容量，应该是 4096" );
	TEST_ASSERT( sector_aligned_read_compute_request_size( 4090, 4096, 4096 ) == 4096,
	             "4090 字节、4096 扇区，取整后是 4096（刚好等于容量），应该是 4096" );
}

static void test_zero_sector_size_means_no_alignment( void )
{
	TEST_CASE( "sector_size 为 0（还不知道扇区大小）时，应该原样返回 requested，不做对齐" );

	TEST_ASSERT( sector_aligned_read_compute_request_size( 53, 0, 4096 ) == 53,
	             "sector_size=0 时应该原样返回 53" );
}

static void test_usable_bytes_takes_smaller_of_requested_and_actually_read( void )
{
	TEST_CASE( "usable 字节数应该是 requested 和 actually_read 两者中较小的那个" );

	/* 正常情况：向上取整多读了一些，只有 requested 那部分是真正要的 */
	TEST_ASSERT( sector_aligned_read_usable_bytes( 53, 512 ) == 53,
	             "请求 53、实际读到 512（对齐后的整扇区），usable 应该是 53" );

	/* 到了文件/设备末尾，实际读到的比请求的还少 */
	TEST_ASSERT( sector_aligned_read_usable_bytes( 512, 100 ) == 100,
	             "请求 512、实际只读到 100（末尾），usable 应该是 100" );

	/* 恰好相等 */
	TEST_ASSERT( sector_aligned_read_usable_bytes( 512, 512 ) == 512,
	             "请求和实际读到的相等时，usable 应该等于两者" );
}

int main( void )
{
	test_rounds_up_to_sector_boundary();
	test_already_aligned_request_unchanged();
	test_clamps_to_buffer_capacity();
	test_zero_sector_size_means_no_alignment();
	test_usable_bytes_takes_smaller_of_requested_and_actually_read();

	TEST_SUMMARY();
}
