#include "bitlocker_unlock.h"

#include "secure_memory.h"

/* 和 bitlocker_volume_opener.c 同样的理由（见该文件头注释）：
 * libbde_volume_open_file_io_handle 的公开声明被 LIBBDE_HAVE_BFIO 宏
 * 保护、要求一个本项目没有vendor的独立 <libbfio.h>，手写声明绕开，
 * 不依赖libbde.h的条件编译。 */
extern int libbde_volume_open_file_io_handle(
     libbde_volume_t *volume,
     libbfio_handle_t *file_io_handle,
     int access_flags,
     libbde_error_t **error );

int bitlocker_unlock_volume(
     const char *device_path,
     uint64_t base_offset,
     bitlocker_resolved_credential_t *credential,
     libbde_volume_t **out_volume,
     libbfio_handle_t **out_bfio_handle,
     libbde_error_t **error )
{
	libbde_volume_t *volume = NULL;
	libbfio_handle_t *bfio_handle = NULL;
	libfsntfs_error_t *bfio_error = NULL;
	int set_result = -1;
	int is_locked_result;
	int return_code = -1;

	if( ( device_path == NULL )
	 || ( credential == NULL )
	 || ( out_volume == NULL )
	 || ( out_bfio_handle == NULL ) )
	{
		return -1;
	}
	*out_volume = NULL;
	*out_bfio_handle = NULL;

	if( libbde_volume_initialize( &volume, error ) != 1 )
	{
		goto cleanup;
	}
	switch( credential->type )
	{
		case BITLOCKER_CREDENTIAL_TYPE_PASSWORD:
			set_result = libbde_volume_set_utf8_password(
			              volume, credential->text.data, credential->text.length, error );
			break;

		case BITLOCKER_CREDENTIAL_TYPE_RECOVERY_KEY:
			set_result = libbde_volume_set_utf8_recovery_password(
			              volume, credential->text.data, credential->text.length, error );
			break;

		case BITLOCKER_CREDENTIAL_TYPE_STARTUP_KEY:
			set_result = libbde_volume_read_startup_key(
			              volume, credential->startup_key_file_path, error );
			break;

		case BITLOCKER_CREDENTIAL_TYPE_FVEK:
			set_result = libbde_volume_set_keys(
			              volume, credential->fvek, credential->fvek_length,
			              credential->tweak, credential->tweak_length, error );
			break;

		default:
			set_result = -1;
			break;
	}
	if( set_result != 1 )
	{
		libbde_volume_free( &volume, NULL );
		volume = NULL;
		goto cleanup;
	}
	/* B9真机验证发现：裸设备路径不能用libbde_volume_open(path)这个简单
	 * 方式打开（通用libcfile文件抽象层查询卷/设备大小会失败），改用
	 * 自定义bfio句柄——和B1 bitlocker_volume_opener.c同样的解法，见
	 * 该文件头注释完整说明。这一步max_retries=0/out_bad_sector_log=NULL：
	 * 解锁阶段只读取很小一段元数据，不是真实大范围恢复I/O，不需要坏道
	 * 容忍（真正需要坏道容忍的是B7post-unlock之后的大范围读取）。 */
	if( ntfs_bfio_adapter_initialize( device_path, base_offset, 0, NULL, &bfio_handle, &bfio_error ) != 1 )
	{
		if( bfio_error != NULL )
		{
			libfsntfs_error_free( &bfio_error );
		}
		libbde_volume_free( &volume, NULL );
		volume = NULL;
		goto cleanup;
	}
	if( libbde_volume_open_file_io_handle( volume, bfio_handle, LIBBDE_ACCESS_FLAG_READ, error ) != 1 )
	{
		libbfio_handle_free( &bfio_handle, NULL );
		libbde_volume_free( &volume, NULL );
		volume = NULL;
		goto cleanup;
	}
	is_locked_result = libbde_volume_is_locked( volume, error );
	if( is_locked_result != 0 )
	{
		/* is_locked_result == 1：凭据没能真正解开卷（凭据不对/类型
		 * 选错），*error此时不会被libbde填充，是"正常的失败"不是
		 * 系统错误；is_locked_result == -1：libbde内部真实出错，
		 * *error会被填充——两种情况这里统一按失败处理，调用方靠
		 * *error是否为NULL区分这两种失败的性质。 */
		libbde_volume_close( volume, NULL );
		libbfio_handle_free( &bfio_handle, NULL );
		libbde_volume_free( &volume, NULL );
		volume = NULL;
		goto cleanup;
	}
	*out_volume = volume;
	*out_bfio_handle = bfio_handle;
	return_code = 1;

cleanup:
	secure_memory_zero( credential->text.data, sizeof( credential->text.data ) );
	credential->text.length = 0;
	secure_memory_zero( credential->fvek, sizeof( credential->fvek ) );
	credential->fvek_length = 0;
	secure_memory_zero( credential->tweak, sizeof( credential->tweak ) );
	credential->tweak_length = 0;

	return return_code;
}
