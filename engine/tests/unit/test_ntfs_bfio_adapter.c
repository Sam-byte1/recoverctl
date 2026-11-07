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

	result = ntfs_bfio_read_core( mock_device_read, &device, buffer, sizeof( buffer ), 0, 0, 0, NULL );

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

	result = ntfs_bfio_read_core( mock_device_read, &device, buffer, sizeof( buffer ), 0, 3, 0, NULL );

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

	result = ntfs_bfio_read_core( mock_device_read, &device, buffer, sizeof( buffer ), 0, 3, 999888, &bad_log );

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

	result = ntfs_bfio_read_core( mock_device_read, &device, buffer, sizeof( buffer ), 0, 3, 0, NULL );

	TEST_ASSERT_EQ_INT( (int) result, -1, "不启用容忍时应该直接失败" );
}

static void test_rejects_invalid_arguments( void )
{
	mock_device_t device;
	uint8_t buffer[ 10 ];
	ssize_t result;

	TEST_CASE( "参数不合法时应该返回 -1，而不是崩溃" );

	memset( &device, 0, sizeof( device ) );

	result = ntfs_bfio_read_core( NULL, &device, buffer, sizeof( buffer ), 0, 0, 0, NULL );
	TEST_ASSERT_EQ_INT( (int) result, -1, "read_fn 为 NULL 应该返回 -1" );

	result = ntfs_bfio_read_core( mock_device_read, &device, NULL, sizeof( buffer ), 0, 0, 0, NULL );
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

	result = ntfs_bfio_read_core( mock_device_read, &device, buffer, 0, 0, 0, 0, NULL );
	TEST_ASSERT_EQ_INT( (int) result, 0, "size=0 应该返回 0" );
}

/*
 * B9真机验证第二轮（2026-11-05小树裁决）：lead_in参数的TDD覆盖——
 * 真机验证能跑通的是"读到正确内容"这个端到端结果，覆盖不到"lead_in
 * 计算/裁剪哪里有符号错误导致读出来的内容实际上是垫片字节"这种细节
 * 错误，这里直接验证能正确跳过lead_in、裁出调用方要的部分。
 */
static void test_lead_in_skips_padding_bytes( void )
{
	uint8_t source[ 1024 ];
	uint8_t buffer[ 100 ];
	mock_device_t device;
	size_t i;
	ssize_t result;

	TEST_CASE( "lead_in>0时，应该跳过前面lead_in个字节，裁出调用方真正要的size字节" );

	for( i = 0; i < sizeof( source ); i++ )
	{
		source[ i ] = (uint8_t) ( i + 1 );
	}

	memset( &device, 0, sizeof( device ) );
	device.source      = source;
	device.source_size = sizeof( source );
	device.sector_size = 512;

	/* lead_in=64：真正要的100字节应该是source[64..163] */
	result = ntfs_bfio_read_core( mock_device_read, &device, buffer, sizeof( buffer ), 64, 0, 0, NULL );

	TEST_ASSERT_EQ_INT( (int) result, 100, "应该正好读到100字节（跳过lead_in之后）" );
	TEST_ASSERT( memcmp( buffer, source + 64, 100 ) == 0, "内容应该是跳过前64字节之后的部分，不是从0开始" );
}

static void test_lead_in_overflow_fails_loudly( void )
{
	mock_device_t device;
	uint8_t buffer[ 100 ];
	ssize_t result;

	TEST_CASE( "lead_in>0且(lead_in+size)超过内部对齐缓冲区上限时，应该直接返回-1，"
	           "不应该静默读出被lead_in污染的数据（2026-11-05小树裁决：失败得响亮优于悄悄用错位置）" );

	memset( &device, 0, sizeof( device ) );

	/* NTFS_BFIO_ALIGNED_BUFFER_SIZE 是 65536，随便挑一个lead_in+size明显超限的组合 */
	result = ntfs_bfio_read_core( mock_device_read, &device, buffer, sizeof( buffer ), 65536, 0, 0, NULL );

	TEST_ASSERT_EQ_INT( (int) result, -1, "应该返回-1" );
}

static void test_size_overflow_fails_loudly_even_with_zero_lead_in( void )
{
	uint8_t big_buffer[ 70000 ];
	mock_device_t device;
	ssize_t result;

	TEST_CASE( "2026-11-07修正：lead_in=0时size超过内部对齐缓冲区上限曾经被设计成"
	           "'退化成直接读取、跳过重试/坏道容忍'，这条分支被真机测试证实是真实的"
	           "静默数据损坏缺口（一个60MB跨多个data run的NTFS大文件被这条分支恢复成"
	           "内容全是0字节，大小却仍然正确、坏道日志也不会记录任何异常）。修复后这个"
	           "函数对任何超过内部缓冲区上限的单次请求一律返回-1，强制调用方"
	           "（ntfs_bfio_read/vshadow_bfio_read）自己拆成不超过上限的若干块分别调用，"
	           "不再提供'悄悄跳过保护机制直接摸设备'这条退路" );

	memset( &device, 0, sizeof( device ) );

	result = ntfs_bfio_read_core( mock_device_read, &device, big_buffer, sizeof( big_buffer ), 0, 0, 0, NULL );

	TEST_ASSERT_EQ_INT( (int) result, -1, "超过内部对齐缓冲区上限的请求，不管lead_in是不是0，都应该直接返回-1" );
}

/*
 * 2026-11-05 小树裁决（BitLocker锁定卷base_offset偏移感知方案）：
 * `ntfs_bfio_seek_offset_core`是真实`seek_offset`回调里"算出新逻辑
 * 偏移"这段纯算术逻辑的注入核心，和上面的`ntfs_bfio_read_core`同样的
 * 依赖注入模式，这里补的是裁决记录里明确点名"值得TDD覆盖、真机验证
 * 覆盖不到"的部分：SEEK_END分支从没被真正触发过（B9真机验证能跑通的
 * 是SEEK_SET+读取这条主路径），万一这里面换算有符号错误，真机测试不
 * 一定能暴露。
 */
static void test_seek_set_returns_offset_directly( void )
{
	int64_t new_offset = -1;

	TEST_CASE( "SEEK_SET：新逻辑偏移就是请求的offset本身，和base_offset/当前位置无关" );

	TEST_ASSERT( ntfs_bfio_seek_offset_core( 12345, 100, SEEK_SET, -1, &new_offset ) == 1, "应该成功" );
	TEST_ASSERT_EQ_INT( (int) new_offset, 100, "SEEK_SET的新偏移应该正好是100" );
}

static void test_seek_cur_adds_to_current_offset( void )
{
	int64_t new_offset = -1;

	TEST_CASE( "SEEK_CUR：新逻辑偏移是当前偏移+offset（可正可负）" );

	TEST_ASSERT( ntfs_bfio_seek_offset_core( 500, 50, SEEK_CUR, -1, &new_offset ) == 1, "正向偏移应该成功" );
	TEST_ASSERT_EQ_INT( (int) new_offset, 550, "500+50应该是550" );

	TEST_ASSERT( ntfs_bfio_seek_offset_core( 500, -50, SEEK_CUR, -1, &new_offset ) == 1, "负向偏移应该成功" );
	TEST_ASSERT_EQ_INT( (int) new_offset, 450, "500-50应该是450" );
}

static void test_seek_end_adds_to_logical_size( void )
{
	int64_t new_offset = -1;

	TEST_CASE( "SEEK_END：新逻辑偏移是logical_size+offset（logical_size是调用方已经减掉"
	           "base_offset之后的逻辑卷大小，不是设备物理大小）" );

	TEST_ASSERT( ntfs_bfio_seek_offset_core( 0, 0, SEEK_END, 1000, &new_offset ) == 1, "offset=0应该成功" );
	TEST_ASSERT_EQ_INT( (int) new_offset, 1000, "应该正好是logical_size本身" );

	TEST_ASSERT( ntfs_bfio_seek_offset_core( 0, -100, SEEK_END, 1000, &new_offset ) == 1, "负offset应该成功" );
	TEST_ASSERT_EQ_INT( (int) new_offset, 900, "1000-100应该是900" );
}

static void test_seek_end_with_unknown_size_fails( void )
{
	int64_t new_offset = -1;

	TEST_CASE( "SEEK_END：logical_size<0（未知/查询失败）时应该直接失败，不猜一个值" );

	TEST_ASSERT( ntfs_bfio_seek_offset_core( 0, 0, SEEK_END, -1, &new_offset ) == -1, "应该返回-1" );
}

static void test_negative_result_rejected( void )
{
	int64_t new_offset = -1;

	TEST_CASE( "算出的新偏移是负数时应该失败——卷内不存在负偏移" );

	TEST_ASSERT( ntfs_bfio_seek_offset_core( 0, -5, SEEK_SET, -1, &new_offset ) == -1,
	             "SEEK_SET到负偏移应该返回-1" );
	TEST_ASSERT( ntfs_bfio_seek_offset_core( 10, -20, SEEK_CUR, -1, &new_offset ) == -1,
	             "SEEK_CUR算出负偏移应该返回-1" );
}

static void test_invalid_whence_rejected( void )
{
	int64_t new_offset = -1;

	TEST_CASE( "whence不是SEEK_SET/SEEK_CUR/SEEK_END时应该失败，而不是当成某个分支的默认行为" );

	TEST_ASSERT( ntfs_bfio_seek_offset_core( 0, 0, 999, -1, &new_offset ) == -1, "应该返回-1" );
}

static void test_seek_core_null_output_rejected( void )
{
	TEST_CASE( "out_new_logical_offset为NULL应该被拒绝，而不是崩溃" );

	TEST_ASSERT( ntfs_bfio_seek_offset_core( 0, 0, SEEK_SET, -1, NULL ) == -1, "应该返回-1" );
}

int main( void )
{
	test_reads_non_sector_aligned_request_on_healthy_device();
	test_retries_transient_failure_then_succeeds();
	test_persistent_failure_skipped_and_logged_when_tolerant();
	test_persistent_failure_fails_when_not_tolerant();
	test_rejects_invalid_arguments();
	test_zero_size_returns_zero();
	test_lead_in_skips_padding_bytes();
	test_lead_in_overflow_fails_loudly();
	test_size_overflow_fails_loudly_even_with_zero_lead_in();
	test_seek_set_returns_offset_directly();
	test_seek_cur_adds_to_current_offset();
	test_seek_end_adds_to_logical_size();
	test_seek_end_with_unknown_size_fails();
	test_negative_result_rejected();
	test_invalid_whence_rejected();
	test_seek_core_null_output_rejected();

	TEST_SUMMARY();
}
