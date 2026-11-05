/*
 * exFAT F2 的 TDD 测试：对着 engine/tests/fixtures/exfat-basic-undelete/volume.img
 * 的真实字节断言——这些数值是 make_fixture.py 里写死的构造参数，不是拍脑袋编的。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_framework.h"
#include "../../core/exfat_boot_sector.h"

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
	exfat_boot_sector_t bs;

	TEST_CASE( "解析真实 fixture 的主引导扇区，字段应该和构造脚本里的参数完全一致" );

	TEST_ASSERT( exfat_boot_sector_parse( boot_sector, 512, &bs ) == 1, "parse 不应报错" );
	TEST_ASSERT( bs.is_valid == 1, "应该被判定为合法的 exFAT 引导扇区" );
	TEST_ASSERT_EQ_INT( (int) bs.bytes_per_sector, 512, "bytes_per_sector 应该是 512" );
	TEST_ASSERT_EQ_INT( (int) bs.sectors_per_cluster, 4, "sectors_per_cluster 应该是 4" );
	TEST_ASSERT_EQ_INT( (int) bs.number_of_fats, 1, "number_of_fats 应该是 1" );
	TEST_ASSERT_EQ_INT( (int) bs.fat_offset_sectors, 12, "fat_offset_sectors 应该是 12" );
	TEST_ASSERT_EQ_INT( (int) bs.fat_length_sectors, 1, "fat_length_sectors 应该是 1" );
	TEST_ASSERT_EQ_INT( (int) bs.cluster_heap_offset_sectors, 13, "cluster_heap_offset_sectors 应该是 13" );
	/* 2026-11-05 第21步新增 BIGDIR 多簇目录场景，fixture 的 CLUSTER_COUNT
	 * 从 24 增加到 28（新增簇 24/26/27/28），volume_length_sectors 跟着
	 * 公式自动变化，这两个数字要跟着 fixture 脚本同步更新，不是巧合 */
	TEST_ASSERT_EQ_INT( (int) bs.cluster_count, 28, "cluster_count 应该是 28" );
	TEST_ASSERT_EQ_INT( (int) bs.first_cluster_of_root_directory, 4, "first_cluster_of_root_directory 应该是 4" );
	TEST_ASSERT_EQ_INT( (int) bs.volume_length_sectors, 136, "volume_length_sectors 应该是 136" );
}

static void test_cluster_to_sector( const uint8_t *boot_sector )
{
	exfat_boot_sector_t bs;

	TEST_CASE( "簇号转扇区号：exFAT 直接用 ClusterHeapOffset，不需要像 FAT32 那样推算" );

	TEST_ASSERT( exfat_boot_sector_parse( boot_sector, 512, &bs ) == 1, "parse 不应报错" );
	TEST_ASSERT_EQ_INT( (int) exfat_boot_sector_cluster_to_sector( &bs, 2 ), 13, "簇 2（位图）应该对应扇区 13" );
	TEST_ASSERT_EQ_INT( (int) exfat_boot_sector_cluster_to_sector( &bs, 3 ), 17, "簇 3（大写表）应该对应扇区 17" );
	TEST_ASSERT_EQ_INT( (int) exfat_boot_sector_cluster_to_sector( &bs, 4 ), 21, "簇 4（根目录）应该对应扇区 21" );
	TEST_ASSERT_EQ_INT( (int) exfat_boot_sector_cluster_to_sector( &bs, 5 ), 25, "簇 5（HELLO.TXT）应该对应扇区 25" );
	TEST_ASSERT_EQ_INT( (int) exfat_boot_sector_cluster_to_sector( &bs, 6 ), 29, "簇 6（SECRET.TXT）应该对应扇区 29" );
}

static void test_rejects_too_short_input( void )
{
	exfat_boot_sector_t bs;
	uint8_t tiny_buffer[ 10 ];

	TEST_CASE( "输入长度不够 512 字节时应该拒绝，而不是越界读取" );

	TEST_ASSERT( exfat_boot_sector_parse( tiny_buffer, sizeof( tiny_buffer ), &bs ) == -1,
	             "长度不足应该返回 -1" );
}

static void test_rejects_garbage_data( void )
{
	exfat_boot_sector_t bs;
	uint8_t garbage[ 512 ];

	TEST_CASE( "对一段随便的非 exFAT 数据，应该解析出 is_valid = 0，而不是报错或者误判" );

	memset( garbage, 0x41, sizeof( garbage ) );

	TEST_ASSERT( exfat_boot_sector_parse( garbage, sizeof( garbage ), &bs ) == 1,
	             "parse 本身不应该因为数据不合法就报错" );
	TEST_ASSERT_EQ_INT( bs.is_valid, 0, "全是垃圾数据不应该被判定为合法 exFAT" );
}

int main( int argc, char *argv[] )
{
	const char *fixture_path = argc > 1
	                             ? argv[ 1 ]
	                             : "../../tests/fixtures/exfat-basic-undelete/volume.img";
	uint8_t *boot_sector = read_file_prefix( fixture_path, 512 );

	if( boot_sector == NULL )
	{
		fprintf( stderr, "无法读取测试镜像: %s\n", fixture_path );
		return 1;
	}

	test_parses_real_fixture_boot_sector( boot_sector );
	test_cluster_to_sector( boot_sector );
	test_rejects_too_short_input();
	test_rejects_garbage_data();

	free( boot_sector );

	TEST_SUMMARY();
}
