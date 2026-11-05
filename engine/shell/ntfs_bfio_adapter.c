#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "ntfs_bfio_adapter.h"
#include "aligned_device_reader.h"
#include "../core/bad_sector_policy.h"
#include "../core/sector_aligned_read.h"

/* 和 cli/main.c 里 PARTITION_COPY_ASSUMED_SECTOR_SIZE 同样的理由：保守假设
 * 4096（覆盖常见的 512 逻辑扇区和 4K Advanced Format 物理扇区，4096 的
 * 倍数必然也是 512 的倍数），不去查真实扇区大小——这里连自己造的
 * boot_sector 都没有，没有现成的"真实扇区大小"可查，4096 是已经在这个
 * 项目里复用过的保守选择 */
#define NTFS_BFIO_SECTOR_SIZE_ASSUMED 4096

#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>

/* 和 disk_enumerator.c 里同样的理由：比较老的 SDK 头文件可能没有这个定义，
 * 有就用系统的，没有就自己补一份 */
#ifndef IOCTL_DISK_GET_LENGTH_INFO
#define IOCTL_DISK_GET_LENGTH_INFO CTL_CODE( IOCTL_DISK_BASE, 0x0017, METHOD_BUFFERED, FILE_READ_ACCESS )
typedef struct _GET_LENGTH_INFORMATION_LOCAL
{
	LARGE_INTEGER Length;
}
GET_LENGTH_INFORMATION_LOCAL;
#define GET_LENGTH_INFORMATION GET_LENGTH_INFORMATION_LOCAL
#endif
#endif

/* 手写声明 libbfio_handle_initialize 和两个 flag 常量——理由和
 * ntfs_bfio_adapter.h 文件头注释一样，不依赖 vendored 的 libbfio 内部
 * 头文件，只在这个 .c 文件内部使用，不需要暴露给调用方 */
#define NTFS_BFIO_FLAG_IO_HANDLE_MANAGED          0x01
#define NTFS_BFIO_FLAG_IO_HANDLE_CLONE_BY_REFERENCE 0x02

extern int libbfio_handle_initialize(
     libbfio_handle_t **handle,
     intptr_t *io_handle,
     int (*free_io_handle)( intptr_t **io_handle, libfsntfs_error_t **error ),
     int (*clone_io_handle)( intptr_t **destination_io_handle, intptr_t *source_io_handle, libfsntfs_error_t **error ),
     int (*open)( intptr_t *io_handle, int access_flags, libfsntfs_error_t **error ),
     int (*close)( intptr_t *io_handle, libfsntfs_error_t **error ),
     ssize_t (*read)( intptr_t *io_handle, uint8_t *buffer, size_t size, libfsntfs_error_t **error ),
     ssize_t (*write)( intptr_t *io_handle, const uint8_t *buffer, size_t size, libfsntfs_error_t **error ),
     off64_t (*seek_offset)( intptr_t *io_handle, off64_t offset, int whence, libfsntfs_error_t **error ),
     int (*exists)( intptr_t *io_handle, libfsntfs_error_t **error ),
     int (*is_open)( intptr_t *io_handle, libfsntfs_error_t **error ),
     int (*get_size)( intptr_t *io_handle, size64_t *size, libfsntfs_error_t **error ),
     uint8_t flags,
     libfsntfs_error_t **error );

/* 上下文结构体：持有真正的设备/文件句柄 + 坏道容忍相关的两个参数。
 * path 在 open 回调里才真正用来打开句柄（不是在 initialize 的时候），
 * 这是故意和 libbfio_file_io_handle 的真实行为对齐——libfsntfs 自己会在
 * 合适的时机调用 open，不是我们应该抢在前面做的事。 */
typedef struct
{
#ifdef _WIN32
	HANDLE file;
#else
	FILE *file;
#endif
	char *path;
	uint32_t max_retries;
	bad_sector_log_t *out_bad_sector_log;
	int64_t current_offset;  /* 跟踪当前读写位置，只是为了让坏道日志里的
	                           * 偏移是有意义的绝对卷内偏移，不是占位的 0；
	                           * 不依赖这个字段做任何 I/O 决策 */
}
ntfs_bfio_context_t;

#ifdef _WIN32
#define NTFS_BFIO_FILE_INVALID NULL
#else
#define NTFS_BFIO_FILE_INVALID NULL
#endif

static int ntfs_bfio_free_io_handle(
     intptr_t **io_handle,
     libfsntfs_error_t **error )
{
	ntfs_bfio_context_t *context;

	(void) error;

	if( io_handle == NULL || *io_handle == NULL )
	{
		return 1;
	}
	context = (ntfs_bfio_context_t *) ( *io_handle );

	if( context->file != NTFS_BFIO_FILE_INVALID )
	{
#ifdef _WIN32
		CloseHandle( context->file );
#else
		fclose( context->file );
#endif
	}
	free( context->path );
	free( context );
	*io_handle = NULL;

	return 1;
}

/* 不期望被真正调用到——构造句柄时用 CLONE_BY_REFERENCE 这个 flag，
 * libbfio 内部克隆时会直接共享同一个 io_handle 指针，不会调用这个回调。
 * 留一个安全的实现只是防御性的，万一哪天行为假设变了也不会崩溃。 */
static int ntfs_bfio_clone_io_handle(
     intptr_t **destination_io_handle,
     intptr_t *source_io_handle,
     libfsntfs_error_t **error )
{
	(void) error;

	if( destination_io_handle == NULL )
	{
		return -1;
	}
	*destination_io_handle = source_io_handle;
	return 1;
}

static int ntfs_bfio_open(
     intptr_t *io_handle,
     int access_flags,
     libfsntfs_error_t **error )
{
	ntfs_bfio_context_t *context = (ntfs_bfio_context_t *) io_handle;

	(void) access_flags;
	(void) error;

	if( context == NULL )
	{
		return -1;
	}
	if( context->file != NTFS_BFIO_FILE_INVALID )
	{
		return 1;
	}

#ifdef _WIN32
	/* 和 fat_reader.c/exfat_reader.c 同样的理由：真实的
	 * \\.\PhysicalDriveN / \\.\<盘符>: 是裸设备，CRT 的 fopen 在这种路径上
	 * 打不开或读不到东西，必须用 CreateFileA+ReadFile */
	context->file = CreateFileA(
	                 context->path,
	                 GENERIC_READ,
	                 FILE_SHARE_READ | FILE_SHARE_WRITE,
	                 NULL,
	                 OPEN_EXISTING,
	                 0,
	                 NULL );
	if( context->file == INVALID_HANDLE_VALUE )
	{
		context->file = NTFS_BFIO_FILE_INVALID;
		return -1;
	}
#else
	context->file = fopen( context->path, "rb" );
	if( context->file == NULL )
	{
		return -1;
	}
#endif
	return 1;
}

static int ntfs_bfio_close(
     intptr_t *io_handle,
     libfsntfs_error_t **error )
{
	ntfs_bfio_context_t *context = (ntfs_bfio_context_t *) io_handle;

	(void) error;

	if( context == NULL )
	{
		return -1;
	}
	if( context->file != NTFS_BFIO_FILE_INVALID )
	{
#ifdef _WIN32
		CloseHandle( context->file );
#else
		fclose( context->file );
#endif
		context->file = NTFS_BFIO_FILE_INVALID;
	}
	return 0;
}

/* 把 context->file 包装成 aligned_device_reader_* 系列函数需要的
 * (context, buffer, size) -> actually_read 形状，和 fat_reader.c/
 * exfat_reader.c 里的同名适配器一模一样 */
static size_t ntfs_bfio_read_adapter( void *context_ptr, void *buffer, size_t size )
{
	ntfs_bfio_context_t *context = (ntfs_bfio_context_t *) context_ptr;

#ifdef _WIN32
	DWORD actually_read = 0;

	if( !ReadFile( context->file, buffer, (DWORD) size, &actually_read, NULL ) )
	{
		return 0;
	}
	return (size_t) actually_read;
#else
	return fread( buffer, 1, size, context->file );
#endif
}

/* 真正的读取回调：P5 坏道保护接入点（2026-11-05 小树裁决第20步 N4）。
 * libfsntfs 发起的读取请求偏移/长度大概率不是扇区整数倍（和 FAT32/exFAT
 * 天然扇区对齐的簇读取不同——这是 N2 判断过的真正风险点），所以这里不能
 * 直接假设 size 本身可以拿去对齐重试：裸设备在非对齐长度下读取会直接
 * 返回 0（真机已验证过的行为），重试同样长度毫无意义。正确做法是复用
 * 已经验证过的 `sector_aligned_read_compute_request_size` 把请求长度
 * 膨胀到扇区边界，读到一块临时缓冲区，再裁剪出调用方真正要的部分——
 * 这正是 aligned_device_reader_copy_to_file 已经在做的事，这里直接复用
 * 同一个函数处理"膨胀+重试+裁剪"，不重新发明一遍。 */
/* 内部对齐缓冲区大小：覆盖 libfsntfs 实测会发起的读取请求（MFT 记录/
 * 引导扇区通常几百字节到几 KB，file_writer.c 给文件内容读取用的缓冲区
 * 本身也是 4096 字节），留出几倍余量，不是精确算出来的下限 */
#define NTFS_BFIO_ALIGNED_BUFFER_SIZE 65536

/* 核心逻辑抽成一个接受注入 read_fn 的函数（2026-11-05 小树裁决第20步
 * N3/N7）：和 aligned_device_reader.c 同样的依赖注入模式，让这段"膨胀
 * 对齐+重试+裁剪+坏道容忍"的算术能在 Mac 本地用内存 mock 做完整 TDD
 * 覆盖，不需要真实 NTFS 卷或真实坏道才能测到。真正的 `ntfs_bfio_read`
 * 回调只是用真实的 `ntfs_bfio_read_adapter` 调用这个函数，自己不包含
 * 任何值得单独测试的逻辑分支。
 *
 * offset_for_log：调用方（真实回调）传入当前的卷内偏移，用于坏道日志；
 * 纯函数本身不关心这个数字的含义，原样记下去。
 *
 * 返回实际可用的字节数（可能是真实数据，也可能是坏道容忍场景下的 0
 * 填充），-1 表示彻底失败。 */
ssize_t ntfs_bfio_read_core(
     aligned_device_read_fn read_fn,
     void *read_context,
     uint8_t *buffer,
     size_t size,
     uint32_t max_retries,
     uint64_t offset_for_log,
     bad_sector_log_t *out_bad_sector_log )
{
	uint8_t aligned_buffer[ NTFS_BFIO_ALIGNED_BUFFER_SIZE ];
	size_t aligned_requested;
	size_t usable;
	uint32_t attempt_number;

	if( read_fn == NULL || buffer == NULL )
	{
		return -1;
	}
	if( size == 0 )
	{
		return 0;
	}
	/* 请求长度超过内部对齐缓冲区能装的范围——目前调用方（libfsntfs 的
	 * 属性数据读取）实测请求都远小于这个值，真的出现这种情况时退化成
	 * 不做坏道容忍的直接读取，不强行拆分成多次读（拆分涉及的 seek 语义
	 * 在这个回调的调用约定下没有被明确保证，不盲目假设） */
	if( size > sizeof( aligned_buffer ) )
	{
		return (ssize_t) read_fn( read_context, buffer, size );
	}

	aligned_requested = sector_aligned_read_compute_request_size(
	                      size, NTFS_BFIO_SECTOR_SIZE_ASSUMED, sizeof( aligned_buffer ) );

	attempt_number = 1;
	while( 1 )
	{
		size_t actually_read = read_fn( read_context, aligned_buffer, aligned_requested );

		usable = sector_aligned_read_usable_bytes( size, actually_read );
		if( usable > 0 )
		{
			break;
		}
		if( bad_sector_policy_decide( attempt_number, max_retries ) == BAD_SECTOR_ACTION_RETRY )
		{
			attempt_number++;
			continue;
		}
		break;
	}

	if( usable == 0 )
	{
		if( out_bad_sector_log == NULL )
		{
			return -1;
		}
		/* 坏道容忍：这里的 0 填充不像 FAT/exFAT 目录数据那样天然有"目录
		 * 结束"这种合法语义——NTFS 卷任意位置的数据被 0 填充，内容本身
		 * 就是错的。这不是新发明一个"0 填充=安全"的结论，只是延续
		 * 第17/18步已经定下的"容忍优于中断，允许这段内容错"这个既有
		 * 产品策略，不需要为 NTFS 重新论证一遍 */
		memset( buffer, 0, size );
		if( bad_sector_log_append( out_bad_sector_log, offset_for_log, (uint64_t) size ) != 1 )
		{
			return -1;
		}
		return (ssize_t) size;
	}

	memcpy( buffer, aligned_buffer, usable );
	return (ssize_t) usable;
}

/* 真正的读取回调：P5 坏道保护接入点（2026-11-05 小树裁决第20步 N4）。
 * libfsntfs 发起的读取请求偏移/长度大概率不是扇区整数倍（和 FAT32/exFAT
 * 天然扇区对齐的簇读取不同——这是 N2 判断过的真正风险点），交给上面的
 * `ntfs_bfio_read_core` 统一处理，这里只负责注入真实的设备读取函数和
 * 维护 current_offset。 */
static ssize_t ntfs_bfio_read(
     intptr_t *io_handle,
     uint8_t *buffer,
     size_t size,
     libfsntfs_error_t **error )
{
	ntfs_bfio_context_t *context = (ntfs_bfio_context_t *) io_handle;
	ssize_t result;

	(void) error;

	if( context == NULL )
	{
		return -1;
	}

	result = ntfs_bfio_read_core(
	          ntfs_bfio_read_adapter, context, buffer, size,
	          context->max_retries, (uint64_t) context->current_offset, context->out_bad_sector_log );

	if( result > 0 )
	{
		context->current_offset += (int64_t) result;
	}
	return result;
}

static ssize_t ntfs_bfio_write(
     intptr_t *io_handle,
     const uint8_t *buffer,
     size_t size,
     libfsntfs_error_t **error )
{
	(void) io_handle;
	(void) buffer;
	(void) size;
	(void) error;

	/* 只读恢复工具，不支持写——明确返回失败而不是假装成功 */
	return -1;
}

static off64_t ntfs_bfio_seek_offset(
     intptr_t *io_handle,
     off64_t offset,
     int whence,
     libfsntfs_error_t **error )
{
	ntfs_bfio_context_t *context = (ntfs_bfio_context_t *) io_handle;

	(void) error;

	if( context == NULL || context->file == NTFS_BFIO_FILE_INVALID )
	{
		return -1;
	}

#ifdef _WIN32
	{
		LARGE_INTEGER move_to;
		LARGE_INTEGER new_position;
		DWORD move_method;

		move_to.QuadPart = offset;
		if( whence == SEEK_SET )
		{
			move_method = FILE_BEGIN;
		}
		else if( whence == SEEK_CUR )
		{
			move_method = FILE_CURRENT;
		}
		else
		{
			move_method = FILE_END;
		}
		if( !SetFilePointerEx( context->file, move_to, &new_position, move_method ) )
		{
			return -1;
		}
		context->current_offset = (int64_t) new_position.QuadPart;
		return (off64_t) new_position.QuadPart;
	}
#else
	{
		long result_position;

		if( fseek( context->file, (long) offset, whence ) != 0 )
		{
			return -1;
		}
		result_position = ftell( context->file );
		context->current_offset = (int64_t) result_position;
		return (off64_t) result_position;
	}
#endif
}

static int ntfs_bfio_exists(
     intptr_t *io_handle,
     libfsntfs_error_t **error )
{
	ntfs_bfio_context_t *context = (ntfs_bfio_context_t *) io_handle;

	(void) error;

	if( context == NULL )
	{
		return -1;
	}
	if( context->file != NTFS_BFIO_FILE_INVALID )
	{
		return 1;
	}
	/* 还没打开过——尝试打开一下马上关掉，只是为了判断"存在"，不改变
	 * 句柄当前的打开状态 */
	if( ntfs_bfio_open( io_handle, 0, NULL ) != 1 )
	{
		return 0;
	}
	ntfs_bfio_close( io_handle, NULL );
	return 1;
}

static int ntfs_bfio_is_open(
     intptr_t *io_handle,
     libfsntfs_error_t **error )
{
	ntfs_bfio_context_t *context = (ntfs_bfio_context_t *) io_handle;

	(void) error;

	if( context == NULL )
	{
		return -1;
	}
	return context->file != NTFS_BFIO_FILE_INVALID ? 1 : 0;
}

static int ntfs_bfio_get_size(
     intptr_t *io_handle,
     size64_t *size,
     libfsntfs_error_t **error )
{
	ntfs_bfio_context_t *context = (ntfs_bfio_context_t *) io_handle;

	(void) error;

	if( context == NULL || size == NULL || context->file == NTFS_BFIO_FILE_INVALID )
	{
		return -1;
	}

#ifdef _WIN32
	{
		LARGE_INTEGER file_size;

		/* 先试最普通的 GetFileSizeEx（对 Mac 侧合成镜像这种普通文件、
		 * 以及某些 Windows 版本上的卷句柄都可能直接管用）；拿不到再退
		 * 化成 disk_enumerator.c 里已经真机验证过的
		 * IOCTL_DISK_GET_LENGTH_INFO（裸设备/卷句柄的正规查询方式） */
		if( GetFileSizeEx( context->file, &file_size ) )
		{
			*size = (size64_t) file_size.QuadPart;
			return 1;
		}

		{
			GET_LENGTH_INFORMATION length_information;
			DWORD bytes_returned = 0;

			memset( &length_information, 0, sizeof( length_information ) );

			if( DeviceIoControl(
			     context->file,
			     IOCTL_DISK_GET_LENGTH_INFO,
			     NULL,
			     0,
			     &length_information,
			     sizeof( length_information ),
			     &bytes_returned,
			     NULL ) )
			{
				*size = (size64_t) length_information.Length.QuadPart;
				return 1;
			}
		}
		return -1;
	}
#else
	{
		long current_position;
		long end_position;

		current_position = ftell( context->file );
		if( fseek( context->file, 0, SEEK_END ) != 0 )
		{
			return -1;
		}
		end_position = ftell( context->file );
		fseek( context->file, current_position, SEEK_SET );

		if( end_position < 0 )
		{
			return -1;
		}
		*size = (size64_t) end_position;
		return 1;
	}
#endif
}

int ntfs_bfio_adapter_initialize(
     const char *volume_path,
     uint32_t max_retries,
     bad_sector_log_t *out_bad_sector_log,
     libbfio_handle_t **out_handle,
     libfsntfs_error_t **error )
{
	ntfs_bfio_context_t *context;

	if( volume_path == NULL || out_handle == NULL )
	{
		return -1;
	}

	context = (ntfs_bfio_context_t *) malloc( sizeof( ntfs_bfio_context_t ) );
	if( context == NULL )
	{
		return -1;
	}
	memset( context, 0, sizeof( ntfs_bfio_context_t ) );
	context->file = NTFS_BFIO_FILE_INVALID;

#ifdef _WIN32
	context->path = _strdup( volume_path );
#else
	context->path = strdup( volume_path );
#endif
	if( context->path == NULL )
	{
		free( context );
		return -1;
	}
	context->max_retries        = max_retries;
	context->out_bad_sector_log = out_bad_sector_log;

	if( libbfio_handle_initialize(
	     out_handle,
	     (intptr_t *) context,
	     ntfs_bfio_free_io_handle,
	     ntfs_bfio_clone_io_handle,
	     ntfs_bfio_open,
	     ntfs_bfio_close,
	     ntfs_bfio_read,
	     ntfs_bfio_write,
	     ntfs_bfio_seek_offset,
	     ntfs_bfio_exists,
	     ntfs_bfio_is_open,
	     ntfs_bfio_get_size,
	     (uint8_t) ( NTFS_BFIO_FLAG_IO_HANDLE_MANAGED | NTFS_BFIO_FLAG_IO_HANDLE_CLONE_BY_REFERENCE ),
	     error ) != 1 )
	{
		free( context->path );
		free( context );
		return -1;
	}

	return 1;
}
