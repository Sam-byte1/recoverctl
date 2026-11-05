/*
 * F4 的 TDD 测试：对真实 fixture 的根目录簇做全量扫描，应该正好找到 1 个
 * 已删除候选项（HELLO.TXT），不应该把正常的 WORLD.TXT 也算进去。
 *
 * 2026-11-05 小树裁决第12步追加：子目录遍历相关测试——找出根目录里的
 * 子目录（SUBDIR）、扫描子目录内容找到已删除文件（SUBDIR\SECRET.TXT，
 * directory_path 应该正确标记）、以及把子目录扫描结果合并进根目录结果。
 *
 * 2026-11-05 小树裁决第14步追加：二级子目录测试——SUBDIR 下面还有一层
 * SUBSUB2，验证 find_subdirectories/scan_directory 这两个核心函数本身
 * 不关心"当前是第几层"，只要给对 directory_path 就能正确处理任意深度
 * （真正的递归下钻逻辑在 shell 层的 fat_reader.c 里，由 e2e 测试覆盖）。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_framework.h"
#include "../../core/fat_boot_sector.h"
#include "../../core/fat_undelete_scanner.h"

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

static const fat_undelete_candidate_t *find_by_name(
       const fat_undelete_scan_result_t *result,
       const char *name_suffix )
{
	int i;
	for( i = 0; i < result->count; i++ )
	{
		if( strstr( result->candidates[ i ].name, name_suffix ) != NULL )
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
	fat_undelete_scan_result_t result;
	const fat_undelete_candidate_t *hello;
	const fat_undelete_candidate_t *longname;

	TEST_CASE( "扫描根目录应该正好找到 2 个已删除候选项（HELLO.TXT + 中文长文件名），不包含正常的 WORLD.TXT 和子目录 SUBDIR" );

	memset( &result, 0, sizeof( result ) );

	TEST_ASSERT( fat_undelete_scanner_scan_directory( root_dir_cluster, cluster_size, "", &result ) == 1,
	             "扫描不应报错" );
	TEST_ASSERT_EQ_INT( result.count, 2, "应该正好找到 2 个候选项" );

	hello = find_by_name( &result, "ELLO.TXT" );
	TEST_ASSERT( hello != NULL, "应该找到名字里带 ELLO.TXT 的候选项（首字符是占位符）" );

	if( hello != NULL )
	{
		TEST_ASSERT_EQ_INT( (int) hello->first_cluster, 3, "起始簇号应该是 3" );
		TEST_ASSERT_EQ_INT( (int) hello->file_size, 50, "文件大小应该是 50" );
		TEST_ASSERT_EQ_STR( hello->directory_path, "", "根目录候选项的 directory_path 应该是空字符串" );
	}

	longname = find_by_name( &result, "中文长文件名测试文件这是一个测试.txt" );
	TEST_ASSERT( longname != NULL,
	             "应该找到完整的中文长文件名（VFAT 长文件名不受删除影响，不像短名首字符会丢失）" );

	if( longname != NULL )
	{
		TEST_ASSERT_EQ_INT( (int) longname->first_cluster, 8, "起始簇号应该是 8" );
	}

	TEST_ASSERT( find_by_name( &result, "WORLD" ) == NULL,
	             "正常存在的 WORLD.TXT 不应该出现在已删除候选项里" );

	fat_undelete_scan_result_free( &result );
}

static void test_find_subdirectories_finds_subdir_skips_dot_entries(
     const uint8_t *root_dir_cluster,
     size_t cluster_size )
{
	fat_subdirectory_list_t subdirs;

	TEST_CASE( "根目录里应该正好找到 2 个子目录 SUBDIR 和 BIGDIR，不会把 HELLO.TXT/WORLD.TXT 当成子目录" );

	memset( &subdirs, 0, sizeof( subdirs ) );

	TEST_ASSERT( fat_undelete_scanner_find_subdirectories( root_dir_cluster, cluster_size, &subdirs ) == 1,
	             "查找子目录不应报错" );
	TEST_ASSERT_EQ_INT( subdirs.count, 2, "根目录下应该正好有 2 个子目录" );

	if( subdirs.count == 2 )
	{
		TEST_ASSERT_EQ_STR( subdirs.entries[ 0 ].name, "SUBDIR", "第一个子目录名字应该是 SUBDIR" );
		TEST_ASSERT_EQ_INT( (int) subdirs.entries[ 0 ].first_cluster, 5, "SUBDIR 的起始簇应该是 5" );

		/* BIGDIR 是 2026-11-05 第21步新增的多簇目录场景（"目录单簇假设"
		 * 修复），自己的目录内容跨 2 个簇才能读全 */
		TEST_ASSERT_EQ_STR( subdirs.entries[ 1 ].name, "BIGDIR", "第二个子目录名字应该是 BIGDIR" );
		TEST_ASSERT_EQ_INT( (int) subdirs.entries[ 1 ].first_cluster, 12, "BIGDIR 的起始簇应该是 12" );
	}

	fat_subdirectory_list_free( &subdirs );
}

static void test_subdirectory_dot_entries_are_not_treated_as_subdirectories( void )
{
	fat_subdirectory_list_t subdirs;
	uint8_t synthetic_dir[ 128 ];

	TEST_CASE( "一个目录自己的 '.' 和 '..' 条目不应该被当成可递归的子目录" );

	/* 手工构造一个只有 "." 和 ".." 两条目录项的最小目录区域，
	 * 直接验证 find_subdirectories 在这种真实结构下返回 0 个子目录，
	 * 不依赖 fixture 文件，纯用已知字节断言 */
	memset( synthetic_dir, 0, sizeof( synthetic_dir ) );
	memcpy( synthetic_dir, ".          ", 11 );
	synthetic_dir[ 11 ] = 0x10;  /* ATTR_DIRECTORY */
	memcpy( synthetic_dir + 32, "..         ", 11 );
	synthetic_dir[ 32 + 11 ] = 0x10;

	memset( &subdirs, 0, sizeof( subdirs ) );
	TEST_ASSERT( fat_undelete_scanner_find_subdirectories( synthetic_dir, sizeof( synthetic_dir ), &subdirs ) == 1,
	             "查找子目录不应报错" );
	TEST_ASSERT_EQ_INT( subdirs.count, 0, "只有 . 和 .. 的目录应该找到 0 个可递归的子目录" );

	fat_subdirectory_list_free( &subdirs );
}

static void test_scan_subdirectory_finds_deleted_file_with_directory_path(
     const uint8_t *subdir_cluster,
     size_t cluster_size )
{
	fat_undelete_scan_result_t result;
	const fat_undelete_candidate_t *secret;

	TEST_CASE( "扫描子目录簇应该找到 SUBDIR\\SECRET.TXT（已删除），directory_path 应该正确标记为 SUBDIR" );

	memset( &result, 0, sizeof( result ) );

	TEST_ASSERT( fat_undelete_scanner_scan_directory( subdir_cluster, cluster_size, "SUBDIR", &result ) == 1,
	             "扫描不应报错" );
	TEST_ASSERT_EQ_INT( result.count, 1, "子目录里应该正好找到 1 个已删除候选项" );

	secret = find_by_name( &result, "ECRET.TXT" );
	TEST_ASSERT( secret != NULL, "应该找到名字里带 ECRET.TXT 的候选项" );

	if( secret != NULL )
	{
		TEST_ASSERT_EQ_INT( (int) secret->first_cluster, 6, "起始簇号应该是 6" );
		TEST_ASSERT_EQ_INT( (int) secret->file_size, 55, "文件大小应该是 55" );
		TEST_ASSERT_EQ_STR( secret->directory_path, "SUBDIR", "directory_path 应该是 SUBDIR" );
	}

	TEST_ASSERT( find_by_name( &result, "NORMAL" ) == NULL,
	             "正常存在的 NORMAL.TXT 不应该出现在已删除候选项里" );

	fat_undelete_scan_result_free( &result );
}

static void test_find_subdirectories_finds_subsubdir_inside_subdir(
     const uint8_t *subdir_cluster,
     size_t cluster_size )
{
	fat_subdirectory_list_t subdirs;

	TEST_CASE( "SUBDIR 自己的目录内容里应该正好找到 1 个子目录 SUBSUB2，不会把 SECRET.TXT/NORMAL.TXT 当成子目录" );

	memset( &subdirs, 0, sizeof( subdirs ) );

	TEST_ASSERT( fat_undelete_scanner_find_subdirectories( subdir_cluster, cluster_size, &subdirs ) == 1,
	             "查找子目录不应报错" );
	TEST_ASSERT_EQ_INT( subdirs.count, 1, "SUBDIR 下应该正好有 1 个子目录" );

	if( subdirs.count == 1 )
	{
		TEST_ASSERT_EQ_STR( subdirs.entries[ 0 ].name, "SUBSUB2", "子目录名字应该是 SUBSUB2" );
		TEST_ASSERT_EQ_INT( (int) subdirs.entries[ 0 ].first_cluster, 9, "SUBSUB2 的起始簇应该是 9" );
	}

	fat_subdirectory_list_free( &subdirs );
}

static void test_scan_subsubdirectory_finds_deleted_file_with_nested_directory_path(
     const uint8_t *subsubdir_cluster,
     size_t cluster_size )
{
	fat_undelete_scan_result_t result;
	const fat_undelete_candidate_t *deep2;

	TEST_CASE( "扫描第二层子目录簇应该找到 SUBDIR\\SUBSUB2\\DEEP2.TXT（已删除），"
	           "directory_path 应该正确标记为嵌套路径 SUBDIR\\SUBSUB2，验证不是只支持一层" );

	memset( &result, 0, sizeof( result ) );

	TEST_ASSERT( fat_undelete_scanner_scan_directory( subsubdir_cluster, cluster_size, "SUBDIR\\SUBSUB2", &result ) == 1,
	             "扫描不应报错" );
	TEST_ASSERT_EQ_INT( result.count, 1, "第二层子目录里应该正好找到 1 个已删除候选项" );

	deep2 = find_by_name( &result, "EEP2.TXT" );
	TEST_ASSERT( deep2 != NULL, "应该找到名字里带 EEP2.TXT 的候选项" );

	if( deep2 != NULL )
	{
		TEST_ASSERT_EQ_INT( (int) deep2->first_cluster, 10, "起始簇号应该是 10" );
		TEST_ASSERT_EQ_INT( (int) deep2->file_size, 59, "文件大小应该是 59" );
		TEST_ASSERT_EQ_STR( deep2->directory_path, "SUBDIR\\SUBSUB2", "directory_path 应该是嵌套路径 SUBDIR\\SUBSUB2" );
	}

	TEST_ASSERT( find_by_name( &result, "STILL2" ) == NULL,
	             "正常存在的 STILL2.TXT 不应该出现在已删除候选项里" );

	fat_undelete_scan_result_free( &result );
}

static void test_merge_appends_subdirectory_results_into_root_results(
     const uint8_t *root_dir_cluster,
     const uint8_t *subdir_cluster,
     size_t cluster_size )
{
	fat_undelete_scan_result_t root_result;
	fat_undelete_scan_result_t subdir_result;

	TEST_CASE( "把子目录扫描结果合并进根目录结果后，应该同时包含两边的候选项，根目录原有内容不丢失" );

	memset( &root_result, 0, sizeof( root_result ) );
	memset( &subdir_result, 0, sizeof( subdir_result ) );

	TEST_ASSERT( fat_undelete_scanner_scan_directory( root_dir_cluster, cluster_size, "", &root_result ) == 1,
	             "扫描根目录不应报错" );
	TEST_ASSERT( fat_undelete_scanner_scan_directory( subdir_cluster, cluster_size, "SUBDIR", &subdir_result ) == 1,
	             "扫描子目录不应报错" );

	TEST_ASSERT( fat_undelete_scan_result_append_all( &root_result, &subdir_result ) == 1,
	             "合并不应报错" );
	TEST_ASSERT_EQ_INT( root_result.count, 3, "合并后应该一共有 3 个候选项（HELLO.TXT + 中文长文件名 + SECRET.TXT）" );
	TEST_ASSERT( find_by_name( &root_result, "ELLO.TXT" ) != NULL, "合并后应该还能找到根目录的 HELLO.TXT" );
	TEST_ASSERT( find_by_name( &root_result, "ECRET.TXT" ) != NULL, "合并后应该能找到子目录的 SECRET.TXT" );

	fat_undelete_scan_result_free( &root_result );
	fat_undelete_scan_result_free( &subdir_result );
}

static void test_free_is_safe_on_empty_result( void )
{
	fat_undelete_scan_result_t result;

	TEST_CASE( "fat_undelete_scan_result_free 对一个从未填充过的 result 调用应该是安全的" );

	memset( &result, 0, sizeof( result ) );
	fat_undelete_scan_result_free( &result );

	TEST_ASSERT_EQ_INT( result.count, 0, "释放后 count 应该是 0" );
	TEST_ASSERT( result.candidates == NULL, "释放后 candidates 应该是 NULL" );
}

int main( int argc, char *argv[] )
{
	const char *fixture_path = argc > 1
	                             ? argv[ 1 ]
	                             : "../../tests/fixtures/fat32-basic-undelete/volume.img";
	size_t image_size = 0;
	uint8_t *image = read_whole_file( fixture_path, &image_size );
	fat_boot_sector_t bs;
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

	if( fat_boot_sector_parse( image, 512, &bs ) != 1 || !bs.is_valid )
	{
		fprintf( stderr, "引导扇区解析失败\n" );
		free( image );
		return 1;
	}

	root_dir_sector_number = fat_boot_sector_cluster_to_sector( &bs, bs.root_cluster );
	root_dir_byte_offset   = (size_t) root_dir_sector_number * bs.bytes_per_sector;
	cluster_size           = (size_t) bs.sectors_per_cluster * bs.bytes_per_sector;

	/* SUBDIR 的起始簇号是 fixture 构造脚本里写死的 5（和 test_find_subdirectories
	 * 的断言是同一个来源），这里直接用簇号换算字节偏移去读它的内容 */
	subdir_sector_number = fat_boot_sector_cluster_to_sector( &bs, 5 );
	subdir_byte_offset   = (size_t) subdir_sector_number * bs.bytes_per_sector;

	/* SUBSUB2（SUBDIR 下面第二层子目录）的起始簇号同样是 fixture 脚本里
	 * 写死的 9，和 test_find_subdirectories_finds_subsubdir_inside_subdir
	 * 的断言是同一个来源 */
	subsubdir_sector_number = fat_boot_sector_cluster_to_sector( &bs, 9 );
	subsubdir_byte_offset   = (size_t) subsubdir_sector_number * bs.bytes_per_sector;

	test_scan_finds_exactly_one_deleted_file( image + root_dir_byte_offset, cluster_size );
	test_find_subdirectories_finds_subdir_skips_dot_entries( image + root_dir_byte_offset, cluster_size );
	test_subdirectory_dot_entries_are_not_treated_as_subdirectories();
	test_scan_subdirectory_finds_deleted_file_with_directory_path( image + subdir_byte_offset, cluster_size );
	test_find_subdirectories_finds_subsubdir_inside_subdir( image + subdir_byte_offset, cluster_size );
	test_scan_subsubdirectory_finds_deleted_file_with_nested_directory_path( image + subsubdir_byte_offset, cluster_size );
	test_merge_appends_subdirectory_results_into_root_results(
	     image + root_dir_byte_offset, image + subdir_byte_offset, cluster_size );
	test_free_is_safe_on_empty_result();

	free( image );

	TEST_SUMMARY();
}
