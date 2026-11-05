/*
 * 和 test_exfat_fat_chain.c 同样的测试结构，换成 FAT32 的 EOF/坏簇/空闲
 * 标记约定（2026-11-05 第21步，"目录单簇假设"修复）：
 * EOF >= 0x0FFFFFF8，坏簇 == 0x0FFFFFF7，空闲 == 0x00000000，只有低 28
 * 位有意义（高 4 位保留，读取时应该被屏蔽掉）。
 */
#include <string.h>

#include "test_framework.h"
#include "../../core/fat_fat_chain.h"

static void put_u32le( uint8_t *buf, uint32_t cluster, uint32_t value )
{
	size_t offset = (size_t) cluster * 4;

	buf[ offset ]     = (uint8_t) ( value & 0xFF );
	buf[ offset + 1 ] = (uint8_t) ( ( value >> 8 ) & 0xFF );
	buf[ offset + 2 ] = (uint8_t) ( ( value >> 16 ) & 0xFF );
	buf[ offset + 3 ] = (uint8_t) ( ( value >> 24 ) & 0xFF );
}

static void test_walks_multi_cluster_chain( void )
{
	uint8_t fat[ 64 ] = { 0 };
	uint32_t clusters[ 8 ];
	uint32_t cluster_count;
	fat_fat_chain_walk_status_t status;

	TEST_CASE( "跟着 FAT32 表走一条簇链：7 -> 9 -> 11 -> EOF" );

	put_u32le( fat, 7, 9 );
	put_u32le( fat, 9, 11 );
	put_u32le( fat, 11, 0x0FFFFFFF );

	TEST_ASSERT( fat_fat_chain_walk( fat, sizeof( fat ), 7, 16, 8, clusters, &cluster_count, &status ) == 1,
	             "调用不应报错" );
	TEST_ASSERT( status == FAT_FAT_CHAIN_WALK_OK, "应该正常走到 EOF 结束" );
	TEST_ASSERT_EQ_INT( (int) cluster_count, 3, "应该正好收集到 3 个簇" );

	if( cluster_count == 3 )
	{
		TEST_ASSERT_EQ_INT( (int) clusters[ 0 ], 7, "第 1 个簇应该是 7" );
		TEST_ASSERT_EQ_INT( (int) clusters[ 1 ], 9, "第 2 个簇应该是 9" );
		TEST_ASSERT_EQ_INT( (int) clusters[ 2 ], 11, "第 3 个簇应该是 11" );
	}
}

static void test_eof_high_bits_are_masked_off( void )
{
	/* FAT32 的 FAT 表项只有低 28 位有意义，高 4 位保留——真实磁盘上这几位
	 * 可能不是 0（规范允许驱动程序拿它们做别的用途），解析时必须屏蔽掉，
	 * 不能直接按完整 32 位去判断 EOF */
	uint8_t fat[ 32 ] = { 0 };
	uint32_t clusters[ 4 ];
	uint32_t cluster_count;
	fat_fat_chain_walk_status_t status;

	TEST_CASE( "EOF 标记高 4 位保留位不是 0 时，仍然应该正确识别为 EOF（必须屏蔽高位再判断）" );

	put_u32le( fat, 5, 0xFFFFFFFF );   /* 高4位全1 + 低28位全1，同样是合法 EOF */

	TEST_ASSERT( fat_fat_chain_walk( fat, sizeof( fat ), 5, 16, 8, clusters, &cluster_count, &status ) == 1,
	             "调用不应报错" );
	TEST_ASSERT( status == FAT_FAT_CHAIN_WALK_OK, "应该正常识别为 EOF" );
	TEST_ASSERT_EQ_INT( (int) cluster_count, 1, "应该只收集到 1 个簇" );
}

static void test_hits_free_marker_mid_chain( void )
{
	uint8_t fat[ 32 ] = { 0 };
	uint32_t clusters[ 4 ];
	uint32_t cluster_count;
	fat_fat_chain_walk_status_t status;

	TEST_CASE( "走到一个已经被标记为空闲（0）的簇，应该识别成链被破坏，不是正常结束" );

	put_u32le( fat, 5, 0x00000000 );

	TEST_ASSERT( fat_fat_chain_walk( fat, sizeof( fat ), 5, 16, 8, clusters, &cluster_count, &status ) == 1,
	             "调用不应报错" );
	TEST_ASSERT( status == FAT_FAT_CHAIN_WALK_HIT_FREE, "应该识别为链被破坏（HIT_FREE）" );
	TEST_ASSERT_EQ_INT( (int) cluster_count, 1, "应该只收集到已经确认存在的第 1 个簇" );
}

static void test_hits_bad_cluster_marker( void )
{
	uint8_t fat[ 32 ] = { 0 };
	uint32_t clusters[ 4 ];
	uint32_t cluster_count;
	fat_fat_chain_walk_status_t status;

	TEST_CASE( "走到标记为坏簇（0x0FFFFFF7）的值，应该识别为 BAD_CLUSTER，不是正常结束" );

	put_u32le( fat, 5, 0x0FFFFFF7 );

	TEST_ASSERT( fat_fat_chain_walk( fat, sizeof( fat ), 5, 16, 8, clusters, &cluster_count, &status ) == 1,
	             "调用不应报错" );
	TEST_ASSERT( status == FAT_FAT_CHAIN_WALK_BAD_CLUSTER, "应该识别为坏簇" );
	TEST_ASSERT_EQ_INT( (int) cluster_count, 1, "应该只收集到已经确认存在的第 1 个簇" );
}

static void test_rejects_out_of_range_cluster_value( void )
{
	uint8_t fat[ 64 ] = { 0 };
	uint32_t clusters[ 4 ];
	uint32_t cluster_count;
	fat_fat_chain_walk_status_t status;

	TEST_CASE( "链指向一个超出卷簇号范围的值，应该识别成数据损坏（OUT_OF_RANGE）" );

	put_u32le( fat, 5, 99999 );  /* 远超 total_data_clusters=16 的合法范围 */

	TEST_ASSERT( fat_fat_chain_walk( fat, sizeof( fat ), 5, 16, 8, clusters, &cluster_count, &status ) == 1,
	             "调用不应报错" );
	TEST_ASSERT( status == FAT_FAT_CHAIN_WALK_OUT_OF_RANGE, "应该识别为超出范围" );
}

static void test_stops_at_max_clusters_to_defend_against_cycles( void )
{
	uint8_t fat[ 32 ] = { 0 };
	uint32_t clusters[ 3 ];
	uint32_t cluster_count;
	fat_fat_chain_walk_status_t status;

	TEST_CASE( "环形引用应该在达到 max_clusters 兜底后停止，不会死循环" );

	put_u32le( fat, 5, 6 );
	put_u32le( fat, 6, 5 );

	TEST_ASSERT( fat_fat_chain_walk( fat, sizeof( fat ), 5, 16, 3, clusters, &cluster_count, &status ) == 1,
	             "调用不应报错" );
	TEST_ASSERT( status == FAT_FAT_CHAIN_WALK_TOO_LONG, "应该识别为超过最大长度兜底" );
	TEST_ASSERT_EQ_INT( (int) cluster_count, 3, "应该正好收集到 max_clusters 指定的 3 个" );
}

static void test_rejects_invalid_arguments( void )
{
	uint8_t fat[ 32 ] = { 0 };
	uint32_t clusters[ 4 ];
	uint32_t cluster_count;
	fat_fat_chain_walk_status_t status;

	TEST_CASE( "参数不合法时应该返回 -1，而不是崩溃" );

	TEST_ASSERT( fat_fat_chain_walk( NULL, sizeof( fat ), 5, 16, 8, clusters, &cluster_count, &status ) == -1,
	             "fat_table_data 为 NULL 应该返回 -1" );
	TEST_ASSERT( fat_fat_chain_walk( fat, sizeof( fat ), 1, 16, 8, clusters, &cluster_count, &status ) == -1,
	             "start_cluster < 2 应该返回 -1（0、1 是保留簇号）" );
	TEST_ASSERT( fat_fat_chain_walk( fat, sizeof( fat ), 5, 16, 0, clusters, &cluster_count, &status ) == -1,
	             "max_clusters 为 0 应该返回 -1" );
}

int main( void )
{
	test_walks_multi_cluster_chain();
	test_eof_high_bits_are_masked_off();
	test_hits_free_marker_mid_chain();
	test_hits_bad_cluster_marker();
	test_rejects_out_of_range_cluster_value();
	test_stops_at_max_clusters_to_defend_against_cycles();
	test_rejects_invalid_arguments();

	TEST_SUMMARY();
}
