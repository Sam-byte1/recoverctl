/*
 * F3 的 TDD 测试：对着真实 fixture 根目录区的目录项断言
 * （entry 0 = HELLO.TXT 已删除，entry 1 = WORLD.TXT 正常，entry 2 = SUBDIR
 * 子目录，entry 3/4 = 中文长文件名的两条 VFAT 长文件名条目，entry 5 =
 * 中文长文件名对应的短名条目（已删除），entry 6 = BIGDIR 子目录，
 * entry 7 = 目录结束）。entry 2 是第12步子目录遍历追加的，entry 3/4/5
 * 是第13步 VFAT 长文件名解析追加的，entry 6 是第21步"目录单簇假设"
 * 修复新增的多簇目录场景追加的。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_framework.h"
#include "../../core/fat_boot_sector.h"
#include "../../core/fat_dir_entry.h"

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

static void test_deleted_hello_txt_entry( const uint8_t *root_dir_sector )
{
	fat_dir_entry_t entry;

	TEST_CASE( "entry 0（已模拟删除的 HELLO.TXT）应该被正确识别" );

	TEST_ASSERT( fat_dir_entry_parse( root_dir_sector, 32, &entry ) == 1, "parse 不应报错" );
	TEST_ASSERT_EQ_INT( entry.kind, FAT_DIR_ENTRY_KIND_DELETED, "应该判定为 DELETED" );
	TEST_ASSERT_EQ_STR( entry.short_name, "_ELLO.TXT",
	                     "文件名第一个字符应该是占位符 _（原字符已被 0xE5 覆盖丢失）" );
	TEST_ASSERT_EQ_INT( (int) entry.first_cluster, 3, "起始簇号应该是 3" );
	TEST_ASSERT_EQ_INT( (int) entry.file_size, 50, "文件大小应该是 50" );
	TEST_ASSERT_EQ_INT( entry.is_directory, 0, "不应该被判定为目录" );
}

static void test_normal_world_txt_entry( const uint8_t *root_dir_sector )
{
	fat_dir_entry_t entry;

	TEST_CASE( "entry 1（正常存在的 WORLD.TXT）应该被正确识别，不应误判为已删除" );

	TEST_ASSERT( fat_dir_entry_parse( root_dir_sector + 32, 32, &entry ) == 1, "parse 不应报错" );
	TEST_ASSERT_EQ_INT( entry.kind, FAT_DIR_ENTRY_KIND_NORMAL, "应该判定为 NORMAL" );
	TEST_ASSERT_EQ_STR( entry.short_name, "WORLD.TXT", "文件名应该是完整的 WORLD.TXT" );
	TEST_ASSERT_EQ_INT( (int) entry.first_cluster, 4, "起始簇号应该是 4" );
	TEST_ASSERT_EQ_INT( (int) entry.file_size, 46, "文件大小应该是 46" );
}

static void test_subdir_entry( const uint8_t *root_dir_sector )
{
	fat_dir_entry_t entry;

	TEST_CASE( "entry 2（子目录 SUBDIR）应该被正确识别为目录，不是文件" );

	TEST_ASSERT( fat_dir_entry_parse( root_dir_sector + 64, 32, &entry ) == 1, "parse 不应报错" );
	TEST_ASSERT_EQ_INT( entry.kind, FAT_DIR_ENTRY_KIND_NORMAL, "应该判定为 NORMAL（未删除）" );
	TEST_ASSERT_EQ_STR( entry.short_name, "SUBDIR", "名字应该是 SUBDIR" );
	TEST_ASSERT_EQ_INT( (int) entry.first_cluster, 5, "起始簇号应该是 5" );
	TEST_ASSERT_EQ_INT( entry.is_directory, 1, "应该被判定为目录" );
}

static void test_longname_entries_in_real_fixture( const uint8_t *root_dir_sector )
{
	fat_dir_entry_t entry;

	TEST_CASE( "entry 3/4（中文长文件名的两条 VFAT 长文件名条目）应该被识别为 LONG_NAME_PART" );

	TEST_ASSERT( fat_dir_entry_parse( root_dir_sector + 96, 32, &entry ) == 1, "parse entry 3 不应报错" );
	TEST_ASSERT_EQ_INT( entry.kind, FAT_DIR_ENTRY_KIND_LONG_NAME_PART, "entry 3 应该判定为 LONG_NAME_PART" );

	TEST_ASSERT( fat_dir_entry_parse( root_dir_sector + 128, 32, &entry ) == 1, "parse entry 4 不应报错" );
	TEST_ASSERT_EQ_INT( entry.kind, FAT_DIR_ENTRY_KIND_LONG_NAME_PART, "entry 4 应该判定为 LONG_NAME_PART" );
}

static void test_longname_short_entry_in_real_fixture( const uint8_t *root_dir_sector )
{
	fat_dir_entry_t entry;

	TEST_CASE( "entry 5（中文长文件名对应的短名条目，已模拟删除）应该被正确识别" );

	TEST_ASSERT( fat_dir_entry_parse( root_dir_sector + 160, 32, &entry ) == 1, "parse 不应报错" );
	TEST_ASSERT_EQ_INT( entry.kind, FAT_DIR_ENTRY_KIND_DELETED, "应该判定为 DELETED" );
	TEST_ASSERT_EQ_INT( (int) entry.first_cluster, 8, "起始簇号应该是 8" );
}

static void test_bigdir_entry( const uint8_t *root_dir_sector )
{
	fat_dir_entry_t entry;

	TEST_CASE( "entry 6（BIGDIR 子目录，2026-11-05 第21步新增的多簇目录场景）应该被正确识别为目录" );

	TEST_ASSERT( fat_dir_entry_parse( root_dir_sector + 192, 32, &entry ) == 1, "parse 不应报错" );
	TEST_ASSERT_EQ_INT( entry.kind, FAT_DIR_ENTRY_KIND_NORMAL, "BIGDIR 是正常存在(未删除)的目录" );
	TEST_ASSERT( entry.is_directory, "应该被识别为目录，不是文件" );
	TEST_ASSERT_EQ_INT( (int) entry.first_cluster, 12, "起始簇号应该是 12" );
}

static void test_end_of_directory_entry( const uint8_t *root_dir_sector )
{
	fat_dir_entry_t entry;

	TEST_CASE( "entry 7（根目录剩余部分，全零）应该被识别为目录结束标记" );

	TEST_ASSERT( fat_dir_entry_parse( root_dir_sector + 224, 32, &entry ) == 1, "parse 不应报错" );
	TEST_ASSERT_EQ_INT( entry.kind, FAT_DIR_ENTRY_KIND_END_OF_DIRECTORY,
	                     "应该判定为 END_OF_DIRECTORY" );
}

static void test_long_name_part_extracts_13_utf16_characters( void )
{
	fat_dir_entry_t entry;
	uint8_t raw[ 32 ];

	TEST_CASE( "VFAT 长文件名条目应该被识别为 LONG_NAME_PART，并正确取出 13 个 UTF-16LE 字符（三段拼接）" );

	memset( raw, 0, sizeof( raw ) );
	raw[ 0 ] = 0x41;            /* 序号字节（本轮不使用这个值，随便填一个非零值） */
	/* Name1：偏移 1-10，5 个字符 "ABCDE" */
	memcpy( raw + 1, "A\0B\0C\0D\0E\0", 10 );
	raw[ 11 ] = 0x0F;           /* ATTR_LONG_NAME */
	raw[ 12 ] = 0x00;
	raw[ 13 ] = 0x99;           /* 短名校验和，本轮不使用 */
	/* Name2：偏移 14-25，6 个字符 "FGHIJK" */
	memcpy( raw + 14, "F\0G\0H\0I\0J\0K\0", 12 );
	raw[ 26 ] = 0x00;
	raw[ 27 ] = 0x00;
	/* Name3：偏移 28-31，2 个字符 "LM" */
	memcpy( raw + 28, "L\0M\0", 4 );

	TEST_ASSERT( fat_dir_entry_parse( raw, sizeof( raw ), &entry ) == 1, "parse 不应报错" );
	TEST_ASSERT_EQ_INT( entry.kind, FAT_DIR_ENTRY_KIND_LONG_NAME_PART, "应该判定为 LONG_NAME_PART" );
	TEST_ASSERT( memcmp( entry.long_name_part_utf16le, "A\0B\0C\0D\0E\0F\0G\0H\0I\0J\0K\0L\0M\0", 26 ) == 0,
	             "应该正确拼出三段共 13 个字符：ABCDEFGHIJKLM" );
}

static void test_rejects_too_short_input( void )
{
	fat_dir_entry_t entry;
	uint8_t tiny_buffer[ 10 ];

	TEST_CASE( "输入长度不够 32 字节时应该拒绝，而不是越界读取" );

	TEST_ASSERT( fat_dir_entry_parse( tiny_buffer, sizeof( tiny_buffer ), &entry ) == -1,
	             "长度不足应该返回 -1" );
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

	if( image == NULL )
	{
		fprintf( stderr, "无法读取测试镜像: %s\n", fixture_path );
		return 1;
	}

	if( fat_boot_sector_parse( image, 512, &bs ) != 1 || !bs.is_valid )
	{
		fprintf( stderr, "引导扇区解析失败，F2 可能有问题\n" );
		free( image );
		return 1;
	}

	root_dir_sector_number = fat_boot_sector_cluster_to_sector( &bs, bs.root_cluster );
	root_dir_byte_offset   = (size_t) root_dir_sector_number * bs.bytes_per_sector;

	test_deleted_hello_txt_entry( image + root_dir_byte_offset );
	test_normal_world_txt_entry( image + root_dir_byte_offset );
	test_subdir_entry( image + root_dir_byte_offset );
	test_longname_entries_in_real_fixture( image + root_dir_byte_offset );
	test_longname_short_entry_in_real_fixture( image + root_dir_byte_offset );
	test_bigdir_entry( image + root_dir_byte_offset );
	test_end_of_directory_entry( image + root_dir_byte_offset );
	test_long_name_part_extracts_13_utf16_characters();
	test_rejects_too_short_input();

	free( image );

	TEST_SUMMARY();
}
