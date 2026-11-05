#include <string.h>

#include "test_framework.h"
#include "../../core/bad_sector_log.h"

static void test_append_grows_and_tracks_records( void )
{
	bad_sector_log_t log;

	TEST_CASE( "追加多条记录应该都能正确保留，count 正确递增" );

	memset( &log, 0, sizeof( log ) );

	TEST_ASSERT( bad_sector_log_append( &log, 0, 512 ) == 1, "第一次追加不应报错" );
	TEST_ASSERT( bad_sector_log_append( &log, 4096, 1024 ) == 1, "第二次追加不应报错" );
	TEST_ASSERT_EQ_INT( log.count, 2, "应该有 2 条记录" );

	if( log.count == 2 )
	{
		TEST_ASSERT_EQ_INT( (int) log.records[ 0 ].offset, 0, "第一条偏移应该是 0" );
		TEST_ASSERT_EQ_INT( (int) log.records[ 0 ].length, 512, "第一条长度应该是 512" );
		TEST_ASSERT_EQ_INT( (int) log.records[ 1 ].offset, 4096, "第二条偏移应该是 4096" );
		TEST_ASSERT_EQ_INT( (int) log.records[ 1 ].length, 1024, "第二条长度应该是 1024" );
	}

	bad_sector_log_free( &log );
}

static void test_grows_beyond_initial_capacity( void )
{
	bad_sector_log_t log;
	int i;

	TEST_CASE( "追加超过初始容量（8条）的记录，应该正确扩容而不丢数据" );

	memset( &log, 0, sizeof( log ) );

	for( i = 0; i < 20; i++ )
	{
		TEST_ASSERT( bad_sector_log_append( &log, (uint64_t) i * 512, 512 ) == 1, "追加不应报错" );
	}
	TEST_ASSERT_EQ_INT( log.count, 20, "应该有 20 条记录" );

	bad_sector_log_free( &log );
}

static void test_total_bytes_skipped_sums_all_records( void )
{
	bad_sector_log_t log;
	uint64_t total;

	TEST_CASE( "跳过总字节数应该是所有记录长度之和" );

	memset( &log, 0, sizeof( log ) );
	bad_sector_log_append( &log, 0, 512 );
	bad_sector_log_append( &log, 1000, 2048 );

	total = bad_sector_log_total_bytes_skipped( &log );
	TEST_ASSERT_EQ_INT( (int) total, 2560, "总字节数应该是 512+2048=2560" );

	bad_sector_log_free( &log );
}

static void test_total_bytes_skipped_on_empty_log_is_zero( void )
{
	bad_sector_log_t log;

	TEST_CASE( "从未追加过记录的日志，总跳过字节数应该是 0" );

	memset( &log, 0, sizeof( log ) );
	TEST_ASSERT_EQ_INT( (int) bad_sector_log_total_bytes_skipped( &log ), 0, "空日志总字节数应该是 0" );
}

static void test_free_is_safe_on_empty_log( void )
{
	bad_sector_log_t log;

	TEST_CASE( "bad_sector_log_free 对一个从未填充过的 log 调用应该是安全的" );

	memset( &log, 0, sizeof( log ) );
	bad_sector_log_free( &log );

	TEST_ASSERT_EQ_INT( log.count, 0, "释放后 count 应该是 0" );
	TEST_ASSERT( log.records == NULL, "释放后 records 应该是 NULL" );
}

static void test_append_rejects_null_log( void )
{
	TEST_CASE( "log 为 NULL 时应该返回 -1，而不是崩溃" );

	TEST_ASSERT( bad_sector_log_append( NULL, 0, 512 ) == -1, "log 为 NULL 应该返回 -1" );
}

int main( void )
{
	test_append_grows_and_tracks_records();
	test_grows_beyond_initial_capacity();
	test_total_bytes_skipped_sums_all_records();
	test_total_bytes_skipped_on_empty_log_is_zero();
	test_free_is_safe_on_empty_log();
	test_append_rejects_null_log();

	TEST_SUMMARY();
}
