/*
 * Task #4 的 TDD 测试：对 undelete_scanner_scan_volume 做断言。
 * 用的是和 Task #3 相同的 fixture：
 * engine/tests/fixtures/ntfs-basic-undelete/volume.img，
 * 里面确定只有 2 个"已删除但可恢复"的文件：passwords.txt 和 another_file。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libfsntfs.h>

#include "test_framework.h"
#include "../../core/undelete_scanner.h"

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

static const undelete_candidate_t *find_by_name(
       const undelete_scan_result_t *result,
       const char *name )
{
	int i;
	for( i = 0; i < result->count; i++ )
	{
		if( strcmp( result->candidates[ i ].name, name ) == 0 )
		{
			return &( result->candidates[ i ] );
		}
	}
	return NULL;
}

static void test_scan_finds_exactly_the_two_deleted_files( libfsntfs_volume_t *volume )
{
	undelete_scan_result_t result;
	libfsntfs_error_t *error = NULL;
	const undelete_candidate_t *passwords = NULL;
	const undelete_candidate_t *another   = NULL;

	TEST_CASE( "全表扫描应该正好找到 2 个已删除且可恢复的文件，不多不少" );

	memset( &result, 0, sizeof( result ) );

	TEST_ASSERT( undelete_scanner_scan_volume( volume, &result, NULL, NULL, &error ) == 1,
	             "扫描不应报错" );
	TEST_ASSERT_EQ_INT( result.count, 2,
	                     "应该正好找到 2 个候选项（passwords.txt、another_file）" );

	passwords = find_by_name( &result, "passwords.txt" );
	another   = find_by_name( &result, "another_file" );

	TEST_ASSERT( passwords != NULL, "应该找到 passwords.txt" );
	TEST_ASSERT( another != NULL, "应该找到 another_file" );

	if( passwords != NULL )
	{
		TEST_ASSERT_EQ_INT( (int) passwords->mft_entry_index, 66,
		                     "passwords.txt 的 MFT 索引应该是 66" );
	}
	if( another != NULL )
	{
		TEST_ASSERT_EQ_INT( (int) another->mft_entry_index, 67,
		                     "another_file 的 MFT 索引应该是 67" );
	}

	undelete_scan_result_free( &result );
}

/*
 * G2（GUI开发排期，2026-11-05）：进度回调的TDD覆盖。用真实fixture卷
 * （不是mock）跑一次完整扫描，验证回调被正确、完整地调用——这是"functional
 * core"本身的契约，不依赖真实GUI/QThread环境也能完整验证。
 */
typedef struct
{
	int call_count;
	uint64_t first_current;
	uint64_t last_current;
	uint64_t observed_total;
	int total_ever_changed;
	int saw_decrease;
}
progress_probe_t;

static void progress_probe_callback(
     uint64_t current,
     uint64_t total,
     void *user_data )
{
	progress_probe_t *probe = (progress_probe_t *) user_data;

	if( probe->call_count == 0 )
	{
		probe->first_current  = current;
		probe->observed_total = total;
	}
	else
	{
		if( total != probe->observed_total )
		{
			probe->total_ever_changed = 1;
		}
		if( current < probe->last_current )
		{
			probe->saw_decrease = 1;
		}
	}
	probe->last_current = current;
	probe->call_count++;
}

static void test_scan_calls_progress_callback_with_monotonic_current_and_stable_total( libfsntfs_volume_t *volume )
{
	undelete_scan_result_t result;
	libfsntfs_error_t *error = NULL;
	progress_probe_t probe;

	TEST_CASE( "扫描过程中应该按MFT索引顺序持续回调进度，current单调不减、total全程不变" );

	memset( &result, 0, sizeof( result ) );
	memset( &probe, 0, sizeof( probe ) );

	TEST_ASSERT( undelete_scanner_scan_volume( volume, &result, progress_probe_callback, &probe, &error ) == 1,
	             "扫描不应报错" );
	TEST_ASSERT( probe.call_count > 0, "应该至少回调一次（卷上总有MFT条目）" );
	TEST_ASSERT_EQ_INT( (int) probe.first_current, 0, "第一次回调的current应该是0（从第一个MFT索引开始）" );
	TEST_ASSERT( probe.observed_total > 0, "total应该是一个已知的正数（扫描前就能查到MFT条目总数）" );
	TEST_ASSERT( probe.total_ever_changed == 0, "total应该全程保持不变（同一次扫描里总条目数不会变）" );
	TEST_ASSERT( probe.saw_decrease == 0, "current应该单调不减，不应该出现回退" );
	TEST_ASSERT_EQ_INT( (int) probe.call_count, (int) probe.observed_total,
	                     "回调次数应该正好等于total（每个MFT索引回调一次，不多不少）" );

	undelete_scan_result_free( &result );
}

static void test_scan_with_null_callback_behaves_exactly_as_before( libfsntfs_volume_t *volume )
{
	undelete_scan_result_t result;
	libfsntfs_error_t *error = NULL;

	TEST_CASE( "progress_callback传NULL应该是安全的，行为和加这个参数之前完全一样（不崩溃、扫描结果不变）" );

	memset( &result, 0, sizeof( result ) );

	TEST_ASSERT( undelete_scanner_scan_volume( volume, &result, NULL, NULL, &error ) == 1,
	             "NULL回调不应该导致扫描失败" );
	TEST_ASSERT_EQ_INT( result.count, 2, "NULL回调不应该影响扫描结果本身" );

	undelete_scan_result_free( &result );
}

static void test_scan_result_free_is_safe_on_empty_result( void )
{
	undelete_scan_result_t result;

	TEST_CASE( "undelete_scan_result_free 对一个从未填充过的 result 调用应该是安全的" );

	memset( &result, 0, sizeof( result ) );
	undelete_scan_result_free( &result );

	TEST_ASSERT_EQ_INT( result.count, 0, "释放后 count 应该是 0" );
	TEST_ASSERT( result.candidates == NULL, "释放后 candidates 应该是 NULL" );
}

int main( int argc, char *argv[] )
{
	const char *fixture_path = argc > 1
	                             ? argv[ 1 ]
	                             : "../../tests/fixtures/ntfs-basic-undelete/volume.img";

	libfsntfs_volume_t *volume = open_fixture( fixture_path );

	if( volume == NULL )
	{
		fprintf( stderr, "无法打开测试镜像: %s\n", fixture_path );
		return 1;
	}

	test_scan_finds_exactly_the_two_deleted_files( volume );
	test_scan_calls_progress_callback_with_monotonic_current_and_stable_total( volume );
	test_scan_with_null_callback_behaves_exactly_as_before( volume );
	test_scan_result_free_is_safe_on_empty_result();

	{
		libfsntfs_error_t *error = NULL;
		libfsntfs_volume_close( volume, &error );
		libfsntfs_volume_free( &volume, &error );
	}

	TEST_SUMMARY();
}
