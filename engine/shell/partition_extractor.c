#include "partition_extractor.h"

#include <stdio.h>
#include <string.h>

#include "aligned_device_reader.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <stdlib.h>
#endif

#define PARTITION_TEMP_FILE_NAME "recoverctl_partition_extract.tmp"

int partition_extractor_get_temp_file_path( char *out_path, size_t out_path_size )
{
#ifdef _WIN32
	char temp_dir[ MAX_PATH ];
	DWORD temp_dir_length = GetTempPathA( (DWORD) sizeof( temp_dir ), temp_dir );

	if( ( temp_dir_length == 0 ) || ( temp_dir_length > sizeof( temp_dir ) ) )
	{
		return -1;
	}
	if( snprintf( out_path, out_path_size, "%s%s", temp_dir, PARTITION_TEMP_FILE_NAME ) >= (int) out_path_size )
	{
		return -1;
	}
#else
	if( snprintf( out_path, out_path_size, "/tmp/%s", PARTITION_TEMP_FILE_NAME ) >= (int) out_path_size )
	{
		return -1;
	}
#endif
	return 1;
}

/* 这个函数之前在Windows/非Windows两个分支里各自维护了一份完整的
 * "打开+定位+循环拷贝"实现，现在都调用共享的
 * `aligned_device_reader_copy_to_file`，用一个保守的扇区大小假设
 * （4096，覆盖常见的512逻辑扇区和4K Advanced Format物理扇区）做对齐。
 * 平台相关的部分收窄到只剩"怎么打开/定位一个裸设备句柄"。 */
#define PARTITION_COPY_ASSUMED_SECTOR_SIZE 4096
#define PARTITION_COPY_MAX_READ_RETRIES 3

#ifdef _WIN32

/* 真实的 \\.\PhysicalDriveN / \\.\<盘符>: 路径是裸设备，不是普通文件——
 * 用CRT的fopen/fseek/fread实测在这种路径上要么打不开、要么读到一半
 * 就不出声地失败。disk_enumerator.c已经验证过CreateFileA+GENERIC_READ+
 * FILE_SHARE_READ/WRITE这套组合能稳定打开裸设备，这里照搬同样的打开
 * 方式，只是换成用ReadFile顺序读。 */
static size_t partition_copy_read_adapter( void *context, void *buffer, size_t size )
{
	HANDLE input = (HANDLE) context;
	DWORD actually_read = 0;

	if( !ReadFile( input, buffer, (DWORD) size, &actually_read, NULL ) )
	{
		return 0;
	}
	return (size_t) actually_read;
}

int partition_extractor_extract_to_temp_file(
     const char *device_path,
     long offset,
     uint64_t size,
     bad_sector_log_t *out_bad_sector_log,
     const char *temp_path )
{
	HANDLE input;
	LARGE_INTEGER move_to;
	int return_code;

	input = CreateFileA(
	         device_path,
	         GENERIC_READ,
	         FILE_SHARE_READ | FILE_SHARE_WRITE,
	         NULL,
	         OPEN_EXISTING,
	         0,
	         NULL );
	if( input == INVALID_HANDLE_VALUE )
	{
		return -1;
	}

	move_to.QuadPart = offset;
	if( !SetFilePointerEx( input, move_to, NULL, FILE_BEGIN ) )
	{
		CloseHandle( input );
		return -1;
	}

	return_code = aligned_device_reader_copy_to_file(
	               partition_copy_read_adapter, (void *) input,
	               PARTITION_COPY_ASSUMED_SECTOR_SIZE, size,
	               out_bad_sector_log != NULL ? PARTITION_COPY_MAX_READ_RETRIES : 0,
	               out_bad_sector_log, temp_path );

	CloseHandle( input );
	return return_code;
}

#else

/* Mac开发机上这个函数只会对着我们自己手造的合成镜像文件（普通文件）
 * 测试，不涉及真实裸设备，不需要扇区对齐——传sector_size=0即可。 */
static size_t partition_copy_read_adapter( void *context, void *buffer, size_t size )
{
	FILE *input = (FILE *) context;

	return fread( buffer, 1, size, input );
}

int partition_extractor_extract_to_temp_file(
     const char *device_path,
     long offset,
     uint64_t size,
     bad_sector_log_t *out_bad_sector_log,
     const char *temp_path )
{
	FILE *input;
	int return_code;

	input = fopen( device_path, "rb" );
	if( input == NULL )
	{
		return -1;
	}
	if( fseek( input, offset, SEEK_SET ) != 0 )
	{
		fclose( input );
		return -1;
	}

	return_code = aligned_device_reader_copy_to_file(
	               partition_copy_read_adapter, input, 0, size,
	               out_bad_sector_log != NULL ? PARTITION_COPY_MAX_READ_RETRIES : 0,
	               out_bad_sector_log, temp_path );

	fclose( input );
	return return_code;
}

#endif /* _WIN32 */
