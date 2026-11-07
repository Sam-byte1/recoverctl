#include <stdlib.h>
#include <string.h>

#include "vshadow_bfio_adapter.h"

/* 和 ntfs_bfio_adapter.c 同样的理由（见该文件头注释）：libbfio_handle_initialize
 * 本身不依赖 vendored 的 libbfio 内部头文件，手写声明，只在这个 .c 文件
 * 内部使用 */
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

#define VSHADOW_BFIO_FLAG_IO_HANDLE_MANAGED          0x01
#define VSHADOW_BFIO_FLAG_IO_HANDLE_CLONE_BY_REFERENCE 0x02

/* 上下文结构体：持有调用方已经打开好的 store（不拥有它的生命周期，见
 * vshadow_bfio_adapter.h 文件头说明）+ 坏道容忍相关的两个参数 + 当前读写
 * 位置（我们自己维护，每次读取都用 libvshadow_store_read_buffer_at_offset
 * 显式传入偏移，不依赖 libvshadow 内部隐含的游标状态，这样不用在意
 * libvshadow 自己的 read_buffer/seek_offset 是不是和我们的调用顺序严格
 * 对应——显式传偏移更不容易出现"两边游标没对齐"这类潜藏的时序问题）。 */
typedef struct
{
	libvshadow_store_t *store;
	uint32_t max_retries;
	bad_sector_log_t *out_bad_sector_log;
	int64_t current_offset;
}
vshadow_bfio_context_t;

static int vshadow_bfio_free_io_handle(
     intptr_t **io_handle,
     libfsntfs_error_t **error )
{
	(void) error;

	if( io_handle == NULL || *io_handle == NULL )
	{
		return 1;
	}
	/* 不释放 context->store——调用方拥有它的生命周期，见头文件说明 */
	free( *io_handle );
	*io_handle = NULL;

	return 1;
}

/* 和 ntfs_bfio_clone_io_handle 同样的理由：构造句柄时用了
 * CLONE_BY_REFERENCE，libbfio 内部克隆会直接共享同一个 io_handle 指针，
 * 不会真正调用这个回调，留一个安全实现纯粹是防御性的 */
static int vshadow_bfio_clone_io_handle(
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

/* store 由调用方在构造这个适配器之前就已经打开好了（见头文件说明），
 * 这里不需要真的做任何"打开"动作，直接报告成功 */
static int vshadow_bfio_open(
     intptr_t *io_handle,
     int access_flags,
     libfsntfs_error_t **error )
{
	(void) access_flags;
	(void) error;

	return io_handle != NULL ? 1 : -1;
}

/* 同样的理由：不拥有 store 的生命周期，不在这里关闭它，和
 * ntfs_bfio_close 返回 0 表示成功是同一个已验证过的 libbfio 约定 */
static int vshadow_bfio_close(
     intptr_t *io_handle,
     libfsntfs_error_t **error )
{
	(void) io_handle;
	(void) error;

	return 0;
}

/* 把 context->store 包装成 aligned_device_reader_* 系列函数需要的
 * (context, buffer, size) -> actually_read 形状——和 ntfs_bfio_read_adapter
 * 唯一的区别是真正读字节这一步换成了 libvshadow_store_read_buffer_at_offset，
 * 不是 ReadFile/fread */
static size_t vshadow_bfio_read_adapter( void *context_ptr, void *buffer, size_t size )
{
	vshadow_bfio_context_t *context = (vshadow_bfio_context_t *) context_ptr;
	ssize_t result;
	libvshadow_error_t *vshadow_error = NULL;

	result = libvshadow_store_read_buffer_at_offset(
	          context->store, buffer, size, (off64_t) context->current_offset, &vshadow_error );

	if( vshadow_error != NULL )
	{
		libvshadow_error_free( &vshadow_error );
	}
	if( result < 0 )
	{
		return 0;
	}
	return (size_t) result;
}

/* 真正的读取回调：复用 ntfs_bfio_read_core 这个已经有完整 mock 单元测试
 * 覆盖的核心逻辑——膨胀对齐+重试+坏道容忍这套算术只认一个注入的
 * read_fn，完全不关心这个 read_fn 内部到底是读物理设备还是读
 * libvshadow 重建出来的逻辑字节流。B9真机验证第二轮（2026-11-05小树
 * 裁决）给 ntfs_bfio_read_core 加了 lead_in 参数之后，这里唯一要跟着
 * 改的地方就是多传一个恒为 0 的 lead_in 实参——不是"一行不改"了，但
 * 也只多这一个数字：libvshadow 走 `libvshadow_store_read_buffer_at_offset`
 * 这种类文件 API，COW 重映射在库内部完成，从来不是裸物理设备，不需要
 * 对齐，lead_in 永远是 0。
 *
 * 2026-11-07 新增"分块循环"：和 ntfs_bfio_adapter.c 里 `ntfs_bfio_read`
 * 同一个真机测试发现的同一个缺口——libfsntfs 对非驻留属性的单次读取
 * 请求可能超过 `ntfs_bfio_read_core` 单次能处理的上限
 * （`NTFS_BFIO_ALIGNED_BUFFER_SIZE`），旧代码里 `ntfs_bfio_read_core` 对
 * 超限请求会在 lead_in==0（这条路径永远是0）时直接绕过重试/坏道容忍，
 * 直接把整个请求转交给 `read_fn`——这正是真机测试里让大文件内容读成
 * 全0字节的那个缺口，VSS离线这条路径和直接NTFS路径共用同一个
 * `ntfs_bfio_read_core`，必须同样修。这里按
 * `NTFS_BFIO_ALIGNED_BUFFER_SIZE` 为上限分块，lead_in 恒为0所以每块
 * 容量恒为这个常量本身，比 ntfs_bfio_adapter.c 那边（lead_in 可能非0）
 * 简单。 */
static ssize_t vshadow_bfio_read(
     intptr_t *io_handle,
     uint8_t *buffer,
     size_t size,
     libfsntfs_error_t **error )
{
	vshadow_bfio_context_t *context = (vshadow_bfio_context_t *) io_handle;
	uint8_t *output_cursor = buffer;
	size_t remaining = size;
	size_t total_read = 0;

	(void) error;

	if( context == NULL )
	{
		return -1;
	}

	while( remaining > 0 )
	{
		size_t chunk_size = remaining < NTFS_BFIO_ALIGNED_BUFFER_SIZE ? remaining : NTFS_BFIO_ALIGNED_BUFFER_SIZE;
		ssize_t chunk_result;

		chunk_result = ntfs_bfio_read_core(
		                vshadow_bfio_read_adapter, context, output_cursor, chunk_size, 0,
		                context->max_retries, (uint64_t) context->current_offset, context->out_bad_sector_log );

		if( chunk_result <= 0 )
		{
			return total_read > 0 ? (ssize_t) total_read : chunk_result;
		}

		context->current_offset += (int64_t) chunk_result;
		output_cursor            += (size_t) chunk_result;
		total_read               += (size_t) chunk_result;
		remaining                -= (size_t) chunk_result;

		if( (size_t) chunk_result < chunk_size )
		{
			break;
		}
	}

	return (ssize_t) total_read;
}

static ssize_t vshadow_bfio_write(
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

/* 显式偏移读取（见 vshadow_bfio_read_adapter 的说明）意味着这里只需要
 * 更新我们自己维护的 current_offset，不需要真的调用
 * libvshadow_store_seek_offset 去同步 libvshadow 内部游标 */
static off64_t vshadow_bfio_seek_offset(
     intptr_t *io_handle,
     off64_t offset,
     int whence,
     libfsntfs_error_t **error )
{
	vshadow_bfio_context_t *context = (vshadow_bfio_context_t *) io_handle;
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
		size64_t store_size = 0;
		libvshadow_error_t *vshadow_error = NULL;

		if( libvshadow_store_get_size( context->store, &store_size, &vshadow_error ) != 1 )
		{
			if( vshadow_error != NULL )
			{
				libvshadow_error_free( &vshadow_error );
			}
			return -1;
		}
		new_offset = (int64_t) store_size + (int64_t) offset;
	}

	if( new_offset < 0 )
	{
		return -1;
	}
	context->current_offset = new_offset;
	return (off64_t) new_offset;
}

/* store 由调用方管理生命周期，只要这个适配器拿到了它，就认为"存在/已打开" */
static int vshadow_bfio_exists(
     intptr_t *io_handle,
     libfsntfs_error_t **error )
{
	(void) error;

	return io_handle != NULL ? 1 : 0;
}

static int vshadow_bfio_is_open(
     intptr_t *io_handle,
     libfsntfs_error_t **error )
{
	(void) error;

	return io_handle != NULL ? 1 : 0;
}

static int vshadow_bfio_get_size(
     intptr_t *io_handle,
     size64_t *size,
     libfsntfs_error_t **error )
{
	vshadow_bfio_context_t *context = (vshadow_bfio_context_t *) io_handle;
	libvshadow_error_t *vshadow_error = NULL;

	(void) error;

	if( context == NULL || size == NULL )
	{
		return -1;
	}

	if( libvshadow_store_get_size( context->store, size, &vshadow_error ) != 1 )
	{
		if( vshadow_error != NULL )
		{
			libvshadow_error_free( &vshadow_error );
		}
		return -1;
	}
	return 1;
}

int vshadow_bfio_adapter_initialize(
     libvshadow_store_t *store,
     uint32_t max_retries,
     bad_sector_log_t *out_bad_sector_log,
     libbfio_handle_t **out_handle,
     libfsntfs_error_t **error )
{
	vshadow_bfio_context_t *context;

	if( store == NULL || out_handle == NULL )
	{
		return -1;
	}

	context = (vshadow_bfio_context_t *) malloc( sizeof( vshadow_bfio_context_t ) );
	if( context == NULL )
	{
		return -1;
	}
	memset( context, 0, sizeof( vshadow_bfio_context_t ) );
	context->store               = store;
	context->max_retries         = max_retries;
	context->out_bad_sector_log  = out_bad_sector_log;

	if( libbfio_handle_initialize(
	     out_handle,
	     (intptr_t *) context,
	     vshadow_bfio_free_io_handle,
	     vshadow_bfio_clone_io_handle,
	     vshadow_bfio_open,
	     vshadow_bfio_close,
	     vshadow_bfio_read,
	     vshadow_bfio_write,
	     vshadow_bfio_seek_offset,
	     vshadow_bfio_exists,
	     vshadow_bfio_is_open,
	     vshadow_bfio_get_size,
	     (uint8_t) ( VSHADOW_BFIO_FLAG_IO_HANDLE_MANAGED | VSHADOW_BFIO_FLAG_IO_HANDLE_CLONE_BY_REFERENCE ),
	     error ) != 1 )
	{
		free( context );
		return -1;
	}

	return 1;
}
