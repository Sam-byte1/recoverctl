/*
 * B6（BitLocker，2026-11-05 任务拆解排期）：解锁适配层的覆盖——没有
 * 真实BitLocker卷之前能验证的部分：①对非BitLocker/不存在的目标应该
 * 优雅失败而不是崩溃；②不管成功还是失败，credential里的敏感字段都
 * 应该被清零（这是整条链路"现在就要主动清零"这条拍板决策能不能落地
 * 的关键可观察postcondition，不需要真实设备也能验证）。真正"凭据对的
 * 时候能不能解锁成功"的验证已经在B9上真机做过。
 */
#include <string.h>

#include "test_framework.h"
#include "../../shell/bitlocker_unlock.h"

static void test_nonexistent_device_fails_gracefully( void )
{
	bitlocker_resolved_credential_t credential;
	libbde_volume_t *volume = NULL;
	libbfio_handle_t *bfio_handle = NULL;
	libbde_error_t *error = NULL;

	TEST_CASE( "对一个不存在的设备路径尝试解锁，应该优雅失败，不应该崩溃" );

	memset( &credential, 0, sizeof( credential ) );
	credential.type = BITLOCKER_CREDENTIAL_TYPE_PASSWORD;
	memcpy( credential.text.data, "testpassword", 12 );
	credential.text.length = 12;

	TEST_ASSERT( bitlocker_unlock_volume( "/nonexistent/path/does/not/exist", 0, &credential, &volume, &bfio_handle, &error ) == -1,
	             "应该返回 -1" );
	TEST_ASSERT( volume == NULL, "out_volume应该保持NULL" );
	TEST_ASSERT( bfio_handle == NULL, "out_bfio_handle应该保持NULL" );

	if( error != NULL )
	{
		libbde_error_free( &error );
	}
}

static void test_credential_cleared_after_failure( void )
{
	bitlocker_resolved_credential_t credential;
	libbde_volume_t *volume = NULL;
	libbfio_handle_t *bfio_handle = NULL;
	libbde_error_t *error = NULL;
	int i;
	int all_zero = 1;

	TEST_CASE( "解锁失败之后，credential里的密码/FVEK/TWEAK字段都应该被清零（不管成败都要清零）" );

	memset( &credential, 0xAB, sizeof( credential ) );
	credential.type = BITLOCKER_CREDENTIAL_TYPE_PASSWORD;
	credential.text.length = 12;

	bitlocker_unlock_volume( "/nonexistent/path/does/not/exist", 0, &credential, &volume, &bfio_handle, &error );

	for( i = 0; i < BITLOCKER_CREDENTIAL_MAX_LENGTH; i++ )
	{
		if( credential.text.data[ i ] != 0 )
		{
			all_zero = 0;
			break;
		}
	}
	TEST_ASSERT( all_zero, "text.data 应该被完全清零" );
	TEST_ASSERT_EQ_INT( (int) credential.text.length, 0, "text.length 应该被重置为0" );

	all_zero = 1;
	for( i = 0; i < BITLOCKER_FVEK_MAX_LENGTH; i++ )
	{
		if( ( credential.fvek[ i ] != 0 ) || ( credential.tweak[ i ] != 0 ) )
		{
			all_zero = 0;
			break;
		}
	}
	TEST_ASSERT( all_zero, "fvek/tweak 也应该被清零（即使这次用的是PASSWORD类型，没有用到这两个字段）" );

	if( error != NULL )
	{
		libbde_error_free( &error );
	}
}

static void test_non_bde_file_fails_gracefully( void )
{
	bitlocker_resolved_credential_t credential;
	libbde_volume_t *volume = NULL;
	libbfio_handle_t *bfio_handle = NULL;
	libbde_error_t *error = NULL;

	TEST_CASE( "对一个不是BitLocker卷的普通文件尝试解锁，应该优雅失败" );

	memset( &credential, 0, sizeof( credential ) );
	credential.type = BITLOCKER_CREDENTIAL_TYPE_PASSWORD;
	memcpy( credential.text.data, "irrelevant", 10 );
	credential.text.length = 10;

	TEST_ASSERT( bitlocker_unlock_volume( __FILE__, 0, &credential, &volume, &bfio_handle, &error ) == -1,
	             "对自己这份源代码文件尝试解锁，应该失败" );
	TEST_ASSERT( volume == NULL, "out_volume应该保持NULL" );

	if( error != NULL )
	{
		libbde_error_free( &error );
	}
}

static void test_null_arguments_rejected( void )
{
	bitlocker_resolved_credential_t credential;
	libbde_volume_t *volume = NULL;
	libbfio_handle_t *bfio_handle = NULL;
	libbde_error_t *error = NULL;

	TEST_CASE( "空指针参数应该被拒绝，而不是崩溃" );

	memset( &credential, 0, sizeof( credential ) );

	TEST_ASSERT( bitlocker_unlock_volume( NULL, 0, &credential, &volume, &bfio_handle, &error ) == -1, "device_path为NULL应该返回-1" );
	TEST_ASSERT( bitlocker_unlock_volume( "/tmp/whatever", 0, NULL, &volume, &bfio_handle, &error ) == -1, "credential为NULL应该返回-1" );
	TEST_ASSERT( bitlocker_unlock_volume( "/tmp/whatever", 0, &credential, NULL, &bfio_handle, &error ) == -1, "out_volume为NULL应该返回-1" );
	TEST_ASSERT( bitlocker_unlock_volume( "/tmp/whatever", 0, &credential, &volume, NULL, &error ) == -1, "out_bfio_handle为NULL应该返回-1" );
}

int main( void )
{
	test_nonexistent_device_fails_gracefully();
	test_credential_cleared_after_failure();
	test_non_bde_file_fails_gracefully();
	test_null_arguments_rejected();

	TEST_SUMMARY();
}
