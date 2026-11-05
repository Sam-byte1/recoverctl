#include <windows.h>
#include <winioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "disk_enumerator.h"

static int disk_enum_result_append(
     disk_enum_result_t *result,
     const disk_entry_t *entry )
{
	if( result->count == result->capacity )
	{
		int new_capacity = result->capacity == 0 ? 8 : result->capacity * 2;
		disk_entry_t *new_array = (disk_entry_t *) realloc(
		                            result->entries,
		                            (size_t) new_capacity * sizeof( disk_entry_t ) );

		if( new_array == NULL )
		{
			return -1;
		}
		result->entries  = new_array;
		result->capacity = new_capacity;
	}
	result->entries[ result->count ] = *entry;
	result->count++;
	return 1;
}

static const char *drive_type_name(
     UINT drive_type )
{
	switch( drive_type )
	{
		case DRIVE_REMOVABLE:
			return "Removable";
		case DRIVE_FIXED:
			return "Fixed";
		case DRIVE_REMOTE:
			return "Network";
		case DRIVE_CDROM:
			return "CD-ROM";
		case DRIVE_RAMDISK:
			return "RAM";
		default:
			return "Unknown";
	}
}

int disk_enumerator_list_logical_drives(
     disk_enum_result_t *out )
{
	DWORD drive_mask = 0;
	int letter_index  = 0;

	if( out == NULL )
	{
		return -1;
	}
	out->entries  = NULL;
	out->count    = 0;
	out->capacity = 0;

	drive_mask = GetLogicalDrives();

	if( drive_mask == 0 )
	{
		return -1;
	}

	for( letter_index = 0; letter_index < 26; letter_index++ )
	{
		disk_entry_t entry;
		char root_path[ 4 ];
		UINT type;
		ULARGE_INTEGER total_bytes;
		ULARGE_INTEGER free_bytes;
		char volume_name[ 256 ];
		char file_system[ 32 ];
		DWORD serial_number        = 0;
		DWORD max_component_length = 0;
		DWORD fs_flags             = 0;

		if( ( drive_mask & ( 1u << letter_index ) ) == 0 )
		{
			continue;
		}

		memset( &entry, 0, sizeof( entry ) );
		entry.drive_letter = (char) ( 'A' + letter_index );

		root_path[ 0 ] = entry.drive_letter;
		root_path[ 1 ] = ':';
		root_path[ 2 ] = '\\';
		root_path[ 3 ] = '\0';

		type = GetDriveTypeA( root_path );
		strncpy( entry.drive_type_name, drive_type_name( type ), sizeof( entry.drive_type_name ) - 1 );

		/* 光驱/网络盘等可能暂时没有介质，取不到信息是正常情况，不当成错误 */
		volume_name[ 0 ] = '\0';
		file_system[ 0 ]  = '\0';

		if( GetVolumeInformationA(
		     root_path,
		     volume_name,
		     sizeof( volume_name ),
		     &serial_number,
		     &max_component_length,
		     &fs_flags,
		     file_system,
		     sizeof( file_system ) ) )
		{
			strncpy( entry.volume_label, volume_name, sizeof( entry.volume_label ) - 1 );
			strncpy( entry.file_system, file_system, sizeof( entry.file_system ) - 1 );
		}

		if( GetDiskFreeSpaceExA( root_path, NULL, &total_bytes, &free_bytes ) )
		{
			entry.total_bytes = (uint64_t) total_bytes.QuadPart;
			entry.free_bytes  = (uint64_t) free_bytes.QuadPart;
		}

		if( disk_enum_result_append( out, &entry ) != 1 )
		{
			disk_enum_result_free( out );
			return -1;
		}
	}

	return 1;
}

void disk_enum_result_free(
      disk_enum_result_t *result )
{
	if( result == NULL )
	{
		return;
	}
	free( result->entries );
	result->entries  = NULL;
	result->count    = 0;
	result->capacity = 0;
}

static int physical_disk_enum_result_append(
     physical_disk_enum_result_t *result,
     const physical_disk_entry_t *entry )
{
	if( result->count == result->capacity )
	{
		int new_capacity = result->capacity == 0 ? 8 : result->capacity * 2;
		physical_disk_entry_t *new_array = (physical_disk_entry_t *) realloc(
		                                     result->entries,
		                                     (size_t) new_capacity * sizeof( physical_disk_entry_t ) );

		if( new_array == NULL )
		{
			return -1;
		}
		result->entries  = new_array;
		result->capacity = new_capacity;
	}
	result->entries[ result->count ] = *entry;
	result->count++;
	return 1;
}

/* GET_LENGTH_INFORMATION / IOCTL_DISK_GET_LENGTH_INFO 在比较老的 SDK 头文件里可能没有，
 * 跟 libcfile 处理同类问题的方式一样，自己补一份定义，有就用系统的 */
#ifndef IOCTL_DISK_GET_LENGTH_INFO
#define IOCTL_DISK_GET_LENGTH_INFO CTL_CODE( IOCTL_DISK_BASE, 0x0017, METHOD_BUFFERED, FILE_READ_ACCESS )
typedef struct _GET_LENGTH_INFORMATION_LOCAL
{
	LARGE_INTEGER Length;
}
GET_LENGTH_INFORMATION_LOCAL;
#define GET_LENGTH_INFORMATION GET_LENGTH_INFORMATION_LOCAL
#endif

static uint64_t get_physical_drive_size(
     HANDLE handle )
{
	GET_LENGTH_INFORMATION length_information;
	DWORD bytes_returned = 0;

	memset( &length_information, 0, sizeof( length_information ) );

	if( DeviceIoControl(
	     handle,
	     IOCTL_DISK_GET_LENGTH_INFO,
	     NULL,
	     0,
	     &length_information,
	     sizeof( length_information ),
	     &bytes_returned,
	     NULL ) )
	{
		return (uint64_t) length_information.Length.QuadPart;
	}
	return 0;
}

/* STORAGE_PROPERTY_QUERY / STORAGE_DEVICE_DESCRIPTOR 拿型号和"是否可移动介质"，
 * 结构定义在 <winioctl.h>，但为了避免不同 SDK 版本字段差异，这里只用到
 * 文档里最稳定的那几个字段偏移，走变长 buffer 的方式读取。 */
static void get_physical_drive_properties(
     HANDLE handle,
     int *out_is_removable,
     char *out_model,
     size_t out_model_size )
{
	STORAGE_PROPERTY_QUERY query;
	BYTE buffer[ 1024 ];
	DWORD bytes_returned = 0;
	PSTORAGE_DEVICE_DESCRIPTOR descriptor;

	*out_is_removable = -1;
	out_model[ 0 ]     = '\0';

	memset( &query, 0, sizeof( query ) );
	query.PropertyId = StorageDeviceProperty;
	query.QueryType  = PropertyStandardQuery;

	memset( buffer, 0, sizeof( buffer ) );

	if( !DeviceIoControl(
	      handle,
	      IOCTL_STORAGE_QUERY_PROPERTY,
	      &query,
	      sizeof( query ),
	      buffer,
	      sizeof( buffer ),
	      &bytes_returned,
	      NULL ) )
	{
		return;
	}

	descriptor = (PSTORAGE_DEVICE_DESCRIPTOR) buffer;
	*out_is_removable = descriptor->RemovableMedia ? 1 : 0;

	if( descriptor->ProductIdOffset != 0 )
	{
		const char *product_id = (const char *) buffer + descriptor->ProductIdOffset;
		strncpy( out_model, product_id, out_model_size - 1 );
	}
}

int disk_enumerator_list_physical_drives(
     physical_disk_enum_result_t *out )
{
	int disk_number = 0;
	int consecutive_misses = 0;

	if( out == NULL )
	{
		return -1;
	}
	out->entries  = NULL;
	out->count    = 0;
	out->capacity = 0;

	/* 物理磁盘编号是连续分配的，连续 4 次打不开就认为后面不会再有了，
	 * 这个阈值留了一点余量，不是严格意义上的"第一次失败就停" */
	for( disk_number = 0; disk_number < 64 && consecutive_misses < 4; disk_number++ )
	{
		char device_path[ 32 ];
		HANDLE handle;
		physical_disk_entry_t entry;

		snprintf( device_path, sizeof( device_path ), "\\\\.\\PhysicalDrive%d", disk_number );

		handle = CreateFileA(
		          device_path,
		          GENERIC_READ, /* IOCTL_DISK_GET_LENGTH_INFO 实测需要至少 GENERIC_READ，
		                         * 单纯 0 访问权限拿不到容量（型号/可移动标志倒是不需要） */
		          FILE_SHARE_READ | FILE_SHARE_WRITE,
		          NULL,
		          OPEN_EXISTING,
		          0,
		          NULL );

		if( handle == INVALID_HANDLE_VALUE )
		{
			consecutive_misses++;
			continue;
		}
		consecutive_misses = 0;

		memset( &entry, 0, sizeof( entry ) );
		entry.disk_number = disk_number;
		entry.size_bytes   = get_physical_drive_size( handle );
		get_physical_drive_properties( handle, &entry.is_removable, entry.model, sizeof( entry.model ) );

		CloseHandle( handle );

		if( physical_disk_enum_result_append( out, &entry ) != 1 )
		{
			physical_disk_enum_result_free( out );
			return -1;
		}
	}

	return 1;
}

void physical_disk_enum_result_free(
      physical_disk_enum_result_t *result )
{
	if( result == NULL )
	{
		return;
	}
	free( result->entries );
	result->entries  = NULL;
	result->count    = 0;
	result->capacity = 0;
}
