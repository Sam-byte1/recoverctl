/*
 * exFAT F3 的 TDD 测试：对着 engine/tests/fixtures/exfat-basic-undelete/volume.img
 * 根目录簇（簇 4，字节偏移 21*512=10752）的真实条目断言。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_framework.h"
#include "../../core/exfat_dir_entry.h"

#define ROOT_DIR_BYTE_OFFSET ( 21 * 512 )
#define ROOT_DIR_CLUSTER_SIZE ( 4 * 512 )

static uint8_t *read_file_region( const char *path, long offset, size_t read_size )
{
	FILE *f = fopen( path, "rb" );
	uint8_t *buffer;

	if( f == NULL )
	{
		return NULL;
	}
	if( fseek( f, offset, SEEK_SET ) != 0 )
	{
		fclose( f );
		return NULL;
	}
	buffer = (uint8_t *) malloc( read_size );
	if( buffer == NULL )
	{
		fclose( f );
		return NULL;
	}
	if( fread( buffer, 1, read_size, f ) != read_size )
	{
		free( buffer );
		fclose( f );
		return NULL;
	}
	fclose( f );
	return buffer;
}

static void test_bitmap_and_upcase_entries_are_skipped( const uint8_t *root_dir )
{
	exfat_dir_entry_set_t set;

	TEST_CASE( "entry 0（位图）/entry 1（大写表）应该被识别为 SKIPPED，各消费 1 条" );

	TEST_ASSERT( exfat_dir_entry_set_parse( root_dir, ROOT_DIR_CLUSTER_SIZE, 0, &set ) == 1, "parse entry 0 不应报错" );
	TEST_ASSERT_EQ_INT( set.kind, EXFAT_DIR_ENTRY_SET_KIND_SKIPPED, "entry 0 应该是 SKIPPED（位图）" );
	TEST_ASSERT_EQ_INT( (int) set.consumed_entries, 1, "entry 0 应该只消费 1 条" );

	TEST_ASSERT( exfat_dir_entry_set_parse( root_dir, ROOT_DIR_CLUSTER_SIZE, 1, &set ) == 1, "parse entry 1 不应报错" );
	TEST_ASSERT_EQ_INT( set.kind, EXFAT_DIR_ENTRY_SET_KIND_SKIPPED, "entry 1 应该是 SKIPPED（大写表）" );
	TEST_ASSERT_EQ_INT( (int) set.consumed_entries, 1, "entry 1 应该只消费 1 条" );
}

static void test_hello_txt_is_normal_file( const uint8_t *root_dir )
{
	exfat_dir_entry_set_t set;

	TEST_CASE( "entry 2（HELLO.TXT，正常存在）应该被正确识别，消费 3 条目录项" );

	TEST_ASSERT( exfat_dir_entry_set_parse( root_dir, ROOT_DIR_CLUSTER_SIZE, 2, &set ) == 1, "parse 不应报错" );
	TEST_ASSERT_EQ_INT( set.kind, EXFAT_DIR_ENTRY_SET_KIND_NORMAL_FILE, "应该识别为正常文件" );
	TEST_ASSERT_EQ_INT( (int) set.consumed_entries, 3, "File+StreamExt+1个Name，应该消费 3 条" );
	TEST_ASSERT_EQ_STR( set.name, "HELLO.TXT", "文件名应该是 HELLO.TXT" );
	TEST_ASSERT_EQ_INT( (int) set.first_cluster, 5, "起始簇应该是 5" );
	TEST_ASSERT_EQ_INT( (int) set.file_size, 37, "文件大小应该是 37 字节" );
	TEST_ASSERT( set.no_fat_chain == 1, "fixture 构造时设置了 NoFatChain 标志，应该识别为连续存储" );
	TEST_ASSERT( set.is_directory == 0, "不应该被误判为目录" );
}

static void test_secret_txt_is_deleted_with_full_name_preserved( const uint8_t *root_dir )
{
	exfat_dir_entry_set_t set;

	TEST_CASE( "entry 5（SECRET.TXT，已模拟删除）应该被识别为已删除，且文件名完整保留（不像 FAT32 那样被破坏首字符）" );

	TEST_ASSERT( exfat_dir_entry_set_parse( root_dir, ROOT_DIR_CLUSTER_SIZE, 5, &set ) == 1, "parse 不应报错" );
	TEST_ASSERT_EQ_INT( set.kind, EXFAT_DIR_ENTRY_SET_KIND_DELETED_FILE, "应该识别为已删除文件" );
	TEST_ASSERT_EQ_INT( (int) set.consumed_entries, 3, "File+StreamExt+1个Name，应该消费 3 条" );
	TEST_ASSERT_EQ_STR( set.name, "SECRET.TXT", "exFAT 删除后文件名应该完整保留，不像 FAT32 首字符被破坏" );
	TEST_ASSERT_EQ_INT( (int) set.first_cluster, 6, "起始簇应该是 6" );
	TEST_ASSERT_EQ_INT( (int) set.file_size, 66, "文件大小应该是 66 字节" );
}

static void test_subdir_is_normal_directory( const uint8_t *root_dir )
{
	exfat_dir_entry_set_t set;

	TEST_CASE( "entry 8（SUBDIR 子目录）应该被正确识别为目录，不是文件" );

	TEST_ASSERT( exfat_dir_entry_set_parse( root_dir, ROOT_DIR_CLUSTER_SIZE, 8, &set ) == 1, "parse 不应报错" );
	TEST_ASSERT_EQ_INT( set.kind, EXFAT_DIR_ENTRY_SET_KIND_NORMAL_FILE, "应该识别为正常（未删除）条目" );
	TEST_ASSERT_EQ_INT( (int) set.consumed_entries, 3, "File+StreamExt+1个Name，应该消费 3 条" );
	TEST_ASSERT_EQ_STR( set.name, "SUBDIR", "名字应该是 SUBDIR" );
	TEST_ASSERT_EQ_INT( (int) set.first_cluster, 7, "起始簇应该是 7" );
	TEST_ASSERT( set.is_directory == 1, "应该被识别为目录" );
}

static void test_chinese_name_is_correctly_decoded( const uint8_t *root_dir )
{
	exfat_dir_entry_set_t set;

	TEST_CASE( "entry 11（中文文件名'测试文件.txt'，已模拟删除）应该被正确解码，不是垃圾字节" );

	TEST_ASSERT( exfat_dir_entry_set_parse( root_dir, ROOT_DIR_CLUSTER_SIZE, 11, &set ) == 1, "parse 不应报错" );
	TEST_ASSERT_EQ_INT( set.kind, EXFAT_DIR_ENTRY_SET_KIND_DELETED_FILE, "应该识别为已删除文件" );
	TEST_ASSERT_EQ_STR( set.name, "测试文件.txt",
	                     "中文文件名应该被正确转码成 UTF-8（2026-11-05 小树裁决第13步修复前，这里会是垃圾字节）" );
	TEST_ASSERT_EQ_INT( (int) set.first_cluster, 10, "起始簇应该是 10" );
}

static void test_frag_entry_has_fat_chain_flag_cleared( const uint8_t *root_dir )
{
	exfat_dir_entry_set_t set;

	TEST_CASE( "entry 14（FRAG.BIN，已模拟删除、碎片化存储）应该识别出 no_fat_chain=0"
	           "（2026-11-05 小树裁决第16步：碎片化文件恢复需要这个标志位）" );

	TEST_ASSERT( exfat_dir_entry_set_parse( root_dir, ROOT_DIR_CLUSTER_SIZE, 14, &set ) == 1, "parse 不应报错" );
	TEST_ASSERT_EQ_INT( set.kind, EXFAT_DIR_ENTRY_SET_KIND_DELETED_FILE, "应该识别为已删除文件" );
	TEST_ASSERT_EQ_STR( set.name, "FRAG.BIN", "文件名应该是 FRAG.BIN" );
	TEST_ASSERT_EQ_INT( (int) set.first_cluster, 20, "起始簇应该是 20" );
	TEST_ASSERT_EQ_INT( set.no_fat_chain, 0, "碎片化文件的 no_fat_chain 应该是 0（需要跟随 FAT 链）" );
}

static void test_bigdir_entry( const uint8_t *root_dir )
{
	exfat_dir_entry_set_t set;

	TEST_CASE( "entry 17（BIGDIR 子目录，2026-11-05 第21步新增的多簇目录场景）应该被正确识别为目录" );

	TEST_ASSERT( exfat_dir_entry_set_parse( root_dir, ROOT_DIR_CLUSTER_SIZE, 17, &set ) == 1, "parse 不应报错" );
	TEST_ASSERT_EQ_INT( set.kind, EXFAT_DIR_ENTRY_SET_KIND_NORMAL_FILE, "BIGDIR 是正常存在(未删除)的目录" );
	TEST_ASSERT_EQ_STR( set.name, "BIGDIR", "文件名应该是 BIGDIR" );
	TEST_ASSERT( set.is_directory, "应该被识别为目录，不是文件" );
	TEST_ASSERT_EQ_INT( (int) set.first_cluster, 24, "起始簇应该是 24" );
	TEST_ASSERT_EQ_INT( set.no_fat_chain, 0, "BIGDIR 是碎片化多簇目录，no_fat_chain 应该是 0" );
}

static void test_end_of_directory_marker( const uint8_t *root_dir )
{
	exfat_dir_entry_set_t set;

	TEST_CASE( "entry 20（HELLO/SECRET/SUBDIR/中文文件名/FRAG.BIN/BIGDIR 六个 entry set 之后）应该是目录结束标记" );

	TEST_ASSERT( exfat_dir_entry_set_parse( root_dir, ROOT_DIR_CLUSTER_SIZE, 20, &set ) == 1, "parse 不应报错" );
	TEST_ASSERT_EQ_INT( set.kind, EXFAT_DIR_ENTRY_SET_KIND_END_OF_DIRECTORY, "entry 20 应该是目录结束标记" );
	TEST_ASSERT_EQ_INT( (int) set.consumed_entries, 1, "结束标记只消费 1 条" );
}

static void test_rejects_too_short_input( void )
{
	exfat_dir_entry_set_t set;
	uint8_t tiny_buffer[ 10 ];

	TEST_CASE( "entries_data_size 小于 1 条目录项大小时应该拒绝，而不是越界读取" );

	TEST_ASSERT( exfat_dir_entry_set_parse( tiny_buffer, sizeof( tiny_buffer ), 0, &set ) == -1,
	             "长度不足应该返回 -1" );
}

int main( int argc, char *argv[] )
{
	const char *fixture_path = argc > 1
	                             ? argv[ 1 ]
	                             : "../../tests/fixtures/exfat-basic-undelete/volume.img";
	uint8_t *root_dir = read_file_region( fixture_path, ROOT_DIR_BYTE_OFFSET, ROOT_DIR_CLUSTER_SIZE );

	if( root_dir == NULL )
	{
		fprintf( stderr, "无法读取测试镜像: %s\n", fixture_path );
		return 1;
	}

	test_bitmap_and_upcase_entries_are_skipped( root_dir );
	test_hello_txt_is_normal_file( root_dir );
	test_secret_txt_is_deleted_with_full_name_preserved( root_dir );
	test_subdir_is_normal_directory( root_dir );
	test_chinese_name_is_correctly_decoded( root_dir );
	test_frag_entry_has_fat_chain_flag_cleared( root_dir );
	test_bigdir_entry( root_dir );
	test_end_of_directory_marker( root_dir );
	test_rejects_too_short_input();

	free( root_dir );

	TEST_SUMMARY();
}
