/*
 * 用一个内存 mock 模拟"真实 Windows 裸设备只接受扇区对齐长度的读取
 * 请求"这个限制（不是扇区整数倍就返回 0，和真实 ReadFile 的行为一致），
 * 端到端验证 aligned_device_reader_copy_to_file 能不能正确处理——
 * 这正是 2026-11-05 在真实 FAT32/exFAT 设备上第一次恢复小文件时才
 * 发现的那个 bug 的回归测试，现在搬到 Mac 本地单元测试里跑，不用再
 * 依赖虚拟机真机测试才能验证这段逻辑。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_framework.h"
#include "../../core/bad_sector_log.h"
#include "../../shell/aligned_device_reader.h"

/* MOCK_FAIL_FOREVER：坏道区域永远读不出来，重试多少次都没用——模拟真实
 * 物理坏道（和"暂时抖了一下，重试就好"的瞬时性错误是两种不同的场景，
 * P5 的测试需要分别覆盖）。 */
#define MOCK_FAIL_FOREVER 0xFFFFFFFFu

typedef struct
{
	const uint8_t *source;
	size_t source_size;
	size_t position;
	size_t sector_size;  /* 0 = 不模拟任何对齐限制，像一个普通文件 */

	/* P5 坏道保护测试用（2026-11-05 小树裁决第17步）：当前读取位置落在
	 * [fail_zone_start, fail_zone_start+fail_zone_length) 范围内、且
	 * fail_countdown 还没清零时，模拟这次读取失败（返回 0，不推进
	 * position）。fail_countdown 不是 MOCK_FAIL_FOREVER 时每失败一次就
	 * 减 1，减到 0 之后这个区域就能正常读出——模拟"重试几次就好"的瞬时
	 * 故障；传 MOCK_FAIL_FOREVER 表示这个区域永远读不出来，模拟真实
	 * 物理坏道。不需要模拟坏道的测试把 fail_zone_length 留 0 即可
	 * （落不进任何区间，行为和原来完全一样）。 */
	size_t fail_zone_start;
	size_t fail_zone_length;
	uint32_t fail_countdown;
}
mock_device_t;

static size_t mock_device_read( void *context, void *buffer, size_t requested_size )
{
	mock_device_t *device = (mock_device_t *) context;
	size_t available;
	size_t to_copy;

	/* 核心：模拟真实设备的限制——请求长度不是扇区大小整数倍时直接拒绝，
	 * 一个字节都不给，这正是当初撞上的那个真实行为 */
	if( ( device->sector_size > 0 ) && ( requested_size % device->sector_size != 0 ) )
	{
		return 0;
	}

	if( device->fail_zone_length > 0
	 && device->position >= device->fail_zone_start
	 && device->position < device->fail_zone_start + device->fail_zone_length
	 && device->fail_countdown > 0 )
	{
		if( device->fail_countdown != MOCK_FAIL_FOREVER )
		{
			device->fail_countdown--;
		}
		return 0;
	}

	available = device->source_size - device->position;
	to_copy   = requested_size < available ? requested_size : available;

	memcpy( buffer, device->source + device->position, to_copy );
	device->position += to_copy;

	return to_copy;
}

static uint8_t *read_whole_file( const char *path, size_t *out_size )
{
	FILE *f = fopen( path, "rb" );
	uint8_t *buffer;
	long size;

	if( f == NULL )
	{
		return NULL;
	}
	fseek( f, 0, SEEK_END );
	size = ftell( f );
	fseek( f, 0, SEEK_SET );

	buffer = (uint8_t *) malloc( (size_t) size );
	if( buffer == NULL || fread( buffer, 1, (size_t) size, f ) != (size_t) size )
	{
		free( buffer );
		fclose( f );
		return NULL;
	}
	fclose( f );
	*out_size = (size_t) size;
	return buffer;
}

/* 回归测试的核心场景：恢复一个 53 字节的小文件，设备扇区大小 512，
 * 对应 FAT32 那次真实撞上的 bug（见 docs/开发日志.md）。 */
static void test_small_file_smaller_than_one_sector( void )
{
	uint8_t source[ 53 ];
	mock_device_t device;
	const char *output_path = "/tmp/test_aligned_device_reader_small.bin";
	uint8_t *result;
	size_t result_size = 0;
	int i;

	TEST_CASE( "恢复一个 53 字节的小文件（小于一个扇区），设备要求读取长度必须是扇区整数倍" );

	for( i = 0; i < 53; i++ )
	{
		source[ i ] = (uint8_t) ( i + 1 );
	}

	memset( &device, 0, sizeof( device ) );
	device.source      = source;
	device.source_size = sizeof( source );
	device.position    = 0;
	device.sector_size = 512;

	TEST_ASSERT( aligned_device_reader_copy_to_file(
	             mock_device_read, &device, device.sector_size, sizeof( source ), 0, NULL, output_path ) == 1,
	             "拷贝不应该失败——这正是当初真实撞上的那个 bug：如果不做扇区对齐，\n"
	             "        请求 53 字节会被 mock（模拟真实设备）拒绝返回 0，导致这里失败" );

	result = read_whole_file( output_path, &result_size );
	TEST_ASSERT( result != NULL, "应该能读到输出文件" );
	if( result != NULL )
	{
		TEST_ASSERT_EQ_INT( (int) result_size, 53, "输出文件大小应该正好是 53 字节，多读到的部分应该被丢弃" );
		TEST_ASSERT( memcmp( result, source, 53 ) == 0, "输出内容应该和源数据完全一致" );
		free( result );
	}
	remove( output_path );
}

/* 跨多个扇区、多个缓冲区块的场景，确保循环本身（不只是单次对齐）是对的 */
static void test_multi_sector_multi_chunk( void )
{
	uint8_t source[ 10000 ];
	mock_device_t device;
	const char *output_path = "/tmp/test_aligned_device_reader_multi.bin";
	uint8_t *result;
	size_t result_size = 0;
	size_t i;

	TEST_CASE( "跨多个扇区、多个内部缓冲区块的大文件，应该完整、按顺序拷贝" );

	for( i = 0; i < sizeof( source ); i++ )
	{
		source[ i ] = (uint8_t) ( i % 256 );
	}

	memset( &device, 0, sizeof( device ) );
	device.source      = source;
	device.source_size = sizeof( source );
	device.position    = 0;
	device.sector_size = 512;

	TEST_ASSERT( aligned_device_reader_copy_to_file(
	             mock_device_read, &device, device.sector_size, sizeof( source ), 0, NULL, output_path ) == 1,
	             "拷贝不应该失败" );

	result = read_whole_file( output_path, &result_size );
	TEST_ASSERT( result != NULL, "应该能读到输出文件" );
	if( result != NULL )
	{
		TEST_ASSERT_EQ_INT( (int) result_size, (int) sizeof( source ), "输出文件大小应该和源数据完全一致" );
		TEST_ASSERT( memcmp( result, source, sizeof( source ) ) == 0, "输出内容应该和源数据完全一致，顺序不能乱" );
		free( result );
	}
	remove( output_path );
}

/* sector_size = 0：不是真实设备（比如对着普通镜像文件），不应该做任何对齐 */
static void test_zero_sector_size_behaves_like_plain_file( void )
{
	uint8_t source[ 37 ];
	mock_device_t device;
	const char *output_path = "/tmp/test_aligned_device_reader_plain.bin";
	uint8_t *result;
	size_t result_size = 0;
	size_t i;

	TEST_CASE( "sector_size=0 时应该像读普通文件一样工作，不受'扇区对齐'限制" );

	for( i = 0; i < sizeof( source ); i++ )
	{
		source[ i ] = (uint8_t) ( i * 3 );
	}

	memset( &device, 0, sizeof( device ) );
	device.source      = source;
	device.source_size = sizeof( source );
	device.position    = 0;
	device.sector_size = 0;

	TEST_ASSERT( aligned_device_reader_copy_to_file(
	             mock_device_read, &device, 0, sizeof( source ), 0, NULL, output_path ) == 1,
	             "拷贝不应该失败" );

	result = read_whole_file( output_path, &result_size );
	TEST_ASSERT( result != NULL, "应该能读到输出文件" );
	if( result != NULL )
	{
		TEST_ASSERT_EQ_INT( (int) result_size, 37, "输出大小应该正好是 37 字节" );
		free( result );
	}
	remove( output_path );
}

/* 设备提前耗尽（比如底层出错或者数据被截断），应该报失败而不是悄悄写出不完整的文件 */
static void test_device_runs_out_early_is_a_failure( void )
{
	uint8_t source[ 10 ];
	mock_device_t device;
	const char *output_path = "/tmp/test_aligned_device_reader_short.bin";

	TEST_CASE( "设备提前耗尽（数据不够 total_size 声称的长度）应该返回失败" );

	memset( source, 0xAB, sizeof( source ) );

	memset( &device, 0, sizeof( device ) );
	device.source      = source;
	device.source_size = sizeof( source );  /* 只有 10 字节 */
	device.position    = 0;
	device.sector_size = 512;

	/* 声称要读 1000 字节，但设备只有 10 字节，且 10 不是 512 的整数倍，
	 * mock 会在请求对齐后的 512 字节时真的尝试拷贝，position 很快超过
	 * source_size，但不会越界（available 会变成 0），usable 最终变 0，
	 * 应该在还没读够 1000 字节时就失败 */
	TEST_ASSERT( aligned_device_reader_copy_to_file(
	             mock_device_read, &device, device.sector_size, 1000, 0, NULL, output_path ) == -1,
	             "设备数据不够时应该返回 -1，而不是悄悄写出一个不完整的文件" );

	remove( output_path );
}

/* P5 坏道保护（2026-11-05 小树裁决第17步）：第二块（offset 4096-8191，
 * 正好是内部缓冲区 ALIGNED_DEVICE_READER_BUFFER_SIZE=4096 的下一块）
 * 模拟"瞬时故障"——失败 2 次，第 3 次（在 max_retries=3 允许范围内）
 * 就恢复正常了。预期：最终内容完整正确，不应该有任何坏道被记录，
 * 因为重试就真的把这块数据读出来了，不是靠跳过蒙混过关。 */
static void test_transient_failure_recovers_via_retry_without_skipping( void )
{
	uint8_t source[ 8192 ];
	mock_device_t device;
	bad_sector_log_t bad_log;
	const char *output_path = "/tmp/test_aligned_device_reader_retry.bin";
	uint8_t *result;
	size_t result_size = 0;
	size_t i;

	TEST_CASE( "瞬时读取失败（重试几次就恢复正常）应该靠重试读出正确内容，不应该被记成坏道" );

	for( i = 0; i < sizeof( source ); i++ )
	{
		source[ i ] = (uint8_t) ( i % 256 );
	}

	memset( &device, 0, sizeof( device ) );
	device.source          = source;
	device.source_size     = sizeof( source );
	device.sector_size     = 512;
	device.fail_zone_start = 4096;
	device.fail_zone_length = 4096;
	device.fail_countdown  = 2;  /* 失败 2 次，第 3 次成功——max_retries=3 应该够用 */

	memset( &bad_log, 0, sizeof( bad_log ) );

	TEST_ASSERT( aligned_device_reader_copy_to_file(
	             mock_device_read, &device, device.sector_size, sizeof( source ), 3, &bad_log, output_path ) == 1,
	             "重试次数够用时应该最终成功，不应该失败" );
	TEST_ASSERT_EQ_INT( bad_log.count, 0, "重试就能读出来的情况，不应该记录任何坏道" );

	result = read_whole_file( output_path, &result_size );
	TEST_ASSERT( result != NULL, "应该能读到输出文件" );
	if( result != NULL )
	{
		TEST_ASSERT_EQ_INT( (int) result_size, (int) sizeof( source ), "输出文件大小应该和源数据完全一致" );
		TEST_ASSERT( memcmp( result, source, sizeof( source ) ) == 0,
		             "内容应该完全正确（重试读出来的，不是跳过补 0 的）" );
		free( result );
	}
	bad_sector_log_free( &bad_log );
	remove( output_path );
}

/* 永久性坏道（重试多少次都没用）+ 启用坏道容忍：应该跳过这一块、
 * 整体仍然成功、坏道区域在输出文件里是 0，其它区域内容完整正确，
 * 并且准确记录了跳过的偏移和长度。 */
static void test_persistent_bad_sector_is_skipped_and_logged_when_tolerant( void )
{
	uint8_t source[ 8192 ];
	mock_device_t device;
	bad_sector_log_t bad_log;
	const char *output_path = "/tmp/test_aligned_device_reader_skip.bin";
	uint8_t *result;
	size_t result_size = 0;
	size_t i;

	TEST_CASE( "永久性坏道（重试也读不出来）+ 启用坏道容忍：应该跳过并记录，整体仍然成功" );

	for( i = 0; i < sizeof( source ); i++ )
	{
		source[ i ] = (uint8_t) ( i % 256 );
	}

	memset( &device, 0, sizeof( device ) );
	device.source           = source;
	device.source_size      = sizeof( source );
	device.sector_size      = 512;
	device.fail_zone_start  = 4096;
	device.fail_zone_length = 4096;
	device.fail_countdown   = MOCK_FAIL_FOREVER;

	memset( &bad_log, 0, sizeof( bad_log ) );

	TEST_ASSERT( aligned_device_reader_copy_to_file(
	             mock_device_read, &device, device.sector_size, sizeof( source ), 3, &bad_log, output_path ) == 1,
	             "启用坏道容忍时，永久性坏道不应该导致整个恢复失败" );
	TEST_ASSERT_EQ_INT( bad_log.count, 1, "应该正好记录 1 条坏道" );

	if( bad_log.count == 1 )
	{
		TEST_ASSERT_EQ_INT( (int) bad_log.records[ 0 ].offset, 4096, "坏道偏移应该是 4096" );
		TEST_ASSERT_EQ_INT( (int) bad_log.records[ 0 ].length, 4096, "坏道长度应该是 4096" );
	}

	result = read_whole_file( output_path, &result_size );
	TEST_ASSERT( result != NULL, "应该能读到输出文件" );
	if( result != NULL )
	{
		uint8_t zeros[ 4096 ];

		memset( zeros, 0, sizeof( zeros ) );
		TEST_ASSERT_EQ_INT( (int) result_size, (int) sizeof( source ), "输出文件大小应该保持和原声称大小一致（不会因为跳过而变短）" );
		TEST_ASSERT( memcmp( result, source, 4096 ) == 0, "坏道之前的内容应该完整正确" );
		TEST_ASSERT( memcmp( result + 4096, zeros, 4096 ) == 0, "坏道区域应该是 0 填充" );
		free( result );
	}
	bad_sector_log_free( &bad_log );
	remove( output_path );
}

/* 同样的永久性坏道，但不启用坏道容忍（out_bad_sector_log=NULL）：
 * 应该保持原有行为——直接失败，不会悄悄跳过写出不完整的内容 */
static void test_persistent_bad_sector_fails_when_not_tolerant( void )
{
	uint8_t source[ 8192 ];
	mock_device_t device;
	const char *output_path = "/tmp/test_aligned_device_reader_no_tolerance.bin";
	size_t i;

	TEST_CASE( "永久性坏道 + 不启用坏道容忍（out_bad_sector_log=NULL）：应该直接失败，不是悄悄跳过" );

	for( i = 0; i < sizeof( source ); i++ )
	{
		source[ i ] = (uint8_t) ( i % 256 );
	}

	memset( &device, 0, sizeof( device ) );
	device.source           = source;
	device.source_size      = sizeof( source );
	device.sector_size      = 512;
	device.fail_zone_start  = 4096;
	device.fail_zone_length = 4096;
	device.fail_countdown   = MOCK_FAIL_FOREVER;

	TEST_ASSERT( aligned_device_reader_copy_to_file(
	             mock_device_read, &device, device.sector_size, sizeof( source ), 3, NULL, output_path ) == -1,
	             "out_bad_sector_log 为 NULL 时应该保持原有行为：读失败就直接返回 -1" );

	remove( output_path );
}

/* 2026-11-05 小树裁决第18步：aligned_device_reader_read_block 是给"一次性
 * 读一整块"场景（比如读一个簇）用的新函数，和上面流式拷贝的测试场景
 * 不同，这里专门针对它单独写测试，复用同一个 mock 设备。 */

static void test_read_block_succeeds_on_healthy_device( void )
{
	uint8_t source[ 512 ];
	uint8_t buffer[ 512 ];
	mock_device_t device;
	size_t i;

	TEST_CASE( "read_block：设备健康时应该正常读到完整数据" );

	for( i = 0; i < sizeof( source ); i++ )
	{
		source[ i ] = (uint8_t) ( i % 256 );
	}

	memset( &device, 0, sizeof( device ) );
	device.source      = source;
	device.source_size = sizeof( source );
	device.sector_size = 512;

	TEST_ASSERT( aligned_device_reader_read_block(
	             mock_device_read, &device, buffer, sizeof( buffer ), 0, 0, NULL ) == 1,
	             "健康设备读取不应该失败" );
	TEST_ASSERT( memcmp( buffer, source, sizeof( source ) ) == 0, "读到的内容应该完全正确" );
}

static void test_read_block_retries_transient_failure( void )
{
	uint8_t source[ 512 ];
	uint8_t buffer[ 512 ];
	mock_device_t device;
	size_t i;

	TEST_CASE( "read_block：瞬时故障应该靠重试读出正确内容" );

	for( i = 0; i < sizeof( source ); i++ )
	{
		source[ i ] = (uint8_t) ( i % 256 );
	}

	memset( &device, 0, sizeof( device ) );
	device.source           = source;
	device.source_size      = sizeof( source );
	device.sector_size      = 512;
	device.fail_zone_start  = 0;
	device.fail_zone_length = 512;
	device.fail_countdown   = 2;

	TEST_ASSERT( aligned_device_reader_read_block(
	             mock_device_read, &device, buffer, sizeof( buffer ), 3, 0, NULL ) == 1,
	             "重试次数够用时应该最终成功" );
	TEST_ASSERT( memcmp( buffer, source, sizeof( source ) ) == 0, "内容应该完全正确（重试读出来的）" );
}

static void test_read_block_skips_and_logs_persistent_bad_sector( void )
{
	uint8_t source[ 512 ];
	uint8_t buffer[ 512 ];
	uint8_t zeros[ 512 ];
	mock_device_t device;
	bad_sector_log_t bad_log;
	size_t i;

	TEST_CASE( "read_block：永久坏道+启用容忍，应该返回成功但 buffer 是 0 填充，并记录坏道" );

	for( i = 0; i < sizeof( source ); i++ )
	{
		source[ i ] = (uint8_t) ( i % 256 );
	}
	memset( zeros, 0, sizeof( zeros ) );

	memset( &device, 0, sizeof( device ) );
	device.source           = source;
	device.source_size      = sizeof( source );
	device.sector_size      = 512;
	device.fail_zone_start  = 0;
	device.fail_zone_length = 512;
	device.fail_countdown   = MOCK_FAIL_FOREVER;

	memset( &bad_log, 0, sizeof( bad_log ) );

	TEST_ASSERT( aligned_device_reader_read_block(
	             mock_device_read, &device, buffer, sizeof( buffer ), 3, 12345, &bad_log ) == 1,
	             "启用坏道容忍时，永久坏道不应该导致失败" );
	TEST_ASSERT( memcmp( buffer, zeros, sizeof( zeros ) ) == 0, "buffer 应该是 0 填充" );
	TEST_ASSERT_EQ_INT( bad_log.count, 1, "应该正好记录 1 条坏道" );
	if( bad_log.count == 1 )
	{
		TEST_ASSERT_EQ_INT( (int) bad_log.records[ 0 ].offset, 12345, "记录的偏移应该是调用方传入的 12345" );
		TEST_ASSERT_EQ_INT( (int) bad_log.records[ 0 ].length, 512, "记录的长度应该是 block_size=512" );
	}
	bad_sector_log_free( &bad_log );
}

static void test_read_block_fails_on_persistent_bad_sector_when_not_tolerant( void )
{
	uint8_t source[ 512 ];
	uint8_t buffer[ 512 ];
	mock_device_t device;
	size_t i;

	TEST_CASE( "read_block：永久坏道+不启用容忍（out_bad_sector_log=NULL），应该直接返回 -1" );

	for( i = 0; i < sizeof( source ); i++ )
	{
		source[ i ] = (uint8_t) ( i % 256 );
	}

	memset( &device, 0, sizeof( device ) );
	device.source           = source;
	device.source_size      = sizeof( source );
	device.sector_size      = 512;
	device.fail_zone_start  = 0;
	device.fail_zone_length = 512;
	device.fail_countdown   = MOCK_FAIL_FOREVER;

	TEST_ASSERT( aligned_device_reader_read_block(
	             mock_device_read, &device, buffer, sizeof( buffer ), 3, 0, NULL ) == -1,
	             "不启用容忍时应该直接失败" );
}

static void test_read_block_rejects_invalid_arguments( void )
{
	uint8_t buffer[ 512 ];
	mock_device_t device;

	TEST_CASE( "read_block：参数不合法时应该返回 -1，而不是崩溃" );

	memset( &device, 0, sizeof( device ) );

	TEST_ASSERT( aligned_device_reader_read_block( NULL, &device, buffer, sizeof( buffer ), 0, 0, NULL ) == -1,
	             "read_fn 为 NULL 应该返回 -1" );
	TEST_ASSERT( aligned_device_reader_read_block( mock_device_read, &device, NULL, sizeof( buffer ), 0, 0, NULL ) == -1,
	             "buffer 为 NULL 应该返回 -1" );
	TEST_ASSERT( aligned_device_reader_read_block( mock_device_read, &device, buffer, 0, 0, 0, NULL ) == -1,
	             "block_size 为 0 应该返回 -1" );
}

int main( void )
{
	test_small_file_smaller_than_one_sector();
	test_multi_sector_multi_chunk();
	test_zero_sector_size_behaves_like_plain_file();
	test_device_runs_out_early_is_a_failure();
	test_transient_failure_recovers_via_retry_without_skipping();
	test_persistent_bad_sector_is_skipped_and_logged_when_tolerant();
	test_persistent_bad_sector_fails_when_not_tolerant();
	test_read_block_succeeds_on_healthy_device();
	test_read_block_retries_transient_failure();
	test_read_block_skips_and_logs_persistent_bad_sector();
	test_read_block_fails_on_persistent_bad_sector_when_not_tolerant();
	test_read_block_rejects_invalid_arguments();

	TEST_SUMMARY();
}
