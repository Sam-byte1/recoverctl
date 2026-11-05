/*
 * F2 的 TDD 测试：对着 engine/tests/fixtures/fat32-basic-undelete/volume.img
 * 的真实引导扇区字节断言——这个镜像已经验证过能被 macOS 自带的 FAT 驱动
 * 正确挂载识别，所以这里的断言值不是我们自己拍脑袋编的，是有独立交叉验证的。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_framework.h"
#include "../../core/fat_boot_sector.h"

static uint8_t *read_file_prefix( const char *path, size_t read_size )
{
	FILE *f = fopen( path, "rb" );
	uint8_t *buffer;

	if( f == NULL )
	{
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

static void test_parses_real_fixture_boot_sector( const uint8_t *boot_sector )
{
	fat_boot_sector_t bs;

	TEST_CASE( "解析真实 fixture 的引导扇区，字段应该和构造脚本里的参数完全一致" );

	TEST_ASSERT( fat_boot_sector_parse( boot_sector, 512, &bs ) == 1, "parse 不应报错" );
	TEST_ASSERT( bs.is_valid == 1, "应该被判定为合法的 FAT32 引导扇区" );
	TEST_ASSERT_EQ_INT( bs.bytes_per_sector, 512, "bytes_per_sector 应该是 512" );
	TEST_ASSERT_EQ_INT( bs.sectors_per_cluster, 1, "sectors_per_cluster 应该是 1" );
	TEST_ASSERT_EQ_INT( bs.reserved_sector_count, 32, "reserved_sector_count 应该是 32" );
	TEST_ASSERT_EQ_INT( bs.number_of_fats, 2, "number_of_fats 应该是 2" );
	TEST_ASSERT_EQ_INT( (int) bs.fat_size_sectors, 547, "fat_size_sectors 应该是 547" );
	TEST_ASSERT_EQ_INT( (int) bs.root_cluster, 2, "root_cluster 应该是 2" );
}

static void test_derived_sector_calculations( const uint8_t *boot_sector )
{
	fat_boot_sector_t bs;

	TEST_CASE( "派生计算：FAT 起始扇区、数据区起始扇区、簇号转扇区号" );

	TEST_ASSERT( fat_boot_sector_parse( boot_sector, 512, &bs ) == 1, "parse 不应报错" );
	TEST_ASSERT_EQ_INT( (int) fat_boot_sector_fat_start_sector( &bs ), 32,
	                     "第一份 FAT 表起始扇区应该是 32（等于 reserved_sector_count）" );
	TEST_ASSERT_EQ_INT( (int) fat_boot_sector_data_start_sector( &bs ), 1126,
	                     "数据区起始扇区应该是 32 + 2*547 = 1126" );
	TEST_ASSERT_EQ_INT( (int) fat_boot_sector_cluster_to_sector( &bs, 2 ), 1126,
	                     "簇 2（根目录）应该对应扇区 1126" );
	TEST_ASSERT_EQ_INT( (int) fat_boot_sector_cluster_to_sector( &bs, 3 ), 1127,
	                     "簇 3（HELLO.TXT）应该对应扇区 1127" );
	TEST_ASSERT_EQ_INT( (int) fat_boot_sector_cluster_to_sector( &bs, 4 ), 1128,
	                     "簇 4（WORLD.TXT）应该对应扇区 1128" );
}

static void test_rejects_too_short_input( void )
{
	fat_boot_sector_t bs;
	uint8_t tiny_buffer[ 10 ];

	TEST_CASE( "输入长度不够 512 字节时应该拒绝，而不是越界读取" );

	TEST_ASSERT( fat_boot_sector_parse( tiny_buffer, sizeof( tiny_buffer ), &bs ) == -1,
	             "长度不足应该返回 -1" );
}

static void test_rejects_garbage_data( void )
{
	fat_boot_sector_t bs;
	uint8_t garbage[ 512 ];

	TEST_CASE( "对一段随便的非 FAT32 数据，应该解析出 is_valid = 0，而不是报错或者误判" );

	memset( garbage, 0x41, sizeof( garbage ) );

	TEST_ASSERT( fat_boot_sector_parse( garbage, sizeof( garbage ), &bs ) == 1,
	             "parse 本身不应该因为数据不合法就报错" );
	TEST_ASSERT_EQ_INT( bs.is_valid, 0, "全是垃圾数据不应该被判定为合法 FAT32" );
}

int main( int argc, char *argv[] )
{
	const char *fixture_path = argc > 1
	                             ? argv[ 1 ]
	                             : "../../tests/fixtures/fat32-basic-undelete/volume.img";
	uint8_t *boot_sector = read_file_prefix( fixture_path, 512 );

	if( boot_sector == NULL )
	{
		fprintf( stderr, "无法读取测试镜像: %s\n", fixture_path );
		return 1;
	}

	test_parses_real_fixture_boot_sector( boot_sector );
	test_derived_sector_calculations( boot_sector );
	test_rejects_too_short_input();
	test_rejects_garbage_data();

	free( boot_sector );

	TEST_SUMMARY();
}
