/*
 * exFAT F4 的 TDD 测试：对真实 fixture 的根目录簇做全量扫描，应该正好找到
 * 1 个已删除候选项（SECRET.TXT，文件名完整保留），不应该把正常的
 * HELLO.TXT、以及位图/大写表这些系统条目算进去。
 *
 * 2026-11-05 小树裁决第12步追加：子目录遍历相关测试——找出根目录里的
 * 子目录（SUBDIR）、扫描子目录内容找到已删除文件（SUBDIR\DEEP.TXT，
 * directory_path 应该正确标记）、以及把子目录扫描结果合并进根目录结果。
 *
 * 2026-11-05 小树裁决第14步追加：二级子目录测试——SUBDIR 下面还有一层
 * SUBSUBDIR，和 FAT32 那一轮同样的验证目的：核心扫描函数本身不关心
 * "当前是第几层"，真正的递归下钻逻辑在 shell 层的 exfat_reader.c 里。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_framework.h"
#include "../../core/exfat_boot_sector.h"
#include "../../core/exfat_undelete_scanner.h"

static uint8_t *read_whole_file( const char *path, size_t *out_size )
{
	FILE *f = fopen( path, "rb" );
	uint8_t *buffer;
	long size;

	if( f == NULL )
	{
		return NULL;
	}
	fseek( f, 0, SEEK_END );
	size = ftell( f );
	fseek( f, 0, SEEK_SET );

	buffer = (uint8_t *) malloc( (size_t) size );
	if( buffer == NULL || fread( buffer, 1, (size_t) size, f ) != (size_t) size )
	{
		free( buffer );
		fclose( f );
		return NULL;
	}
	fclose( f );
	*out_size = (size_t) size;
	return buffer;
}

static const exfat_undelete_candidate_t *find_by_name(
       const exfat_undelete_scan_result_t *result,
       const char *name )
{
	int i;
	for( i = 0; i < result->count; i++ )
	{
		if( strcmp( result->candidates[ i ].name, name ) == 0 )
		{
			return &( result->candidates[ i ] );
		}
	}
	return NULL;
}

static void test_scan_finds_exactly_one_deleted_file(
     const uint8_t *root_dir_cluster,
     size_t cluster_size )
{
	exfat_undelete_scan_result_t result;
	const exfat_undelete_candidate_t *secret;
	const exfat_undelete_candidate_t *chinese;
	const exfat_undelete_candidate_t *frag;

	TEST_CASE( "扫描根目录应该正好找到 3 个已删除候选项（SECRET.TXT + 中文文件名 + 碎片化的 FRAG.BIN），"
	           "文件名完整保留，不包含子目录 SUBDIR" );

	memset( &result, 0, sizeof( result ) );

	TEST_ASSERT( exfat_undelete_scanner_scan_directory( root_dir_cluster, cluster_size, "", &result ) == 1,
	             "扫描不应报错" );
	TEST_ASSERT_EQ_INT( result.count, 3, "应该正好找到 3 个候选项" );

	secret = find_by_name( &result, "SECRET.TXT" );
	TEST_ASSERT( secret != NULL, "应该找到完整文件名 SECRET.TXT（exFAT 删除不破坏文件名）" );

	if( secret != NULL )
	{
		TEST_ASSERT_EQ_INT( (int) secret->first_cluster, 6, "起始簇号应该是 6" );
		TEST_ASSERT_EQ_INT( (int) secret->file_size, 66, "文件大小应该是 66" );
		TEST_ASSERT_EQ_STR( secret->directory_path, "", "根目录候选项的 directory_path 应该是空字符串" );
		TEST_ASSERT_EQ_INT( secret->no_fat_chain, 1, "fixture 里的连续存储文件，no_fat_chain 应该透传为 1" );
	}

	chinese = find_by_name( &result, "测试文件.txt" );
	TEST_ASSERT( chinese != NULL, "应该找到正确解码的中文文件名" );

	if( chinese != NULL )
	{
		TEST_ASSERT_EQ_INT( (int) chinese->first_cluster, 10, "起始簇号应该是 10" );
	}

	frag = find_by_name( &result, "FRAG.BIN" );
	TEST_ASSERT( frag != NULL, "应该找到碎片化文件 FRAG.BIN（2026-11-05 小树裁决第16步新增场景）" );

	if( frag != NULL )
	{
		TEST_ASSERT_EQ_INT( (int) frag->first_cluster, 20, "起始簇号应该是 20" );
		TEST_ASSERT_EQ_INT( (int) frag->file_size, 2648, "文件大小应该是 2648" );
		TEST_ASSERT_EQ_INT( frag->no_fat_chain, 0, "碎片化文件的 no_fat_chain 应该透传为 0" );
	}

	TEST_ASSERT( find_by_name( &result, "HELLO.TXT" ) == NULL,
	             "正常存在的 HELLO.TXT 不应该出现在已删除候选项里" );

	exfat_undelete_scan_result_free( &result );
}

static void test_find_subdirectories_finds_subdir(
     const uint8_t *root_dir_cluster,
     size_t cluster_size )
{
	exfat_subdirectory_list_t subdirs;

	TEST_CASE( "根目录里应该正好找到 2 个子目录 SUBDIR 和 BIGDIR" );

	memset( &subdirs, 0, sizeof( subdirs ) );

	TEST_ASSERT( exfat_undelete_scanner_find_subdirectories( root_dir_cluster, cluster_size, &subdirs ) == 1,
	             "查找子目录不应报错" );
	TEST_ASSERT_EQ_INT( subdirs.count, 2, "根目录下应该正好有 2 个子目录" );

	if( subdirs.count == 2 )
	{
		TEST_ASSERT_EQ_STR( subdirs.entries[ 0 ].name, "SUBDIR", "第一个子目录名字应该是 SUBDIR" );
		TEST_ASSERT_EQ_INT( (int) subdirs.entries[ 0 ].first_cluster, 7, "SUBDIR 的起始簇应该是 7" );
		/* 2026-11-05 第21步："目录单簇假设"修复：子目录列表现在要透传
		 * no_fat_chain/file_size，供 shell 层判断该不该跟着 FAT 链多读几个
		 * 簇。fixture 里的 SUBDIR 是用默认 fragmented=False 构造的，
		 * 应该是连续存储（no_fat_chain=1），大小正好 1 个簇 */
		TEST_ASSERT_EQ_INT( subdirs.entries[ 0 ].no_fat_chain, 1, "SUBDIR 是连续存储，no_fat_chain 应该透传为 1" );
		TEST_ASSERT_EQ_INT( (int) subdirs.entries[ 0 ].file_size, 2048, "SUBDIR 声明大小应该是 1 个簇(2048字节)" );

		/* BIGDIR 是 fragmented=True 构造的多簇目录（第21步新增场景），
		 * no_fat_chain 应该透传为 0，表示 shell 层该跟着 FAT 链走，
		 * 不能只读 first_cluster 那一个簇 */
		TEST_ASSERT_EQ_STR( subdirs.entries[ 1 ].name, "BIGDIR", "第二个子目录名字应该是 BIGDIR" );
		TEST_ASSERT_EQ_INT( (int) subdirs.entries[ 1 ].first_cluster, 24, "BIGDIR 的起始簇应该是 24" );
		TEST_ASSERT_EQ_INT( subdirs.entries[ 1 ].no_fat_chain, 0, "BIGDIR 是碎片化多簇目录，no_fat_chain 应该透传为 0" );
		TEST_ASSERT_EQ_INT( (int) subdirs.entries[ 1 ].file_size, 4096, "BIGDIR 声明大小应该是 2 个簇(4096字节)" );
	}

	exfat_subdirectory_list_free( &subdirs );
}

static void test_scan_subdirectory_finds_deleted_file_with_directory_path(
     const uint8_t *subdir_cluster,
     size_t cluster_size )
{
	exfat_undelete_scan_result_t result;
	const exfat_undelete_candidate_t *deep;

	TEST_CASE( "扫描子目录簇应该找到 SUBDIR\\DEEP.TXT（已删除），directory_path 应该正确标记为 SUBDIR" );

	memset( &result, 0, sizeof( result ) );

	TEST_ASSERT( exfat_undelete_scanner_scan_directory( subdir_cluster, cluster_size, "SUBDIR", &result ) == 1,
	             "扫描不应报错" );
	TEST_ASSERT_EQ_INT( result.count, 1, "子目录里应该正好找到 1 个已删除候选项" );

	deep = find_by_name( &result, "DEEP.TXT" );
	TEST_ASSERT( deep != NULL, "应该找到完整文件名 DEEP.TXT" );

	if( deep != NULL )
	{
		TEST_ASSERT_EQ_INT( (int) deep->first_cluster, 8, "起始簇号应该是 8" );
		TEST_ASSERT_EQ_STR( deep->directory_path, "SUBDIR", "directory_path 应该是 SUBDIR" );
	}

	TEST_ASSERT( find_by_name( &result, "STILL.TXT" ) == NULL,
	             "正常存在的 STILL.TXT 不应该出现在已删除候选项里" );

	exfat_undelete_scan_result_free( &result );
}

static void test_find_subdirectories_finds_subsubdir_inside_subdir(
     const uint8_t *subdir_cluster,
     size_t cluster_size )
{
	exfat_subdirectory_list_t subdirs;

	TEST_CASE( "SUBDIR 自己的目录内容里应该正好找到 1 个子目录 SUBSUBDIR，不会把 DEEP.TXT/STILL.TXT 当成子目录" );

	memset( &subdirs, 0, sizeof( subdirs ) );

	TEST_ASSERT( exfat_undelete_scanner_find_subdirectories( subdir_cluster, cluster_size, &subdirs ) == 1,
	             "查找子目录不应报错" );
	TEST_ASSERT_EQ_INT( subdirs.count, 1, "SUBDIR 下应该正好有 1 个子目录" );

	if( subdirs.count == 1 )
	{
		TEST_ASSERT_EQ_STR( subdirs.entries[ 0 ].name, "SUBSUBDIR", "子目录名字应该是 SUBSUBDIR" );
		TEST_ASSERT_EQ_INT( (int) subdirs.entries[ 0 ].first_cluster, 11, "SUBSUBDIR 的起始簇应该是 11" );
	}

	exfat_subdirectory_list_free( &subdirs );
}

static void test_scan_subsubdirectory_finds_deleted_file_with_nested_directory_path(
     const uint8_t *subsubdir_cluster,
     size_t cluster_size )
{
	exfat_undelete_scan_result_t result;
	const exfat_undelete_candidate_t *deep2;

	TEST_CASE( "扫描第二层子目录簇应该找到 SUBDIR\\SUBSUBDIR\\DEEP2.TXT（已删除），"
	           "directory_path 应该正确标记为嵌套路径 SUBDIR\\SUBSUBDIR，验证不是只支持一层" );

	memset( &result, 0, sizeof( result ) );

	TEST_ASSERT( exfat_undelete_scanner_scan_directory( subsubdir_cluster, cluster_size, "SUBDIR\\SUBSUBDIR", &result ) == 1,
	             "扫描不应报错" );
	TEST_ASSERT_EQ_INT( result.count, 1, "第二层子目录里应该正好找到 1 个已删除候选项" );

	deep2 = find_by_name( &result, "DEEP2.TXT" );
	TEST_ASSERT( deep2 != NULL, "应该找到完整文件名 DEEP2.TXT" );

	if( deep2 != NULL )
	{
		TEST_ASSERT_EQ_INT( (int) deep2->first_cluster, 12, "起始簇号应该是 12" );
		TEST_ASSERT_EQ_STR( deep2->directory_path, "SUBDIR\\SUBSUBDIR", "directory_path 应该是嵌套路径 SUBDIR\\SUBSUBDIR" );
	}

	TEST_ASSERT( find_by_name( &result, "STILL2.TXT" ) == NULL,
	             "正常存在的 STILL2.TXT 不应该出现在已删除候选项里" );

	exfat_undelete_scan_result_free( &result );
}

static void test_merge_appends_subdirectory_results_into_root_results(
     const uint8_t *root_dir_cluster,
     const uint8_t *subdir_cluster,
     size_t cluster_size )
{
	exfat_undelete_scan_result_t root_result;
	exfat_undelete_scan_result_t subdir_result;

	TEST_CASE( "把子目录扫描结果合并进根目录结果后，应该同时包含两边的候选项" );

	memset( &root_result, 0, sizeof( root_result ) );
	memset( &subdir_result, 0, sizeof( subdir_result ) );

	TEST_ASSERT( exfat_undelete_scanner_scan_directory( root_dir_cluster, cluster_size, "", &root_result ) == 1,
	             "扫描根目录不应报错" );
	TEST_ASSERT( exfat_undelete_scanner_scan_directory( subdir_cluster, cluster_size, "SUBDIR", &subdir_result ) == 1,
	             "扫描子目录不应报错" );

	TEST_ASSERT( exfat_undelete_scan_result_append_all( &root_result, &subdir_result ) == 1,
	             "合并不应报错" );
	TEST_ASSERT_EQ_INT( root_result.count, 4, "合并后应该一共有 4 个候选项（SECRET.TXT + 中文文件名 + FRAG.BIN + DEEP.TXT）" );
	TEST_ASSERT( find_by_name( &root_result, "SECRET.TXT" ) != NULL, "合并后应该还能找到根目录的 SECRET.TXT" );
	TEST_ASSERT( find_by_name( &root_result, "DEEP.TXT" ) != NULL, "合并后应该能找到子目录的 DEEP.TXT" );

	exfat_undelete_scan_result_free( &root_result );
	exfat_undelete_scan_result_free( &subdir_result );
}

static void test_free_is_safe_on_empty_result( void )
{
	exfat_undelete_scan_result_t result;

	TEST_CASE( "exfat_undelete_scan_result_free 对一个从未填充过的 result 调用应该是安全的" );

	memset( &result, 0, sizeof( result ) );
	exfat_undelete_scan_result_free( &result );

	TEST_ASSERT_EQ_INT( result.count, 0, "释放后 count 应该是 0" );
	TEST_ASSERT( result.candidates == NULL, "释放后 candidates 应该是 NULL" );
}

int main( int argc, char *argv[] )
{
	const char *fixture_path = argc > 1
	                             ? argv[ 1 ]
	                             : "../../tests/fixtures/exfat-basic-undelete/volume.img";
	size_t image_size = 0;
	uint8_t *image = read_whole_file( fixture_path, &image_size );
	exfat_boot_sector_t bs;
	uint32_t root_dir_sector_number;
	size_t root_dir_byte_offset;
	size_t cluster_size;
	uint32_t subdir_sector_number;
	size_t subdir_byte_offset;
	uint32_t subsubdir_sector_number;
	size_t subsubdir_byte_offset;

	if( image == NULL )
	{
		fprintf( stderr, "无法读取测试镜像: %s\n", fixture_path );
		return 1;
	}

	if( exfat_boot_sector_parse( image, 512, &bs ) != 1 || !bs.is_valid )
	{
		fprintf( stderr, "引导扇区解析失败\n" );
		free( image );
		return 1;
	}

	root_dir_sector_number = exfat_boot_sector_cluster_to_sector( &bs, bs.first_cluster_of_root_directory );
	root_dir_byte_offset   = (size_t) root_dir_sector_number * bs.bytes_per_sector;
	cluster_size           = (size_t) bs.sectors_per_cluster * bs.bytes_per_sector;

	/* SUBDIR 的起始簇号是 fixture 构造脚本里写死的 7，直接用簇号换算字节偏移 */
	subdir_sector_number = exfat_boot_sector_cluster_to_sector( &bs, 7 );
	subdir_byte_offset   = (size_t) subdir_sector_number * bs.bytes_per_sector;

	/* SUBSUBDIR（SUBDIR 下面第二层子目录）的起始簇号同样是 fixture 脚本里
	 * 写死的 11，和 test_find_subdirectories_finds_subsubdir_inside_subdir
	 * 的断言是同一个来源 */
	subsubdir_sector_number = exfat_boot_sector_cluster_to_sector( &bs, 11 );
	subsubdir_byte_offset   = (size_t) subsubdir_sector_number * bs.bytes_per_sector;

	test_scan_finds_exactly_one_deleted_file( image + root_dir_byte_offset, cluster_size );
	test_find_subdirectories_finds_subdir( image + root_dir_byte_offset, cluster_size );
	test_scan_subdirectory_finds_deleted_file_with_directory_path( image + subdir_byte_offset, cluster_size );
	test_find_subdirectories_finds_subsubdir_inside_subdir( image + subdir_byte_offset, cluster_size );
	test_scan_subsubdirectory_finds_deleted_file_with_nested_directory_path( image + subsubdir_byte_offset, cluster_size );
	test_merge_appends_subdirectory_results_into_root_results(
	     image + root_dir_byte_offset, image + subdir_byte_offset, cluster_size );
	test_free_is_safe_on_empty_result();

	free( image );

	TEST_SUMMARY();
}
