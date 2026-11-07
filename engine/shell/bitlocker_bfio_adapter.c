#include <stdlib.h>
#include <string.h>

#include "bitlocker_bfio_adapter.h"

/* 和 ntfs_bfio_adapter.c / vshadow_bfio_adapter.c 同样的理由：
 * libbfio_handle_initialize 本身不依赖 vendored 的 libbfio 内部头文件，
 * 手写声明，只在这个 .c 文件内部使用 */
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

#define BITLOCKER_BFIO_FLAG_IO_HANDLE_MANAGED          0x01
#define BITLOCKER_BFIO_FLAG_IO_HANDLE_CLONE_BY_REFERENCE 0x02

/* 上下文结构体：持有调用方已经解锁好的 volume（不拥有它的生命周期，
 * 见头文件说明）+ 坏道容忍相关的两个参数 + 当前读写位置（显式自己
 * 维护，每次读取用 libbde_volume_read_buffer_at_offset 显式传入偏移，
 * 和 vshadow_bfio_adapter 同样的理由：不依赖 libbde 内部隐含游标，
 * 不用在意两边游标是否严格对齐）。 */
typedef struct
{
	libbde_volume_t *volume;
	uint32_t max_retries;
	bad_sector_log_t *out_bad_sector_log;
	int64_t current_offset;
}
bitlocker_bfio_context_t;

static int bitlocker_bfio_free_io_handle(
     intptr_t **io_handle,
     libfsntfs_error_t **error )
{
	(void) error;

	if( io_handle == NULL || *io_handle == NULL )
	{
		return 1;
	}
	/* 不释放 context->volume——调用方拥有它的生命周期，见头文件说明 */
	free( *io_handle );
	*io_handle = NULL;

	return 1;
}

static int bitlocker_bfio_clone_io_handle(
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

static int bitlocker_bfio_open(
     intptr_t *io_handle,
     int access_flags,
     libfsntfs_error_t **error )
{
	(void) access_flags;
	(void) error;

	return io_handle != NULL ? 1 : -1;
}

static int bitlocker_bfio_close(
     intptr_t *io_handle,
     libfsntfs_error_t **error )
{
	(void) io_handle;
	(void) error;

	return 0;
}

static size_t bitlocker_bfio_read_adapter( void *context_ptr, void *buffer, size_t size )
{
	bitlocker_bfio_context_t *context = (bitlocker_bfio_context_t *) context_ptr;
	ssize_t result;
	libbde_error_t *bde_error = NULL;

	result = libbde_volume_read_buffer_at_offset(
	          context->volume, buffer, size, (off64_t) context->current_offset, &bde_error );

	if( bde_error != NULL )
	{
		libbde_error_free( &bde_error );
	}
	if( result < 0 )
	{
		return 0;
	}
	return (size_t) result;
}

/* 真正的读取回调：复用 ntfs_bfio_read_core 这个已经有完整 mock 单元
 * 测试覆盖的核心逻辑——和 vshadow_bfio_adapter 同样的理由。B9真机验证
 * 第二轮（2026-11-05小树裁决）给 ntfs_bfio_read_core 加了 lead_in 参数
 * 之后，这里也要跟着多传一个恒为 0 的 lead_in 实参：这个适配器走的是
 * `libbde_volume_read_buffer_at_offset`这种类文件API（解密之后的逻辑
 * 字节流，libbde内部自己处理AES解密的扇区级对齐），从来不是裸物理
 * 设备，不需要对齐。 */
static ssize_t bitlocker_bfio_read(
     intptr_t *io_handle,
     uint8_t *buffer,
     size_t size,
     libfsntfs_error_t **error )
{
	bitlocker_bfio_context_t *context = (bitlocker_bfio_context_t *) io_handle;
	ssize_t result;

	(void) error;

	if( context == NULL )
	{
		return -1;
	}

	result = ntfs_bfio_read_core(
	          bitlocker_bfio_read_adapter, context, buffer, size, 0,
	          context->max_retries, (uint64_t) context->current_offset, context->out_bad_sector_log );

	if( result > 0 )
	{
		context->current_offset += (int64_t) result;
	}
	return result;
}

static ssize_t bitlocker_bfio_write(
     intptr_t *io_handle,
     const uint8_t *buffer,
     size_t size,
     libfsntfs_error_t **error )
{
	(void) io_handle;
	(void) buffer;
	(void) size;
	(void) error;

	/* 只读恢复工具，不支持写 */
	return -1;
}

static off64_t bitlocker_bfio_seek_offset(
     intptr_t *io_handle,
     off64_t offset,
     int whence,
     libfsntfs_error_t **error )
{
	bitlocker_bfio_context_t *context = (bitlocker_bfio_context_t *) io_handle;
	int64_t new_offset;

	(void) error;

	if( context == NULL )
	{
		return -1;
	}

	if( whence == SEEK_SET )
	{
		new_offset = (int64_t) offset;
	}
	else if( whence == SEEK_CUR )
	{
		new_offset = context->current_offset + (int64_t) offset;
	}
	else
	{
		size64_t volume_size = 0;
		libbde_error_t *bde_error = NULL;

		if( libbde_volume_get_size( context->volume, &volume_size, &bde_error ) != 1 )
		{
			if( bde_error != NULL )
			{
				libbde_error_free( &bde_error );
			}
			return -1;
		}
		new_offset = (int64_t) volume_size + (int64_t) offset;
	}

	if( new_offset < 0 )
	{
		return -1;
	}
	context->current_offset = new_offset;
	return (off64_t) new_offset;
}

static int bitlocker_bfio_exists(
     intptr_t *io_handle,
     libfsntfs_error_t **error )
{
	(void) error;

	return io_handle != NULL ? 1 : 0;
}

static int bitlocker_bfio_is_open(
     intptr_t *io_handle,
     libfsntfs_error_t **error )
{
	(void) error;

	return io_handle != NULL ? 1 : 0;
}

static int bitlocker_bfio_get_size(
     intptr_t *io_handle,
     size64_t *size,
     libfsntfs_error_t **error )
{
	bitlocker_bfio_context_t *context = (bitlocker_bfio_context_t *) io_handle;
	libbde_error_t *bde_error = NULL;

	(void) error;

	if( context == NULL || size == NULL )
	{
		return -1;
	}

	if( libbde_volume_get_size( context->volume, size, &bde_error ) != 1 )
	{
		if( bde_error != NULL )
		{
			libbde_error_free( &bde_error );
		}
		return -1;
	}
	return 1;
}

int bitlocker_bfio_adapter_initialize(
     libbde_volume_t *volume,
     uint32_t max_retries,
     bad_sector_log_t *out_bad_sector_log,
     libbfio_handle_t **out_handle,
     libfsntfs_error_t **error )
{
	bitlocker_bfio_context_t *context;

	if( volume == NULL || out_handle == NULL )
	{
		return -1;
	}

	context = (bitlocker_bfio_context_t *) malloc( sizeof( bitlocker_bfio_context_t ) );
	if( context == NULL )
	{
		return -1;
	}
	memset( context, 0, sizeof( bitlocker_bfio_context_t ) );
	context->volume              = volume;
	context->max_retries         = max_retries;
	context->out_bad_sector_log  = out_bad_sector_log;

	if( libbfio_handle_initialize(
	     out_handle,
	     (intptr_t *) context,
	     bitlocker_bfio_free_io_handle,
	     bitlocker_bfio_clone_io_handle,
	     bitlocker_bfio_open,
	     bitlocker_bfio_close,
	     bitlocker_bfio_read,
	     bitlocker_bfio_write,
	     bitlocker_bfio_seek_offset,
	     bitlocker_bfio_exists,
	     bitlocker_bfio_is_open,
	     bitlocker_bfio_get_size,
	     (uint8_t) ( BITLOCKER_BFIO_FLAG_IO_HANDLE_MANAGED | BITLOCKER_BFIO_FLAG_IO_HANDLE_CLONE_BY_REFERENCE ),
	     error ) != 1 )
	{
		free( context );
		return -1;
	}

	return 1;
}
