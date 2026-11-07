#include "bitlocker_volume_opener.h"

#include <stdlib.h>
#include <string.h>

#include "ntfs_bfio_adapter.h"

/* B9真机验证发现的真实坑（不是理论推测）：`libbde_volume_open(path)`这个
 * 最简单的打开方式，内部走的是libbde自带的通用`libcfile`文件抽象层，
 * 对着一个真实Windows裸卷设备路径（比如`\\.\G:`，尤其是BitLocker锁定
 * 状态下的卷）会在查询文件大小这一步失败（`GetFileSizeEx`对卷/设备
 * 句柄不是通用可靠的查询方式，普通文件才行）——用libbde官方自带的
 * `bdeinfo`诊断工具对真实锁定卷测试复现过同样的失败，确认不是我们
 * 代码的bug，是通用路径本身对裸设备天然不可靠。
 *
 * 解决办法是复用本项目已经为NTFS裸设备访问验证过的
 * `ntfs_bfio_adapter_initialize`（内部`GetFileSizeEx`失败时会退回
 * `DeviceIoControl(IOCTL_DISK_GET_LENGTH_INFO)`这个裸设备正规查询
 * 方式）构造一个自定义libbfio句柄，再调用libbde公开支持的
 * `libbde_volume_open_file_io_handle`——所有libyal库共享同一套
 * libbfio ABI，这个适配器不关心最终喂给哪个库使用，这里直接复用，
 * 不是重新发明一个"bitlocker专属"的裸设备打开逻辑。
 *
 * `libbde_volume_open_file_io_handle`这个符号在libbde.h里的公开声明被
 * `LIBBDE_HAVE_BFIO`宏保护、要求能找到一个独立的`<libbfio.h>`公开头
 * 文件——本项目只vendor了libfsntfs自带的libbfio内部私有拷贝，没有这个
 * 独立头文件，常规方式会因为缺文件编译失败（和`ntfs_bfio_adapter.h`
 * 头注释里记录的`libfsntfs_volume_open_file_io_handle`是同一个坑，
 * 同一个解法：手写声明，不依赖libbde.h的条件编译，也不include任何
 * vendored的libbfio内部头文件——这个符号真实存在于已经链接的libbde
 * 动态库里，实际编译+链接已经验证过）。 */
extern int libbde_volume_open_file_io_handle(
     libbde_volume_t *volume,
     libbfio_handle_t *file_io_handle,
     int access_flags,
     libbde_error_t **error );

static int key_protector_list_append(
     bitlocker_key_protector_list_t *list,
     const bitlocker_key_protector_info_t *entry )
{
	if( list->count == list->capacity )
	{
		int new_capacity = list->capacity == 0 ? 8 : list->capacity * 2;
		bitlocker_key_protector_info_t *new_array = (bitlocker_key_protector_info_t *) realloc(
		                                               list->entries,
		                                               (size_t) new_capacity * sizeof( bitlocker_key_protector_info_t ) );

		if( new_array == NULL )
		{
			return -1;
		}
		list->entries  = new_array;
		list->capacity = new_capacity;
	}
	list->entries[ list->count ] = *entry;
	list->count++;
	return 0;
}

int bitlocker_volume_opener_list_key_protectors(
     const char *device_path,
     uint64_t base_offset,
     bitlocker_key_protector_list_t *out,
     libbde_error_t **error )
{
	libbde_volume_t *volume = NULL;
	libbde_key_protector_t *key_protector = NULL;
	libbfio_handle_t *bfio_handle = NULL;
	libfsntfs_error_t *bfio_error = NULL;
	int number_of_key_protectors = 0;
	int i;

	if( ( device_path == NULL )
	 || ( out == NULL ) )
	{
		return -1;
	}
	out->entries  = NULL;
	out->count    = 0;
	out->capacity = 0;

	if( libbde_volume_initialize( &volume, error ) != 1 )
	{
		return -1;
	}
	/* B1本身是只读诊断，不是真实恢复I/O，不需要坏道容忍：max_retries=0、
	 * out_bad_sector_log=NULL，读失败就是真失败，和项目其它"只是探测"
	 * 场景（比如NTFS探测调用）同样的约定。 */
	if( ntfs_bfio_adapter_initialize( device_path, base_offset, 0, NULL, &bfio_handle, &bfio_error ) != 1 )
	{
		if( bfio_error != NULL )
		{
			libfsntfs_error_free( &bfio_error );
		}
		libbde_volume_free( &volume, NULL );
		return -1;
	}
	if( libbde_volume_open_file_io_handle( volume, bfio_handle, LIBBDE_ACCESS_FLAG_READ, error ) != 1 )
	{
		libbfio_handle_free( &bfio_handle, NULL );
		libbde_volume_free( &volume, NULL );
		return -1;
	}
	if( libbde_volume_get_number_of_key_protectors( volume, &number_of_key_protectors, error ) != 1 )
	{
		libbde_volume_close( volume, NULL );
		libbfio_handle_free( &bfio_handle, NULL );
		libbde_volume_free( &volume, NULL );
		return -1;
	}
	for( i = 0; i < number_of_key_protectors; i++ )
	{
		bitlocker_key_protector_info_t entry;

		memset( &entry, 0, sizeof( entry ) );

		if( libbde_volume_get_key_protector_by_index( volume, i, &key_protector, error ) != 1 )
		{
			bitlocker_key_protector_list_free( out );
			libbde_volume_close( volume, NULL );
			libbfio_handle_free( &bfio_handle, NULL );
			libbde_volume_free( &volume, NULL );
			return -1;
		}
		if( ( libbde_key_protector_get_type( key_protector, &entry.type, error ) != 1 )
		 || ( libbde_key_protector_get_identifier( key_protector, entry.guid, sizeof( entry.guid ), error ) != 1 ) )
		{
			libbde_key_protector_free( &key_protector, NULL );
			bitlocker_key_protector_list_free( out );
			libbde_volume_close( volume, NULL );
			libbfio_handle_free( &bfio_handle, NULL );
			libbde_volume_free( &volume, NULL );
			return -1;
		}
		libbde_key_protector_free( &key_protector, NULL );
		key_protector = NULL;

		if( key_protector_list_append( out, &entry ) != 0 )
		{
			bitlocker_key_protector_list_free( out );
			libbde_volume_close( volume, NULL );
			libbfio_handle_free( &bfio_handle, NULL );
			libbde_volume_free( &volume, NULL );
			return -1;
		}
	}
	libbde_volume_close( volume, NULL );
	libbfio_handle_free( &bfio_handle, NULL );
	libbde_volume_free( &volume, NULL );
	return 1;
}

void bitlocker_key_protector_list_free(
      bitlocker_key_protector_list_t *list )
{
	if( list == NULL )
	{
		return;
	}
	free( list->entries );
	list->entries  = NULL;
	list->count    = 0;
	list->capacity = 0;
}
