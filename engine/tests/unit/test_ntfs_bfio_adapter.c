/*
 * 2026-11-05 小树裁决第20步 N7：测试的是 ntfs_bfio_read_core 这个回调
 * 核心逻辑和 mock 设备"对接对不对"，重试/跳过的决策逻辑本身在
 * bad_sector_policy_decide/aligned_device_reader_read_block 那一层已经
 * 验证过，这里不重新证明一遍，复用 test_aligned_device_reader.c 同款
 * mock 设备。
 */
#include <string.h>

#include "test_framework.h"
#include "../../shell/ntfs_bfio_adapter.h"

#define MOCK_FAIL_FOREVER 0xFFFFFFFFu

typedef struct
{
	const uint8_t *source;
	size_t source_size;
	size_t position;
	size_t sector_size;

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

static void test_reads_non_sector_aligned_request_on_healthy_device( void )
{
	/* 这正是 N2 判断过的真正风险点：libfsntfs 发起的请求大概率不是扇区
	 * 整数倍，这里故意请求 100 字节（不是 512 的整数倍），模拟设备要求
	 * 扇区对齐，验证核心函数能正确膨胀请求、裁剪出调用方真正要的部分 */
	uint8_t source[ 512 ];
	uint8_t buffer[ 100 ];
	mock_device_t device;
	size_t i;
	ssize_t result;

	TEST_CASE( "非扇区对齐的请求长度（100字节），设备要求扇区对齐，应该正确膨胀+裁剪读出来" );

	for( i = 0; i < sizeof( source ); i++ )
	{
		source[ i ] = (uint8_t) ( i + 1 );
	}

	memset( &device, 0, sizeof( device ) );
	device.source      = source;
	device.source_size = sizeof( source );
	device.sector_size = 512;

	result = ntfs_bfio_read_core( mock_device_read, &device, buffer, sizeof( buffer ), 0, 0, NULL );

	TEST_ASSERT_EQ_INT( (int) result, 100, "应该正好读到 100 字节" );
	TEST_ASSERT( memcmp( buffer, source, 100 ) == 0, "内容应该完全正确" );
}

static void test_retries_transient_failure_then_succeeds( void )
{
	uint8_t source[ 512 ];
	uint8_t buffer[ 100 ];
	mock_device_t device;
	size_t i;
	ssize_t result;

	TEST_CASE( "瞬时故障应该靠重试读出正确内容" );

	for( i = 0; i < sizeof( source ); i++ )
	{
		source[ i ] = (uint8_t) ( i + 1 );
	}

	memset( &device, 0, sizeof( device ) );
	device.source           = source;
	device.source_size      = sizeof( source );
	device.sector_size      = 512;
	device.fail_zone_start  = 0;
	device.fail_zone_length = 512;
	device.fail_countdown   = 2;

	result = ntfs_bfio_read_core( mock_device_read, &device, buffer, sizeof( buffer ), 3, 0, NULL );

	TEST_ASSERT_EQ_INT( (int) result, 100, "重试次数够用应该最终成功" );
	TEST_ASSERT( memcmp( buffer, source, 100 ) == 0, "内容应该完全正确" );
}

static void test_persistent_failure_skipped_and_logged_when_tolerant( void )
{
	uint8_t source[ 512 ];
	uint8_t buffer[ 100 ];
	uint8_t zeros[ 100 ];
	mock_device_t device;
	bad_sector_log_t bad_log;
	size_t i;
	ssize_t result;

	TEST_CASE( "永久坏道+启用坏道容忍，应该返回成功但是 0 填充，并记录坏道（偏移用调用方传入的值）" );

	for( i = 0; i < sizeof( source ); i++ )
	{
		source[ i ] = (uint8_t) ( i + 1 );
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

	result = ntfs_bfio_read_core( mock_device_read, &device, buffer, sizeof( buffer ), 3, 999888, &bad_log );

	TEST_ASSERT_EQ_INT( (int) result, 100, "启用坏道容忍时应该返回成功（声称的字节数）" );
	TEST_ASSERT( memcmp( buffer, zeros, 100 ) == 0, "buffer 应该是 0 填充" );
	TEST_ASSERT_EQ_INT( bad_log.count, 1, "应该正好记录 1 条坏道" );
	if( bad_log.count == 1 )
	{
		TEST_ASSERT_EQ_INT( (int) bad_log.records[ 0 ].offset, 999888, "记录的偏移应该是调用方传入的 offset_for_log" );
		TEST_ASSERT_EQ_INT( (int) bad_log.records[ 0 ].length, 100, "记录的长度应该是调用方请求的 size" );
	}
	bad_sector_log_free( &bad_log );
}

static void test_persistent_failure_fails_when_not_tolerant( void )
{
	uint8_t source[ 512 ];
	uint8_t buffer[ 100 ];
	mock_device_t device;
	size_t i;
	ssize_t result;

	TEST_CASE( "永久坏道+不启用坏道容忍（out_bad_sector_log=NULL），应该直接返回 -1" );

	for( i = 0; i < sizeof( source ); i++ )
	{
		source[ i ] = (uint8_t) ( i + 1 );
	}

	memset( &device, 0, sizeof( device ) );
	device.source           = source;
	device.source_size      = sizeof( source );
	device.sector_size      = 512;
	device.fail_zone_start  = 0;
	device.fail_zone_length = 512;
	device.fail_countdown   = MOCK_FAIL_FOREVER;

	result = ntfs_bfio_read_core( mock_device_read, &device, buffer, sizeof( buffer ), 3, 0, NULL );

	TEST_ASSERT_EQ_INT( (int) result, -1, "不启用容忍时应该直接失败" );
}

static void test_rejects_invalid_arguments( void )
{
	mock_device_t device;
	uint8_t buffer[ 10 ];
	ssize_t result;

	TEST_CASE( "参数不合法时应该返回 -1，而不是崩溃" );

	memset( &device, 0, sizeof( device ) );

	result = ntfs_bfio_read_core( NULL, &device, buffer, sizeof( buffer ), 0, 0, NULL );
	TEST_ASSERT_EQ_INT( (int) result, -1, "read_fn 为 NULL 应该返回 -1" );

	result = ntfs_bfio_read_core( mock_device_read, &device, NULL, sizeof( buffer ), 0, 0, NULL );
	TEST_ASSERT_EQ_INT( (int) result, -1, "buffer 为 NULL 应该返回 -1" );
}

static void test_zero_size_returns_zero( void )
{
	mock_device_t device;
	uint8_t buffer[ 10 ];
	ssize_t result;

	TEST_CASE( "size 为 0 时应该直接返回 0，不是错误" );

	memset( &device, 0, sizeof( device ) );
	device.source      = buffer;
	device.source_size = sizeof( buffer );

	result = ntfs_bfio_read_core( mock_device_read, &device, buffer, 0, 0, 0, NULL );
	TEST_ASSERT_EQ_INT( (int) result, 0, "size=0 应该返回 0" );
}

int main( void )
{
	test_reads_non_sector_aligned_request_on_healthy_device();
	test_retries_transient_failure_then_succeeds();
	test_persistent_failure_skipped_and_logged_when_tolerant();
	test_persistent_failure_fails_when_not_tolerant();
	test_rejects_invalid_arguments();
	test_zero_size_returns_zero();

	TEST_SUMMARY();
}
