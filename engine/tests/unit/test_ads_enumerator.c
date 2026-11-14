/*
 * J1 的 TDD 测试：对 ads_enumerator_list_streams 这个纯函数做断言。
 * 测试数据来自 engine/tests/fixtures/ntfs-ads-undelete/（见该目录 README.md）。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libfsntfs.h>

#include "test_framework.h"
#include "../../core/ads_enumerator.h"
#include "../../core/undelete_detector.h"

static libfsntfs_volume_t *open_fixture( const char *path )
{
	libfsntfs_volume_t *volume = NULL;
	libfsntfs_error_t *error   = NULL;

	if( libfsntfs_volume_initialize( &volume, &error ) != 1 )
	{
		return NULL;
	}
	if( libfsntfs_volume_open( volume, path, LIBFSNTFS_OPEN_READ, &error ) != 1 )
	{
		libfsntfs_error_free( &error );
		libfsntfs_volume_free( &volume, NULL );
		return NULL;
	}
	return volume;
}

/* entry 38 = 被模拟删除的 ads_test.txt，带 1 个具名备用数据流 "secret" */
static void test_deleted_file_with_ads_lists_the_named_stream( libfsntfs_volume_t *volume )
{
	libfsntfs_file_entry_t *file_entry = NULL;
	libfsntfs_error_t *error           = NULL;
	undelete_candidate_t candidate;
	ads_stream_list_t streams;

	memset( &streams, 0, sizeof( streams ) );

	TEST_CASE( "entry 38（被模拟删除、带具名ADS secret的ads_test.txt）应该能枚举出这个ADS" );

	TEST_ASSERT( libfsntfs_volume_get_file_entry_by_index( volume, 38, &file_entry, &error ) == 1,
	             "应该能拿到 entry 38" );

	/* 先确认这确实是本项目已有判定逻辑认定的"已删除可恢复"候选项——
	 * J1 要验证的核心前提就是"候选项的 file_entry 上也能枚举出ADS"，
	 * 不是随便拿一个 file_entry 测，要用真正符合调用方实际使用场景
	 * 的候选项对象。 */
	TEST_ASSERT( undelete_detector_inspect( file_entry, 38, &candidate, &error ) == 1,
	             "inspect 不应报错" );
	TEST_ASSERT_EQ_INT( candidate.is_deleted_recoverable, 1,
	                     "entry 38 应判定为已删除且可恢复（make_fixture.py 翻转了in-use位）" );

	TEST_ASSERT( ads_enumerator_list_streams( file_entry, &streams, &error ) == 1,
	             "枚举ADS不应报错" );
	TEST_ASSERT_EQ_INT( streams.count, 1,
	                     "这个已删除的文件应该有且仅有1个具名备用数据流" );

	if( streams.count == 1 )
	{
		TEST_ASSERT_EQ_STR( streams.streams[ 0 ].name, "secret",
		                     "ADS的名字应该是secret" );
		TEST_ASSERT_EQ_INT( streams.streams[ 0 ].has_size, 1,
		                     "应该能成功读到ADS的大小" );
		TEST_ASSERT_EQ_INT( (int) streams.streams[ 0 ].size, 29,
		                     "ads_test.txt.secret.original 实际字节数是29（wc -c核实），应该原样读出" );
	}

	ads_stream_list_free( &streams );
	libfsntfs_file_entry_free( &file_entry, &error );
}

/* entry 5 = NTFS 根目录，固定存在、没有任何具名备用数据流——确认
 * "没有ADS"这个绝大多数文件的正常情况，枚举结果应该是count==0而不是
 * 报错，不能把"没有ADS"和"枚举失败"混为一谈。 */
static void test_entry_without_ads_has_zero_count( libfsntfs_volume_t *volume )
{
	libfsntfs_file_entry_t *file_entry = NULL;
	libfsntfs_error_t *error           = NULL;
	ads_stream_list_t streams;

	memset( &streams, 0, sizeof( streams ) );

	TEST_CASE( "entry 5（NTFS根目录，没有ADS）枚举结果应该是count=0，不是报错" );

	TEST_ASSERT( libfsntfs_volume_get_file_entry_by_index( volume, 5, &file_entry, &error ) == 1,
	             "应该能拿到 entry 5" );
	TEST_ASSERT( ads_enumerator_list_streams( file_entry, &streams, &error ) == 1,
	             "没有ADS不应该导致枚举函数报错" );
	TEST_ASSERT_EQ_INT( streams.count, 0,
	                     "根目录没有具名备用数据流" );

	ads_stream_list_free( &streams );
	libfsntfs_file_entry_free( &file_entry, &error );
}

/* NULL file_entry 应该被安全拒绝，不能崩溃 */
static void test_null_file_entry_is_rejected( void )
{
	libfsntfs_error_t *error = NULL;
	ads_stream_list_t streams;

	memset( &streams, 0, sizeof( streams ) );

	TEST_CASE( "file_entry为NULL时应该安全返回失败，不崩溃" );

	TEST_ASSERT( ads_enumerator_list_streams( NULL, &streams, &error ) == -1,
	             "NULL file_entry应该返回-1" );
	TEST_ASSERT_EQ_INT( streams.count, 0,
	                     "失败时out应该保持清零状态" );
}

int main( int argc, char *argv[] )
{
	const char *fixture_path = argc > 1
	                             ? argv[ 1 ]
	                             : "../../tests/fixtures/ntfs-ads-undelete/volume.img";

	libfsntfs_volume_t *volume = open_fixture( fixture_path );

	if( volume == NULL )
	{
		fprintf( stderr, "无法打开测试镜像: %s\n", fixture_path );
		return 1;
	}

	test_deleted_file_with_ads_lists_the_named_stream( volume );
	test_entry_without_ads_has_zero_count( volume );
	test_null_file_entry_is_rejected();

	{
		libfsntfs_error_t *error = NULL;
		libfsntfs_volume_close( volume, &error );
		libfsntfs_volume_free( &volume, &error );
	}

	TEST_SUMMARY();
}
