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
	uint64_t base_offset;  /* 调用方眼里的"逻辑偏移0"对应真实设备上的
	                         * 物理偏移——见头文件注释完整说明 */
	uint32_t max_retries;
	bad_sector_log_t *out_bad_sector_log;
	int64_t current_offset;  /* 跟踪当前*逻辑*读写位置（不含base_offset），
	                           * 一是为了让坏道日志里的偏移是有意义的
	                           * 逻辑卷内偏移，二是seek_offset靠它支持
	                           * SEEK_CUR；真实物理偏移=base_offset+这个值 */
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
	/* B9真机验证第二轮（2026-11-05小树裁决）之后，这里不再预先seek到
	 * base_offset——裸物理盘设备对象的seek起点本身也必须扇区对齐，这个
	 * 对齐工作只有紧邻每次真正读取之前现算才可靠（见
	 * ntfs_bfio_read_adapter），open这里提前seek到一个未经对齐处理的
	 * base_offset不仅没用，万一base_offset本身不是扇区整数倍还会直接
	 * 打开失败。 */
	context->current_offset = 0;
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
 * exfat_reader.c 里的同名适配器形状一致，但多了一步 B9真机验证第二轮
 * （2026-11-05小树裁决）新增的"现场定位"：裸物理盘设备对象
 * （`\\.\PhysicalDriveN`）连seek的*起点*本身都必须扇区对齐，不像
 * `\\.\<盘符>:`卷设备对象那样有卷管理器的缓存垫片兜底——所以每次被
 * 调用（包括 ntfs_bfio_read_core 坏道重试触发的每一次重试）都现场用
 * `base_offset+current_offset`算出物理目标、向下取整到扇区边界、
 * 现场seek，不依赖"之前某次seek_offset调用在真实设备上留下的游标
 * 位置"这个假设——这个函数自己完全自给自足。 */
static size_t ntfs_bfio_read_adapter( void *context_ptr, void *buffer, size_t size )
{
	ntfs_bfio_context_t *context = (ntfs_bfio_context_t *) context_ptr;
	uint64_t physical_target;
	uint64_t aligned_physical_target;
	size_t lead_in;

	physical_target = context->base_offset + (uint64_t) context->current_offset;
	sector_aligned_read_compute_aligned_seek(
	 physical_target, NTFS_BFIO_SECTOR_SIZE_ASSUMED, &aligned_physical_target, &lead_in );

#ifdef _WIN32
	{
		LARGE_INTEGER move_to;
		LARGE_INTEGER new_position;
		DWORD actually_read = 0;

		move_to.QuadPart = (LONGLONG) aligned_physical_target;
		if( !SetFilePointerEx( context->file, move_to, &new_position, FILE_BEGIN ) )
		{
			return 0;
		}
		if( !ReadFile( context->file, buffer, (DWORD) size, &actually_read, NULL ) )
		{
			return 0;
		}
		return (size_t) actually_read;
	}
#else
	if( fseek( context->file, (long) aligned_physical_target, SEEK_SET ) != 0 )
	{
		return 0;
	}
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
/* NTFS_BFIO_ALIGNED_BUFFER_SIZE 现在定义在 ntfs_bfio_adapter.h 里（原本
 * 65536 这个数字背后的理由——覆盖 MFT 记录/引导扇区这类小请求——已经
 * 不再成立：2026-11-07 真机测试证实 libfsntfs 对非驻留属性的单次读取
 * 请求经常远超过这个数字，这个常量现在的真实含义是"ntfs_bfio_read_core
 * 单次调用的硬性上限"，超限的大请求要由调用方（ntfs_bfio_read/
 * vshadow_bfio_read）自己拆成多块，不是这个常量本身需要调大，详见头
 * 文件和 ntfs_bfio_read 的实现注释 */

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
     size_t lead_in,
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
	/* 请求长度（含lead_in垫出来的部分）超过内部对齐缓冲区能装的范围：
	 * 一律返回-1，不管lead_in是不是0——2026-11-07真机测试一个60MB跨多个
	 * data run的NTFS已删除大文件时发现，"lead_in==0时退化成直接读取、
	 * 跳过重试/坏道容忍"这条旧的兜底分支（2026-11-05 B9真机验证第二轮
	 * 裁决时加的）是一个真实的静默数据损坏缺口：libfsntfs对非驻留属性的
	 * 单次data run往往一次性发起远超过本函数内部对齐缓冲区（64KB）的
	 * 读取请求（这次实测里哪怕是"看起来不碎片化"的30MB连续文件，NTFS
	 * 卷分配器本身也天然把它切成了多个几百KB到5MB量级的data
	 * run——"够大就会跨多个run"是正常情况，不是刻意构造的极端场景），
	 * 走旧的bypass分支之后恢复出的文件内容全部变成0字节（大小仍然
	 * 正确，`bad_sector_log`里也不会留下任何记录，表面上看是"干净成功"，
	 * 实际内容整个报废）。调用方必须把请求拆成不超过本函数内部缓冲区
	 * 容量的若干块分别调用——这件事现在由`ntfs_bfio_read()`/
	 * `vshadow_bfio_read()`这两个外层"命令式外壳"负责（它们知道
	 * `current_offset`要在块与块之间怎么推进），这个纯函数本身不再
	 * 提供"直接旁路、不管对不对"这条退路，宁可对越界请求失败得响亮，
	 * 也不要悄悄返回一段可能是错的内容却不留任何坏道痕迹。 */
	if( ( lead_in + size ) > sizeof( aligned_buffer ) )
	{
		return -1;
	}

	aligned_requested = sector_aligned_read_compute_request_size(
	                      lead_in + size, NTFS_BFIO_SECTOR_SIZE_ASSUMED, sizeof( aligned_buffer ) );

	attempt_number = 1;
	while( 1 )
	{
		size_t actually_read = read_fn( read_context, aligned_buffer, aligned_requested );

		usable = sector_aligned_read_usable_bytes_with_lead_in( lead_in, size, actually_read );
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

	memcpy( buffer, aligned_buffer + lead_in, usable );
	return (ssize_t) usable;
}

/* 真正的读取回调：P5 坏道保护接入点（2026-11-05 小树裁决第20步 N4）。
 * libfsntfs 发起的读取请求偏移/长度大概率不是扇区整数倍（和 FAT32/exFAT
 * 天然扇区对齐的簇读取不同——这是 N2 判断过的真正风险点），交给上面的
 * `ntfs_bfio_read_core` 统一处理，这里只负责注入真实的设备读取函数和
 * 维护 current_offset。
 *
 * 2026-11-07 新增"分块循环"：`ntfs_bfio_read_core` 一次只能处理不超过
 * 它内部对齐缓冲区（`NTFS_BFIO_ALIGNED_BUFFER_SIZE`，64KB）的请求，但
 * libfsntfs 对非驻留属性发起的单次读取请求经常远大于这个数字（真机
 * 测试实测：哪怕是几十MB的"连续"文件，NTFS卷分配器本身也会把它切成
 * 多个几百KB到几MB量级的data run，单个run的读取请求轻松超过64KB）。
 * 这里按`NTFS_BFIO_ALIGNED_BUFFER_SIZE`为上限把一次大请求拆成若干次
 * 对`ntfs_bfio_read_core`的调用，每一块都完整走一遍膨胀对齐+重试+坏道
 * 容忍——不是在这之前（2026-11-05加的那条"lead_in==0时退化成直接读取"
 * 分支）那样让超限请求绕过这整套机制直接摸真实设备，那条分支在真机
 * 测试里被证实会把大文件恢复成内容全部是0字节的样子（大小仍然正确，
 * 坏道日志也不会记录任何异常，表面上是"干净成功"，实际内容整个
 * 报废）。这里的分块循环只负责"怎么把一次大请求拆成多次符合
 * read_core契约的调用"，膨胀对齐/重试/坏道容忍这些算术逻辑本身完全
 * 不变、还是`ntfs_bfio_read_core`那一套已经有mock单元测试覆盖的实现。 */
static ssize_t ntfs_bfio_read(
     intptr_t *io_handle,
     uint8_t *buffer,
     size_t size,
     libfsntfs_error_t **error )
{
	ntfs_bfio_context_t *context = (ntfs_bfio_context_t *) io_handle;
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
		uint64_t physical_target;
		uint64_t aligned_physical_target;
		size_t lead_in;
		size_t chunk_capacity;
		size_t chunk_size;
		ssize_t chunk_result;

		/* 每一块都重新现算lead_in：current_offset在每块读完之后会推进，
		 * 新的物理偏移不保证还和上一块一样的扇区对齐余量 */
		physical_target = context->base_offset + (uint64_t) context->current_offset;
		sector_aligned_read_compute_aligned_seek(
		 physical_target, NTFS_BFIO_SECTOR_SIZE_ASSUMED, &aligned_physical_target, &lead_in );
		(void) aligned_physical_target;

		/* lead_in必然小于NTFS_BFIO_SECTOR_SIZE_ASSUMED（4096），远小于
		 * NTFS_BFIO_ALIGNED_BUFFER_SIZE（65536），这里减法不会下溢 */
		chunk_capacity = NTFS_BFIO_ALIGNED_BUFFER_SIZE - lead_in;
		chunk_size     = remaining < chunk_capacity ? remaining : chunk_capacity;

		chunk_result = ntfs_bfio_read_core(
		                ntfs_bfio_read_adapter, context, output_cursor, chunk_size, lead_in,
		                context->max_retries, (uint64_t) context->current_offset, context->out_bad_sector_log );

		if( chunk_result <= 0 )
		{
			/* 这一块彻底失败：如果前面已经攒到一些字节，按标准的
			 * short-read语义返回已经凑到的部分，不要把之前已经读
			 * 成功的内容也一起扔掉；一个字节都还没读到就直接把
			 * 这次失败原样传上去（可能是0，也可能是-1） */
			return total_read > 0 ? (ssize_t) total_read : chunk_result;
		}

		context->current_offset += (int64_t) chunk_result;
		output_cursor            += (size_t) chunk_result;
		total_read               += (size_t) chunk_result;
		remaining                -= (size_t) chunk_result;

		if( (size_t) chunk_result < chunk_size )
		{
			/* 设备这次给的比这一块请求的少——标准short-read语义，到此
			 * 为止，不强行再转一轮凑够调用方最初要的全部size */
			break;
		}
	}

	return (ssize_t) total_read;
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

/* 纯函数实现，见头文件注释——完全不碰真实I/O、不知道base_offset这件事 */
int ntfs_bfio_seek_offset_core(
     int64_t current_logical_offset,
     int64_t offset,
     int whence,
     int64_t logical_size,
     int64_t *out_new_logical_offset )
{
	int64_t new_offset;

	if( out_new_logical_offset == NULL )
	{
		return -1;
	}
	if( whence == SEEK_SET )
	{
		new_offset = offset;
	}
	else if( whence == SEEK_CUR )
	{
		new_offset = current_logical_offset + offset;
	}
	else if( whence == SEEK_END )
	{
		if( logical_size < 0 )
		{
			return -1;
		}
		new_offset = logical_size + offset;
	}
	else
	{
		return -1;
	}
	if( new_offset < 0 )
	{
		return -1;
	}
	*out_new_logical_offset = new_offset;
	return 1;
}

/* 查询真实设备的*物理*总大小（不减base_offset，供get_size()和
 * seek_offset()的SEEK_END分支共用，避免重复两份GetFileSizeEx/IOCTL
 * 查询逻辑） */
static int ntfs_bfio_query_physical_size(
     ntfs_bfio_context_t *context,
     int64_t *out_physical_size )
{
	if( context == NULL || out_physical_size == NULL || context->file == NTFS_BFIO_FILE_INVALID )
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
			*out_physical_size = (int64_t) file_size.QuadPart;
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
				*out_physical_size = (int64_t) length_information.Length.QuadPart;
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
		*out_physical_size = (int64_t) end_position;
		return 1;
	}
#endif
}

/* B9真机验证第二轮（2026-11-05小树裁决）之后，这个回调不再碰任何真实
 * 设备——只更新context->current_offset这个纯粹的逻辑偏移bookkeeping，
 * 和`vshadow_bfio_seek_offset`（已经验证过的既有模式）完全一致。真正
 * 对真实设备做"扇区对齐+定位"的工作挪到了`ntfs_bfio_read_adapter`里，
 * 每次真正读取前现场计算、现场seek，不依赖这次seek调用在真实设备上
 * 留下过什么游标状态——libbfio实际发起读取走的是无条件"先seek、再
 * read"（`libbfio_handle_read_buffer_at_offset`，已读源码确认），这个
 * 简化不会丢失任何正确性，却去掉了一份容易悄悄失效的隐式调用顺序假设。 */
static off64_t ntfs_bfio_seek_offset(
     intptr_t *io_handle,
     off64_t offset,
     int whence,
     libfsntfs_error_t **error )
{
	ntfs_bfio_context_t *context = (ntfs_bfio_context_t *) io_handle;
	int64_t logical_size = -1;
	int64_t new_logical_offset;

	(void) error;

	if( context == NULL || context->file == NTFS_BFIO_FILE_INVALID )
	{
		return -1;
	}
	if( whence == SEEK_END )
	{
		int64_t physical_size;

		if( ntfs_bfio_query_physical_size( context, &physical_size ) != 1 )
		{
			return -1;
		}
		logical_size = physical_size - (int64_t) context->base_offset;
		if( logical_size < 0 )
		{
			return -1;
		}
	}
	if( ntfs_bfio_seek_offset_core( context->current_offset, offset, whence, logical_size, &new_logical_offset ) != 1 )
	{
		return -1;
	}
	context->current_offset = new_logical_offset;
	return (off64_t) new_logical_offset;
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
	int64_t physical_size;

	(void) error;

	if( context == NULL || size == NULL || context->file == NTFS_BFIO_FILE_INVALID )
	{
		return -1;
	}
	if( ntfs_bfio_query_physical_size( context, &physical_size ) != 1 )
	{
		return -1;
	}
	/* base_offset>物理大小是一次不合法的构造（调用方传入的偏移超出了
	 * 设备真实范围），防御性拒绝而不是算出一个负数再静默截断 */
	if( (uint64_t) physical_size < context->base_offset )
	{
		return -1;
	}
	*size = (size64_t) ( physical_size - (int64_t) context->base_offset );
	return 1;
}

int ntfs_bfio_adapter_initialize(
     const char *volume_path,
     uint64_t base_offset,
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
	context->base_offset        = base_offset;
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
