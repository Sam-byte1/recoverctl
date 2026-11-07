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

/*
 * B9真机验证（BitLocker，2026-11-05小树裁决）发现的第二层对齐问题：
 * `\\.\PhysicalDriveN`裸物理盘设备对象连读取*起点*本身也必须扇区对齐，
 * 下面几条直接对应真机撞到的真实数值——偏移35651648（=base_offset
 * 65536+逻辑偏移35586112），mod 512=64，不是扇区整数倍，真机上
 * `ReadFile`报`ERROR_INVALID_PARAMETER`。
 */
static void test_compute_aligned_seek_rounds_down_to_sector_boundary( void )
{
	uint64_t aligned_target = 999;
	size_t lead_in = 999;

	TEST_CASE( "真机撞到的真实场景：偏移35651648、扇区512，应该向下取整到35651584，lead_in=64" );

	sector_aligned_read_compute_aligned_seek( 35651648, 512, &aligned_target, &lead_in );

	TEST_ASSERT( aligned_target == 35651584, "对齐后的物理偏移应该是35651584" );
	TEST_ASSERT_EQ_INT( (int) lead_in, 64, "lead_in应该是64（35651648-35651584）" );
}

static void test_compute_aligned_seek_already_aligned_has_zero_lead_in( void )
{
	uint64_t aligned_target = 999;
	size_t lead_in = 999;

	TEST_CASE( "物理偏移本来就是扇区整数倍时，对齐后不变，lead_in应该是0" );

	sector_aligned_read_compute_aligned_seek( 65536, 512, &aligned_target, &lead_in );

	TEST_ASSERT( aligned_target == 65536, "65536本来就是512的整数倍，应该保持不变" );
	TEST_ASSERT_EQ_INT( (int) lead_in, 0, "lead_in应该是0" );
}

static void test_compute_aligned_seek_worst_case_lead_in( void )
{
	uint64_t aligned_target = 999;
	size_t lead_in = 999;

	TEST_CASE( "最坏情况：偏移比扇区边界多511字节，lead_in应该正好是511" );

	sector_aligned_read_compute_aligned_seek( 1023, 512, &aligned_target, &lead_in );

	TEST_ASSERT( aligned_target == 512, "1023向下取整到512应该是512" );
	TEST_ASSERT_EQ_INT( (int) lead_in, 511, "lead_in应该是511（1023-512）" );
}

static void test_compute_aligned_seek_zero_sector_size_means_no_alignment( void )
{
	uint64_t aligned_target = 999;
	size_t lead_in = 999;

	TEST_CASE( "sector_size为0时应该原样返回physical_target，lead_in是0，不做对齐" );

	sector_aligned_read_compute_aligned_seek( 12345, 0, &aligned_target, &lead_in );

	TEST_ASSERT( aligned_target == 12345, "应该原样返回12345" );
	TEST_ASSERT_EQ_INT( (int) lead_in, 0, "lead_in应该是0" );
}

static void test_usable_bytes_with_lead_in_skips_the_padding( void )
{
	TEST_CASE( "usable字节数应该跳过lead_in那部分之后，再和requested取较小值" );

	/* 请求64字节lead_in + 100字节真正要的内容，膨胀读取512字节全部成功 */
	TEST_ASSERT( sector_aligned_read_usable_bytes_with_lead_in( 64, 100, 512 ) == 100,
	             "lead_in=64、requested=100、actually_read=512时，usable应该是100" );
}

static void test_usable_bytes_with_lead_in_zero_matches_old_behavior( void )
{
	TEST_CASE( "lead_in=0时应该和sector_aligned_read_usable_bytes行为完全一致（回归保障）" );

	TEST_ASSERT( sector_aligned_read_usable_bytes_with_lead_in( 0, 53, 512 ) == 53,
	             "lead_in=0、请求53、实际读到512，usable应该是53" );
	TEST_ASSERT( sector_aligned_read_usable_bytes_with_lead_in( 0, 512, 100 ) == 100,
	             "lead_in=0、请求512、实际只读到100，usable应该是100" );
}

static void test_usable_bytes_with_lead_in_device_exhausted_before_lead_in_covered( void )
{
	TEST_CASE( "设备提前耗尽、实际读到的字节数连lead_in都没覆盖到时，usable应该是0，不是负数" );

	TEST_ASSERT( sector_aligned_read_usable_bytes_with_lead_in( 64, 100, 50 ) == 0,
	             "lead_in=64、actually_read=50（连lead_in都不够），usable应该是0" );
	TEST_ASSERT( sector_aligned_read_usable_bytes_with_lead_in( 64, 100, 64 ) == 0,
	             "actually_read正好等于lead_in（跳过之后一个字节都不剩），usable应该是0" );
}

int main( void )
{
	test_rounds_up_to_sector_boundary();
	test_already_aligned_request_unchanged();
	test_clamps_to_buffer_capacity();
	test_zero_sector_size_means_no_alignment();
	test_usable_bytes_takes_smaller_of_requested_and_actually_read();
	test_compute_aligned_seek_rounds_down_to_sector_boundary();
	test_compute_aligned_seek_already_aligned_has_zero_lead_in();
	test_compute_aligned_seek_worst_case_lead_in();
	test_compute_aligned_seek_zero_sector_size_means_no_alignment();
	test_usable_bytes_with_lead_in_skips_the_padding();
	test_usable_bytes_with_lead_in_zero_matches_old_behavior();
	test_usable_bytes_with_lead_in_device_exhausted_before_lead_in_covered();

	TEST_SUMMARY();
}
