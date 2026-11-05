#include <stdlib.h>
#include <string.h>

#include "aligned_device_reader.h"
#include "fat_reader.h"
#include "../core/fat_fat_chain.h"
#include "../core/path_builder.h"

/* 目录簇链遍历的防御性上限（2026-11-05 第21步，"目录单簇假设"修复）：
 * 和 FAT_MAX_RECURSION_DEPTH 同样的设计哲学，不是真实场景的硬限制，
 * 纯粹防止损坏数据/理论上的环形引用导致无限循环或超大内存分配。 */
#define FAT_READER_MAX_DIRECTORY_CLUSTERS 4096

/* 递归扫描子目录时的最大深度兜底——正常目录嵌套不可能接近这个深度，
 * 这纯粹是防御损坏数据/理论上的环形引用（比如损坏的簇号意外指回上层
 * 目录），不是真实场景会触发的限制。2026-11-05 小树裁决第14步：从
 * 固定一层扩展成真正的递归，这个深度上限是递归版本新增的安全阀，
 * 一层版本不需要这个，因为它本来就只会往下走一层。 */
#define FAT_MAX_RECURSION_DEPTH 16

/* 读失败时最多重试几次才判定为坏道——不是真实场景实测出来的"最优值"，
 * 是一个保守、够用的默认值：重试这几次的开销对一次性的恢复操作来说
 * 可以忽略不计，没必要做成可配置项增加复杂度（2026-11-05 小树裁决
 * 第17/18步：原来只给 recover_file 用，第18步起 scan 路径的目录簇读取
 * 也复用同一个常量） */
#define FAT_READER_MAX_READ_RETRIES 3

/* 下面这组 reader_io_* 是对"打开设备/文件、按偏移读字节"这件事的最小封装，
 * 只在本文件内部使用。Windows 下走 CreateFileA+ReadFile（裸设备路径需要），
 * 其他平台（目前只有 Mac 开发机，只对着自己造的镜像文件测试）继续用 fopen 这套，
 * 没必要为了和 Windows 保持一致而引入不需要的复杂度。 */
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

int fat_reader_open(
     const char *path,
     fat_reader_t *out )
{
	return fat_reader_open_at_offset( path, 0, out );
}

int fat_reader_open_at_offset(
     const char *path,
     long base_offset,
     fat_reader_t *out )
{
	uint8_t boot_sector_bytes[ 512 ];

	if( path == NULL || out == NULL )
	{
		return -1;
	}
	memset( out, 0, sizeof( fat_reader_t ) );
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

	if( fat_boot_sector_parse( boot_sector_bytes, sizeof( boot_sector_bytes ), &out->boot_sector ) != 1
	 || !out->boot_sector.is_valid )
	{
		reader_io_close( out->file );
		out->file = READER_IO_INVALID;
		return -1;
	}

	return 1;
}

void fat_reader_close(
      fat_reader_t *reader )
{
	if( reader == NULL || reader->file == READER_IO_INVALID )
	{
		return;
	}
	reader_io_close( reader->file );
	reader->file = READER_IO_INVALID;
}

/* 下面这个适配器把 reader->file 包装成 aligned_device_reader_* 系列函数
 * 需要的 (context, buffer, size) -> actually_read 形状，真正的扇区对齐/
 * 重试处理逻辑已经抽到 engine/shell/aligned_device_reader.c 里独立测试
 * 过了，这里不再重复维护一份 */
static size_t fat_reader_read_adapter( void *context, void *buffer, size_t size )
{
	fat_reader_t *reader = (fat_reader_t *) context;

	return reader_io_read( reader->file, buffer, size );
}

/* 把"读某个簇的完整内容到一块新分配的内存"这个重复动作抽出来，根目录和
 * 子目录读取都是同一回事——只是簇号不同，调用方用完自己 free。
 *
 * P5 坏道保护（2026-11-05 小树裁决第18步）：out_bad_sector_log 非 NULL 时，
 * 读取失败会先重试，重试耗尽后不是直接放弃——用
 * aligned_device_reader_read_block 的"0 填充+记日志+返回成功"语义：
 * 一个全 0 的目录簇，按 FAT 目录项格式解析出来，首字节 0x00 天然就是
 * "目录结束"标记，等价于"这个（子）目录是空的"，scan 逻辑原有的"跳过
 * 这一层、不拖垮其它已扫到的结果"的效果没有变化，只是现在这个结果是
 * 坏道容忍统一逻辑的自然推论，不需要再单独维护一套"返回 NULL 让调用方
 * 跳过"的平行语义。 */
static uint8_t *fat_reader_read_cluster(
     fat_reader_t *reader,
     uint32_t cluster,
     size_t cluster_size,
     uint32_t max_retries,
     bad_sector_log_t *out_bad_sector_log )
{
	uint32_t sector;
	long byte_offset;
	uint8_t *buffer;

	sector      = fat_boot_sector_cluster_to_sector( &reader->boot_sector, cluster );
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
	     fat_reader_read_adapter, reader, buffer, cluster_size,
	     max_retries, (uint64_t) byte_offset, out_bad_sector_log ) != 1 )
	{
		free( buffer );
		return NULL;
	}

	return buffer;
}

/* 把整张 FAT 表读进内存——和 exfat_reader.c 里的同名辅助函数同样的理由，
 * FatOffset/FatSize 换算出来的字节偏移/大小本身就是扇区整数倍，不需要
 * 额外的扇区对齐处理 */
static uint8_t *fat_reader_read_fat_table(
     fat_reader_t *reader,
     size_t *out_fat_table_size )
{
	size_t fat_table_size;
	long byte_offset;
	uint8_t *buffer;

	fat_table_size = (size_t) reader->boot_sector.fat_size_sectors * reader->boot_sector.bytes_per_sector;
	byte_offset    = reader->base_offset
	               + (long) fat_boot_sector_fat_start_sector( &reader->boot_sector ) * reader->boot_sector.bytes_per_sector;

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

/* 这个卷数据区一共有多少个数据簇——fat_fat_chain_walk 判断簇号是否超出
 * 合法范围需要这个值，FAT32 引导扇区本身没有直接存这个字段，要用总扇区数
 * 减掉数据区起始扇区号再除以每簇扇区数反推出来 */
static uint32_t fat_reader_total_data_clusters(
     fat_reader_t *reader )
{
	uint32_t data_start_sector;

	if( reader->boot_sector.sectors_per_cluster == 0 )
	{
		return 0;
	}
	data_start_sector = fat_boot_sector_data_start_sector( &reader->boot_sector );
	if( reader->boot_sector.total_sectors <= data_start_sector )
	{
		return 0;
	}
	return ( reader->boot_sector.total_sectors - data_start_sector ) / reader->boot_sector.sectors_per_cluster;
}

/* 读取一个目录（根目录或子目录）占用的全部簇，跟着 FAT 链拼接成一段
 * 连续内存返回——fat_undelete_scanner_scan_directory/find_subdirectories
 * 这两个纯函数只关心"一段目录字节"，不关心这段字节背后是一个簇还是
 * 多个簇拼接而来（2026-11-05 第21步，修复"目录单簇假设"：原来这里只读
 * 第一个簇，目录内容超过一个簇时后面的已删除候选会被静默漏掉）。
 *
 * FAT32 没有 exFAT 那种"连续存储可以不查表"的 NoFatChain 优化，簇永远
 * 要靠 FAT 表链式结构表示，所以这里不需要任何分支判断，根目录和子目录
 * 统一走这一条路径——比 exFAT 那边的同名函数更简单。 */
static uint8_t *fat_reader_read_directory_clusters(
     fat_reader_t *reader,
     uint32_t first_cluster,
     size_t cluster_size,
     uint32_t max_retries,
     bad_sector_log_t *out_bad_sector_log,
     size_t *out_total_size )
{
	uint8_t *fat_table;
	size_t fat_table_size;
	uint32_t *chain;
	uint32_t collected;
	fat_fat_chain_walk_status_t status;
	uint8_t *result;
	uint32_t i;

	fat_table = fat_reader_read_fat_table( reader, &fat_table_size );
	if( fat_table == NULL )
	{
		return NULL;
	}

	chain = (uint32_t *) malloc( (size_t) FAT_READER_MAX_DIRECTORY_CLUSTERS * sizeof( uint32_t ) );
	if( chain == NULL )
	{
		free( fat_table );
		return NULL;
	}

	if( fat_fat_chain_walk(
	     fat_table, fat_table_size, first_cluster,
	     fat_reader_total_data_clusters( reader ), FAT_READER_MAX_DIRECTORY_CLUSTERS,
	     chain, &collected, &status ) != 1
	 || status != FAT_FAT_CHAIN_WALK_OK )
	{
		free( chain );
		free( fat_table );
		return NULL;
	}
	free( fat_table );

	result = (uint8_t *) malloc( (size_t) collected * cluster_size );
	if( result == NULL )
	{
		free( chain );
		return NULL;
	}

	for( i = 0; i < collected; i++ )
	{
		uint8_t *cluster_buffer = fat_reader_read_cluster(
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

	*out_total_size = (size_t) collected * cluster_size;
	return result;
}

int fat_reader_scan_root_directory(
     fat_reader_t *reader,
     fat_undelete_scan_result_t *out )
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
	directory_buffer = fat_reader_read_directory_clusters(
	                     reader, reader->boot_sector.root_cluster, cluster_size, 0, NULL, &directory_buffer_size );
	if( directory_buffer == NULL )
	{
		return -1;
	}

	scan_result = fat_undelete_scanner_scan_directory( directory_buffer, directory_buffer_size, "", out );

	free( directory_buffer );
	return scan_result;
}

/* 递归扫描一个目录（根目录或任意层级的子目录）：把这一层找到的已删除
 * 候选项累加进 out，再找这一层的子目录、对每个子目录递归下去。
 * depth==0 时读取失败是真失败（根目录都读不到，说明卷本身有问题）；
 * depth>0 时读取失败只跳过这一层，不拖垮已经找到的其它结果——这个
 * 区分和原来一层版本的处理哲学一致，只是现在要在递归函数内部自己
 * 判断，不能再依赖调用方（只有根目录）天然保证 depth==0。 */
static int fat_reader_scan_directory_recursive(
     fat_reader_t *reader,
     uint32_t cluster,
     size_t cluster_size,
     const char *directory_path,
     int depth,
     bad_sector_log_t *out_bad_sector_log,
     fat_undelete_scan_result_t *out )
{
	uint8_t *buffer;
	size_t buffer_size;
	fat_undelete_scan_result_t this_level_result;
	fat_subdirectory_list_t subdirs;
	int i;

	if( depth >= FAT_MAX_RECURSION_DEPTH )
	{
		return 1;
	}

	buffer = fat_reader_read_directory_clusters( reader, cluster, cluster_size,
	           out_bad_sector_log != NULL ? FAT_READER_MAX_READ_RETRIES : 0, out_bad_sector_log, &buffer_size );
	if( buffer == NULL )
	{
		return depth == 0 ? -1 : 1;
	}

	memset( &this_level_result, 0, sizeof( this_level_result ) );
	if( fat_undelete_scanner_scan_directory( buffer, buffer_size, directory_path, &this_level_result ) != 1 )
	{
		free( buffer );
		return depth == 0 ? -1 : 1;
	}
	if( fat_undelete_scan_result_append_all( out, &this_level_result ) != 1 )
	{
		fat_undelete_scan_result_free( &this_level_result );
		free( buffer );
		return -1;
	}
	fat_undelete_scan_result_free( &this_level_result );

	memset( &subdirs, 0, sizeof( subdirs ) );
	if( fat_undelete_scanner_find_subdirectories( buffer, buffer_size, &subdirs ) != 1 )
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
			/* 路径太长装不下——跳过这一层的子目录名拼接，不让整体扫描失败，
			 * 但也不继续往这个子目录递归了（没有可靠的路径可以标记它） */
			continue;
		}

		recurse_result = fat_reader_scan_directory_recursive(
		                   reader, subdirs.entries[ i ].first_cluster, cluster_size, child_path, depth + 1,
		                   out_bad_sector_log, out );
		if( recurse_result != 1 )
		{
			fat_subdirectory_list_free( &subdirs );
			return recurse_result;
		}
	}

	fat_subdirectory_list_free( &subdirs );
	return 1;
}

int fat_reader_scan_directory_tree(
     fat_reader_t *reader,
     bad_sector_log_t *out_bad_sector_log,
     fat_undelete_scan_result_t *out )
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

	result = fat_reader_scan_directory_recursive(
	          reader, reader->boot_sector.root_cluster, cluster_size, "", 0, out_bad_sector_log, out );

	if( result != 1 )
	{
		/* 失败时 out 可能已经被递归过程中途填充了一部分数据——按本项目
		 * 一贯的"失败就不留半成品"原则清空，调用方不需要自己判断
		 * "失败了但 out 里是不是还有点东西" */
		fat_undelete_scan_result_free( out );
	}

	return result;
}

int fat_reader_recover_file(
     fat_reader_t *reader,
     uint32_t first_cluster,
     uint32_t file_size,
     bad_sector_log_t *out_bad_sector_log,
     const char *output_path )
{
	uint32_t start_sector;
	long byte_offset;

	if( reader == NULL || reader->file == READER_IO_INVALID || output_path == NULL )
	{
		return -1;
	}

	start_sector = fat_boot_sector_cluster_to_sector( &reader->boot_sector, first_cluster );
	byte_offset  = reader->base_offset + (long) start_sector * reader->boot_sector.bytes_per_sector;

	if( reader_io_seek( reader->file, byte_offset ) != 1 )
	{
		return -1;
	}

	return aligned_device_reader_copy_to_file(
	        fat_reader_read_adapter,
	        reader,
	        (size_t) reader->boot_sector.bytes_per_sector,
	        file_size,
	        out_bad_sector_log != NULL ? FAT_READER_MAX_READ_RETRIES : 0,
	        out_bad_sector_log,
	        output_path );
}
