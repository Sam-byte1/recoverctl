#include <stdlib.h>
#include <string.h>

#include "aligned_device_reader.h"
#include "exfat_reader.h"
#include "../core/exfat_fat_chain.h"
#include "../core/path_builder.h"

/* 和 fat_reader.c 的同名常量同样的理由：递归扫描子目录时的最大深度兜底，
 * 防御损坏数据/理论上的环形引用，不是真实场景会触发的限制。 */
#define EXFAT_MAX_RECURSION_DEPTH 16

/* 和 fat_reader.c 里 FAT_READER_MAX_READ_RETRIES 同样的理由，不是实测出来
 * 的"最优值"，是一个保守够用的默认值（2026-11-05 小树裁决第18步）。*/
#define EXFAT_READER_MAX_READ_RETRIES 3

/* 目录簇链遍历的防御性上限（2026-11-05 第21步，"目录单簇假设"修复）：
 * 和 FAT_MAX_RECURSION_DEPTH 同样的设计哲学，不是真实场景的硬限制，
 * 纯粹防止损坏数据/理论上的环形引用导致无限循环或超大内存分配。 */
#define EXFAT_READER_MAX_DIRECTORY_CLUSTERS 4096

static uint8_t *exfat_reader_read_fat_table(
     exfat_reader_t *reader,
     size_t *out_fat_table_size );

/* 和 fat_reader.c 里的 reader_io_* 完全一样的封装，这里不重复注释理由，
 * 直接照搬 FAT32 那一轮已经验证过的实现（包括下面读取时的扇区对齐处理，
 * 也是从 FAT32 真实设备那次踩坑里直接学到的，这里从一开始就应用，不用
 * 再独立踩一次）。 */
#ifdef _WIN32

static int reader_io_open( const char *path, HANDLE *out_handle )
{
	HANDLE handle = CreateFileA(
	                 path,
	                 GENERIC_READ,
	                 FILE_SHARE_READ | FILE_SHARE_WRITE,
	                 NULL,
	                 OPEN_EXISTING,
	                 0,
	                 NULL );

	if( handle == INVALID_HANDLE_VALUE )
	{
		return -1;
	}
	*out_handle = handle;
	return 1;
}

static int reader_io_seek( HANDLE handle, long offset )
{
	LARGE_INTEGER move_to;

	move_to.QuadPart = offset;
	if( !SetFilePointerEx( handle, move_to, NULL, FILE_BEGIN ) )
	{
		return -1;
	}
	return 1;
}

static size_t reader_io_read( HANDLE handle, void *buffer, size_t size )
{
	DWORD actually_read = 0;

	if( !ReadFile( handle, buffer, (DWORD) size, &actually_read, NULL ) )
	{
		return 0;
	}
	return (size_t) actually_read;
}

static void reader_io_close( HANDLE handle )
{
	CloseHandle( handle );
}

#define READER_IO_INVALID NULL

#else

static int reader_io_open( const char *path, FILE **out_handle )
{
	FILE *handle = fopen( path, "rb" );

	if( handle == NULL )
	{
		return -1;
	}
	*out_handle = handle;
	return 1;
}

static int reader_io_seek( FILE *handle, long offset )
{
	if( fseek( handle, offset, SEEK_SET ) != 0 )
	{
		return -1;
	}
	return 1;
}

static size_t reader_io_read( FILE *handle, void *buffer, size_t size )
{
	return fread( buffer, 1, size, handle );
}

static void reader_io_close( FILE *handle )
{
	fclose( handle );
}

#define READER_IO_INVALID NULL

#endif

int exfat_reader_open(
     const char *path,
     exfat_reader_t *out )
{
	return exfat_reader_open_at_offset( path, 0, out );
}

int exfat_reader_open_at_offset(
     const char *path,
     long base_offset,
     exfat_reader_t *out )
{
	uint8_t boot_sector_bytes[ 512 ];

	if( path == NULL || out == NULL )
	{
		return -1;
	}
	memset( out, 0, sizeof( exfat_reader_t ) );
	out->base_offset = base_offset;

	if( reader_io_open( path, &out->file ) != 1 )
	{
		out->file = READER_IO_INVALID;
		return -1;
	}

	if( reader_io_seek( out->file, out->base_offset ) != 1
	 || reader_io_read( out->file, boot_sector_bytes, sizeof( boot_sector_bytes ) ) != sizeof( boot_sector_bytes ) )
	{
		reader_io_close( out->file );
		out->file = READER_IO_INVALID;
		return -1;
	}

	if( exfat_boot_sector_parse( boot_sector_bytes, sizeof( boot_sector_bytes ), &out->boot_sector ) != 1
	 || !out->boot_sector.is_valid )
	{
		reader_io_close( out->file );
		out->file = READER_IO_INVALID;
		return -1;
	}

	return 1;
}

void exfat_reader_close(
      exfat_reader_t *reader )
{
	if( reader == NULL || reader->file == READER_IO_INVALID )
	{
		return;
	}
	reader_io_close( reader->file );
	reader->file = READER_IO_INVALID;
}

/* 和 fat_reader.c 里的适配器一样，把 reader->file 包装成
 * aligned_device_reader_* 系列函数需要的形状，真正的扇区对齐/重试处理
 * 逻辑共用同一份实现，不再各自维护一份容易漂移的拷贝 */
static size_t exfat_reader_read_adapter( void *context, void *buffer, size_t size )
{
	exfat_reader_t *reader = (exfat_reader_t *) context;

	return reader_io_read( reader->file, buffer, size );
}

/* 和 fat_reader.c 里的同名辅助函数一样，根目录和子目录读取都是"读某个
 * 簇的完整内容到一块新分配的内存"这个重复动作，抽出来共用。
 *
 * P5 坏道保护（2026-11-05 小树裁决第18步）：和 fat_reader_read_cluster
 * 同样的设计——out_bad_sector_log 非 NULL 时，重试耗尽后用全 0 内容
 * 冒充这个簇的数据（在 exFAT 目录项格式下，全 0 同样会被解析成
 * "目录结束"，等价于把这个（子）目录当空目录处理），并记一笔坏道日志，
 * 而不是返回 NULL 让调用方走一套平行的"跳过"逻辑。 */
static uint8_t *exfat_reader_read_cluster(
     exfat_reader_t *reader,
     uint32_t cluster,
     size_t cluster_size,
     uint32_t max_retries,
     bad_sector_log_t *out_bad_sector_log )
{
	uint32_t sector;
	long byte_offset;
	uint8_t *buffer;

	sector      = exfat_boot_sector_cluster_to_sector( &reader->boot_sector, cluster );
	byte_offset = reader->base_offset + (long) sector * reader->boot_sector.bytes_per_sector;

	buffer = (uint8_t *) malloc( cluster_size );
	if( buffer == NULL )
	{
		return NULL;
	}

	if( reader_io_seek( reader->file, byte_offset ) != 1 )
	{
		free( buffer );
		return NULL;
	}

	if( aligned_device_reader_read_block(
	     exfat_reader_read_adapter, reader, buffer, cluster_size,
	     max_retries, (uint64_t) byte_offset, out_bad_sector_log ) != 1 )
	{
		free( buffer );
		return NULL;
	}

	return buffer;
}

/* 读取一个目录（根目录或子目录）占用的全部簇，按簇链顺序拼接成一段
 * 连续内存返回——exfat_undelete_scanner_scan_directory/find_subdirectories
 * 这两个纯函数只关心"一段目录字节"，不关心这段字节背后是一个簇还是
 * 多个簇拼接而来，所以这里拼完直接喂给它们就行，核心扫描逻辑不用改
 * （2026-11-05 第21步，修复"目录单簇假设"：原来这里只读第一个簇，
 * 目录内容超过一个簇时后面的已删除候选会被静默漏掉）。
 *
 * no_fat_chain 的语义和 exfat_reader_recover_file 完全一样：
 * - 1（连续存储）：不查 FAT 表，直接从 first_cluster 开始读
 *   ceil(declared_size / cluster_size) 个连续簇——declared_size 必须是
 *   调用方已知的准确值（子目录传 StreamExtension 的 DataLength）。
 * - 0（碎片化/根目录默认）：查 FAT 表走簇链，走到 EOF 标记为止，走到
 *   几个簇就是几个簇——不像文件恢复那样要求"精确等于声明的簇数"，
 *   因为目录没有一个预先声明的确切大小可以拿来做一致性校验
 *   （declared_size 这时候被忽略）。根目录没有 StreamExtension、不存在
 *   NoFatChain 这个标志位，永远走这条分支。 */
static uint8_t *exfat_reader_read_directory_clusters(
     exfat_reader_t *reader,
     uint32_t first_cluster,
     int no_fat_chain,
     uint64_t declared_size,
     size_t cluster_size,
     uint32_t max_retries,
     bad_sector_log_t *out_bad_sector_log,
     size_t *out_total_size )
{
	uint8_t *result;
	uint32_t cluster_count;
	uint32_t *chain;
	uint32_t i;

	if( no_fat_chain )
	{
		cluster_count = (uint32_t) ( ( declared_size + cluster_size - 1 ) / cluster_size );
		if( cluster_count == 0 )
		{
			cluster_count = 1;
		}

		chain = (uint32_t *) malloc( (size_t) cluster_count * sizeof( uint32_t ) );
		if( chain == NULL )
		{
			return NULL;
		}
		for( i = 0; i < cluster_count; i++ )
		{
			chain[ i ] = first_cluster + i;
		}
	}
	else
	{
		uint8_t *fat_table;
		size_t fat_table_size;
		exfat_fat_chain_walk_status_t status;

		fat_table = exfat_reader_read_fat_table( reader, &fat_table_size );
		if( fat_table == NULL )
		{
			return NULL;
		}

		chain = (uint32_t *) malloc( (size_t) EXFAT_READER_MAX_DIRECTORY_CLUSTERS * sizeof( uint32_t ) );
		if( chain == NULL )
		{
			free( fat_table );
			return NULL;
		}

		if( exfat_fat_chain_walk(
		     fat_table, fat_table_size, first_cluster,
		     reader->boot_sector.cluster_count, EXFAT_READER_MAX_DIRECTORY_CLUSTERS,
		     chain, &cluster_count, &status ) != 1
		 || status != EXFAT_FAT_CHAIN_WALK_OK )
		{
			free( chain );
			free( fat_table );
			return NULL;
		}
		free( fat_table );
	}

	result = (uint8_t *) malloc( (size_t) cluster_count * cluster_size );
	if( result == NULL )
	{
		free( chain );
		return NULL;
	}

	for( i = 0; i < cluster_count; i++ )
	{
		uint8_t *cluster_buffer = exfat_reader_read_cluster(
		                            reader, chain[ i ], cluster_size, max_retries, out_bad_sector_log );
		if( cluster_buffer == NULL )
		{
			free( result );
			free( chain );
			return NULL;
		}
		memcpy( result + (size_t) i * cluster_size, cluster_buffer, cluster_size );
		free( cluster_buffer );
	}
	free( chain );

	*out_total_size = (size_t) cluster_count * cluster_size;
	return result;
}

int exfat_reader_scan_root_directory(
     exfat_reader_t *reader,
     exfat_undelete_scan_result_t *out )
{
	size_t cluster_size;
	uint8_t *directory_buffer;
	size_t directory_buffer_size;
	int scan_result;

	if( reader == NULL || reader->file == READER_IO_INVALID || out == NULL )
	{
		return -1;
	}

	cluster_size     = (size_t) reader->boot_sector.sectors_per_cluster * reader->boot_sector.bytes_per_sector;
	directory_buffer = exfat_reader_read_directory_clusters(
	                     reader, reader->boot_sector.first_cluster_of_root_directory, 0, 0,
	                     cluster_size, 0, NULL, &directory_buffer_size );
	if( directory_buffer == NULL )
	{
		return -1;
	}

	scan_result = exfat_undelete_scanner_scan_directory( directory_buffer, directory_buffer_size, "", out );

	free( directory_buffer );
	return scan_result;
}

/* 递归扫描一个目录（根目录或任意层级的子目录），和 fat_reader.c 里的
 * 同名函数同样的设计：depth==0 读取失败是真失败，depth>0 读取失败只
 * 跳过这一层，不拖垮已经找到的其它结果。 */
static int exfat_reader_scan_directory_recursive(
     exfat_reader_t *reader,
     uint32_t cluster,
     int no_fat_chain,
     uint64_t declared_size,
     size_t cluster_size,
     const char *directory_path,
     int depth,
     bad_sector_log_t *out_bad_sector_log,
     exfat_undelete_scan_result_t *out )
{
	uint8_t *buffer;
	size_t buffer_size;
	exfat_undelete_scan_result_t this_level_result;
	exfat_subdirectory_list_t subdirs;
	int i;

	if( depth >= EXFAT_MAX_RECURSION_DEPTH )
	{
		return 1;
	}

	buffer = exfat_reader_read_directory_clusters( reader, cluster, no_fat_chain, declared_size, cluster_size,
	           out_bad_sector_log != NULL ? EXFAT_READER_MAX_READ_RETRIES : 0, out_bad_sector_log, &buffer_size );
	if( buffer == NULL )
	{
		return depth == 0 ? -1 : 1;
	}

	memset( &this_level_result, 0, sizeof( this_level_result ) );
	if( exfat_undelete_scanner_scan_directory( buffer, buffer_size, directory_path, &this_level_result ) != 1 )
	{
		free( buffer );
		return depth == 0 ? -1 : 1;
	}
	if( exfat_undelete_scan_result_append_all( out, &this_level_result ) != 1 )
	{
		exfat_undelete_scan_result_free( &this_level_result );
		free( buffer );
		return -1;
	}
	exfat_undelete_scan_result_free( &this_level_result );

	memset( &subdirs, 0, sizeof( subdirs ) );
	if( exfat_undelete_scanner_find_subdirectories( buffer, buffer_size, &subdirs ) != 1 )
	{
		free( buffer );
		return depth == 0 ? -1 : 1;
	}
	free( buffer );

	for( i = 0; i < subdirs.count; i++ )
	{
		char child_path[ 64 ];
		int recurse_result;

		strncpy( child_path, directory_path, sizeof( child_path ) - 1 );
		child_path[ sizeof( child_path ) - 1 ] = '\0';

		if( path_builder_append_segment( child_path, sizeof( child_path ), subdirs.entries[ i ].name ) != 1 )
		{
			continue;
		}

		recurse_result = exfat_reader_scan_directory_recursive(
		                   reader, subdirs.entries[ i ].first_cluster,
		                   subdirs.entries[ i ].no_fat_chain, subdirs.entries[ i ].file_size,
		                   cluster_size, child_path, depth + 1,
		                   out_bad_sector_log, out );
		if( recurse_result != 1 )
		{
			exfat_subdirectory_list_free( &subdirs );
			return recurse_result;
		}
	}

	exfat_subdirectory_list_free( &subdirs );
	return 1;
}

int exfat_reader_scan_directory_tree(
     exfat_reader_t *reader,
     bad_sector_log_t *out_bad_sector_log,
     exfat_undelete_scan_result_t *out )
{
	size_t cluster_size;
	int result;

	if( reader == NULL || reader->file == READER_IO_INVALID || out == NULL )
	{
		return -1;
	}

	out->candidates = NULL;
	out->count      = 0;
	out->capacity   = 0;

	cluster_size = (size_t) reader->boot_sector.sectors_per_cluster * reader->boot_sector.bytes_per_sector;

	result = exfat_reader_scan_directory_recursive(
	          reader, reader->boot_sector.first_cluster_of_root_directory, 0, 0, cluster_size, "", 0,
	          out_bad_sector_log, out );

	if( result != 1 )
	{
		exfat_undelete_scan_result_free( out );
	}

	return result;
}

/* 把整张 FAT 表读进内存（和子目录/根目录簇的读取一样是"读一段固定大小
 * 区域"这个重复动作，但 FAT 表起始偏移和大小是按扇区算的，不是按簇，
 * 没有复用 exfat_reader_read_cluster 的价值，所以单独写）。FatOffset
 * 换算出来的字节偏移本身就是扇区整数倍，FatLength 同理，不需要额外的
 * 扇区对齐处理。*/
static uint8_t *exfat_reader_read_fat_table(
     exfat_reader_t *reader,
     size_t *out_fat_table_size )
{
	size_t fat_table_size;
	long byte_offset;
	uint8_t *buffer;

	fat_table_size = (size_t) reader->boot_sector.fat_length_sectors * reader->boot_sector.bytes_per_sector;
	byte_offset    = reader->base_offset
	               + (long) reader->boot_sector.fat_offset_sectors * reader->boot_sector.bytes_per_sector;

	buffer = (uint8_t *) malloc( fat_table_size );
	if( buffer == NULL )
	{
		return NULL;
	}

	if( reader_io_seek( reader->file, byte_offset ) != 1
	 || reader_io_read( reader->file, buffer, fat_table_size ) != fat_table_size )
	{
		free( buffer );
		return NULL;
	}

	*out_fat_table_size = fat_table_size;
	return buffer;
}

/* 碎片化文件恢复路径（2026-11-05 小树裁决第16步）：先读整张 FAT 表，
 * 走出完整簇链，再逐簇读取、依次写出。expected_clusters（由 file_size
 * 反推）同时兼顾两个作用：既是簇链数组的容量上限（防环兜底），也是一次
 * 一致性校验——如果真实链在声称的簇数之前就提前 EOF/遇到空闲簇，或者
 * 超过这个数还没结束，都说明链和目录项记录的大小对不上，直接判定失败，
 * 不写出可能错误/被截断的内容（延续本项目"不能静默返回看起来成功、
 * 实际错误的结果"这条一贯原则）。 */
static int exfat_reader_recover_fragmented_file(
     exfat_reader_t *reader,
     uint32_t first_cluster,
     uint64_t file_size,
     bad_sector_log_t *out_bad_sector_log,
     const char *output_path )
{
	size_t cluster_size;
	uint32_t expected_clusters;
	uint8_t *fat_table;
	size_t fat_table_size;
	uint32_t *chain;
	uint32_t collected;
	exfat_fat_chain_walk_status_t status;
	FILE *output;
	uint64_t bytes_remaining;
	uint32_t i;
	int result;

	cluster_size = (size_t) reader->boot_sector.sectors_per_cluster * reader->boot_sector.bytes_per_sector;
	if( cluster_size == 0 )
	{
		return -1;
	}
	expected_clusters = (uint32_t) ( ( file_size + cluster_size - 1 ) / cluster_size );
	if( expected_clusters == 0 )
	{
		expected_clusters = 1;
	}

	fat_table = exfat_reader_read_fat_table( reader, &fat_table_size );
	if( fat_table == NULL )
	{
		return -1;
	}

	chain = (uint32_t *) malloc( (size_t) expected_clusters * sizeof( uint32_t ) );
	if( chain == NULL )
	{
		free( fat_table );
		return -1;
	}

	if( exfat_fat_chain_walk(
	     fat_table, fat_table_size, first_cluster,
	     reader->boot_sector.cluster_count, expected_clusters,
	     chain, &collected, &status ) != 1
	 || status != EXFAT_FAT_CHAIN_WALK_OK
	 || collected != expected_clusters )
	{
		free( chain );
		free( fat_table );
		return -1;
	}
	free( fat_table );

	output = fopen( output_path, "wb" );
	if( output == NULL )
	{
		free( chain );
		return -1;
	}

	result          = 1;
	bytes_remaining = file_size;

	for( i = 0; i < collected; i++ )
	{
		uint8_t *cluster_buffer;
		size_t write_size;

		/* out_bad_sector_log 非 NULL 时，exfat_reader_read_cluster 对
		 * 重试耗尽的簇会返回一块 0 填充的 buffer（而不是 NULL），这正好
		 * 符合碎片化恢复这里想要的降级语义：坏道簇的内容在输出文件里
		 * 是 0，其它簇的内容照常正确写出，不会因为一个坏簇就放弃整个
		 * 文件（2026-11-05 小树裁决第18步，参照 FAT32/exFAT 连续存储
		 * 恢复已经验证过的"0 填充+记日志+继续"做法） */
		cluster_buffer = exfat_reader_read_cluster( reader, chain[ i ], cluster_size,
		                   out_bad_sector_log != NULL ? EXFAT_READER_MAX_READ_RETRIES : 0, out_bad_sector_log );
		if( cluster_buffer == NULL )
		{
			result = -1;
			break;
		}

		write_size = (size_t) ( bytes_remaining < (uint64_t) cluster_size ? bytes_remaining : (uint64_t) cluster_size );

		if( fwrite( cluster_buffer, 1, write_size, output ) != write_size )
		{
			free( cluster_buffer );
			result = -1;
			break;
		}
		free( cluster_buffer );
		bytes_remaining -= write_size;
	}

	fclose( output );
	free( chain );
	return result;
}

/* P5 坏道保护（2026-11-05 小树裁决第18步，桶①+桶②）：out_bad_sector_log
 * 非 NULL 时两条分支（连续存储/碎片化）都启用重试+跳过+记录，固定重试
 * 次数 EXFAT_READER_MAX_READ_RETRIES，和 FAT32 那条路径的设计保持一致，
 * 不是两套平行逻辑。 */
int exfat_reader_recover_file(
     exfat_reader_t *reader,
     uint32_t first_cluster,
     uint64_t file_size,
     int no_fat_chain,
     bad_sector_log_t *out_bad_sector_log,
     const char *output_path )
{
	uint32_t start_sector;
	long byte_offset;

	if( reader == NULL || reader->file == READER_IO_INVALID || output_path == NULL )
	{
		return -1;
	}

	if( !no_fat_chain )
	{
		return exfat_reader_recover_fragmented_file( reader, first_cluster, file_size, out_bad_sector_log, output_path );
	}

	start_sector = exfat_boot_sector_cluster_to_sector( &reader->boot_sector, first_cluster );
	byte_offset  = reader->base_offset + (long) start_sector * reader->boot_sector.bytes_per_sector;

	if( reader_io_seek( reader->file, byte_offset ) != 1 )
	{
		return -1;
	}

	return aligned_device_reader_copy_to_file(
	        exfat_reader_read_adapter,
	        reader,
	        (size_t) reader->boot_sector.bytes_per_sector,
	        file_size,
	        out_bad_sector_log != NULL ? EXFAT_READER_MAX_READ_RETRIES : 0,
	        out_bad_sector_log,
	        output_path );
}
