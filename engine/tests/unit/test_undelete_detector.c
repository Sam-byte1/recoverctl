/*
 * Task #3 的 TDD 测试：对 undelete_detector_inspect 这个纯函数做断言。
 * 测试数据来自 engine/tests/fixtures/ntfs-basic-undelete/（见该目录 README.md）。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libfsntfs.h>

#include "test_framework.h"
#include "../../core/undelete_detector.h"

static libfsntfs_volume_t *open_fixture( const char *path )
{
	libfsntfs_volume_t *volume = NULL;
	libfsntfs_error_t *error   = NULL;

	if( libfsntfs_volume_initialize( &volume, &error ) != 1 )
	{
		return NULL;
	}
	if( libfsntfs_volume_open( volume, path, LIBFSNTFS_OPEN_READ, &error ) != 1 )
	{
		libfsntfs_error_free( &error );
		libfsntfs_volume_free( &volume, NULL );
		return NULL;
	}
	return volume;
}

/* entry 65 = a_file，正常存在的文件，不应被判定为已删除 */
static void test_allocated_entry_is_not_recoverable( libfsntfs_volume_t *volume )
{
	libfsntfs_file_entry_t *file_entry = NULL;
	libfsntfs_error_t *error           = NULL;
	undelete_candidate_t result;

	TEST_CASE( "entry 65（正常存在的文件）不应被判定为已删除" );

	TEST_ASSERT( libfsntfs_volume_get_file_entry_by_index( volume, 65, &file_entry, &error ) == 1,
	             "应该能拿到 entry 65" );
	TEST_ASSERT( undelete_detector_inspect( file_entry, 65, &result, &error ) == 1,
	             "inspect 不应报错" );
	TEST_ASSERT_EQ_INT( result.is_deleted_recoverable, 0,
	                     "entry 65 是正常文件，不应判定为已删除" );
	TEST_ASSERT_EQ_INT( result.has_file_size, 0,
	                     "不是候选项的记录不应该带着一个 has_file_size=1，避免调用方误用" );

	libfsntfs_file_entry_free( &file_entry, &error );
}

/* entry 66 = 被模拟删除的 passwords.txt */
static void test_deleted_passwords_txt_is_recoverable( libfsntfs_volume_t *volume )
{
	libfsntfs_file_entry_t *file_entry = NULL;
	libfsntfs_error_t *error           = NULL;
	undelete_candidate_t result;

	TEST_CASE( "entry 66（被模拟删除的 passwords.txt）应被正确识别" );

	TEST_ASSERT( libfsntfs_volume_get_file_entry_by_index( volume, 66, &file_entry, &error ) == 1,
	             "应该能拿到 entry 66" );
	TEST_ASSERT( undelete_detector_inspect( file_entry, 66, &result, &error ) == 1,
	             "inspect 不应报错" );
	TEST_ASSERT_EQ_INT( result.is_deleted_recoverable, 1,
	                     "entry 66 应判定为已删除且可恢复" );
	TEST_ASSERT_EQ_STR( result.name, "passwords.txt",
	                     "文件名应该是 passwords.txt" );
	TEST_ASSERT_EQ_INT( (int) result.mft_entry_index, 66,
	                     "mft_entry_index 应该原样回传" );
	TEST_ASSERT_EQ_INT( (int) result.parent_mft_index, 5,
	                     "passwords.txt 直接在根目录下，父目录 MFT 索引应该是 5（NTFS 根目录固定索引）" );
	TEST_ASSERT_EQ_INT( result.has_file_size, 1,
	                     "passwords.txt 应该能成功读到文件大小" );
	TEST_ASSERT_EQ_INT( (int) result.file_size, 116,
	                     "passwords.txt.original 实际字节数是 116（ls -la 核实），应该原样读出" );

	libfsntfs_file_entry_free( &file_entry, &error );
}

/* entry 67 = 被模拟删除的 a_directory\another_file */
static void test_deleted_another_file_is_recoverable( libfsntfs_volume_t *volume )
{
	libfsntfs_file_entry_t *file_entry = NULL;
	libfsntfs_error_t *error           = NULL;
	undelete_candidate_t result;

	TEST_CASE( "entry 67（被模拟删除的 another_file）应被正确识别" );

	TEST_ASSERT( libfsntfs_volume_get_file_entry_by_index( volume, 67, &file_entry, &error ) == 1,
	             "应该能拿到 entry 67" );
	TEST_ASSERT( undelete_detector_inspect( file_entry, 67, &result, &error ) == 1,
	             "inspect 不应报错" );
	TEST_ASSERT_EQ_INT( result.is_deleted_recoverable, 1,
	                     "entry 67 应判定为已删除且可恢复" );
	TEST_ASSERT_EQ_STR( result.name, "another_file",
	                     "文件名应该是 another_file" );
	TEST_ASSERT_EQ_INT( (int) result.parent_mft_index, 64,
	                     "another_file 在 a_directory 里，父目录 MFT 索引应该是 64（a_directory 的 MFT 索引）" );
	TEST_ASSERT_EQ_INT( result.has_file_size, 1,
	                     "another_file 应该能成功读到文件大小" );
	TEST_ASSERT_EQ_INT( (int) result.file_size, 22,
	                     "another_file.original 实际字节数是 22（ls -la 核实），应该原样读出" );

	libfsntfs_file_entry_free( &file_entry, &error );
}

/* entry 27 = 从未使用过的空槽位，不应被误判为已删除文件 */
static void test_never_used_slot_is_not_recoverable( libfsntfs_volume_t *volume )
{
	libfsntfs_file_entry_t *file_entry = NULL;
	libfsntfs_error_t *error           = NULL;
	undelete_candidate_t result;
	int get_result;

	TEST_CASE( "entry 27（从未使用过的空槽位）不应被误判为已删除文件" );

	get_result = libfsntfs_volume_get_file_entry_by_index( volume, 27, &file_entry, &error );

	if( get_result != 1 )
	{
		/* 某些情况下库会直接拒绝为完全空的槽位构造 file_entry，
		 * 这本身也是"不算已删除文件"的合理表现 */
		libfsntfs_error_free( &error );
		TEST_ASSERT( 1, "entry 27 无法构造 file_entry 也是合理结果（空槽位）" );
		return;
	}

	TEST_ASSERT( undelete_detector_inspect( file_entry, 27, &result, &error ) == 1,
	             "inspect 不应报错" );
	TEST_ASSERT_EQ_INT( result.is_deleted_recoverable, 0,
	                     "空槽位不应被误判为已删除文件" );

	libfsntfs_file_entry_free( &file_entry, &error );
}

/* entry 16 = 从未初始化的垃圾槽位（整条清零，连 FILE 魔数都没有）。
 * 2026-11-04 在真实 Windows 格式化的 NTFS 卷上做 Task #9 测试时发现的真实场景：
 * 真实磁盘上这类区域是彻底的全零字节，libfsntfs 对它判断 is_allocated 会报错，
 * 不能把这个错误当成扫描失败，要优雅地当作"不可恢复"处理。 */
static void test_uninitialized_slot_is_not_recoverable_not_an_error( libfsntfs_volume_t *volume )
{
	libfsntfs_file_entry_t *file_entry = NULL;
	libfsntfs_error_t *error           = NULL;
	undelete_candidate_t result;
	int get_result;

	TEST_CASE( "entry 16（真实磁盘上常见的从未初始化槽位，整条全零）不应导致报错，应优雅判定为不可恢复" );

	get_result = libfsntfs_volume_get_file_entry_by_index( volume, 16, &file_entry, &error );

	if( get_result != 1 )
	{
		libfsntfs_error_free( &error );
		TEST_ASSERT( 1, "entry 16 无法构造 file_entry 也是合理结果" );
		return;
	}

	TEST_ASSERT( undelete_detector_inspect( file_entry, 16, &result, &error ) == 1,
	             "inspect 不应把这种垃圾数据升级成报错" );
	TEST_ASSERT_EQ_INT( result.is_deleted_recoverable, 0,
	                     "全零的垃圾槽位不应被判定为可恢复" );

	libfsntfs_file_entry_free( &file_entry, &error );
}

int main( int argc, char *argv[] )
{
	const char *fixture_path = argc > 1
	                             ? argv[ 1 ]
	                             : "../../tests/fixtures/ntfs-basic-undelete/volume.img";

	libfsntfs_volume_t *volume = open_fixture( fixture_path );

	if( volume == NULL )
	{
		fprintf( stderr, "无法打开测试镜像: %s\n", fixture_path );
		return 1;
	}

	test_allocated_entry_is_not_recoverable( volume );
	test_deleted_passwords_txt_is_recoverable( volume );
	test_deleted_another_file_is_recoverable( volume );
	test_never_used_slot_is_not_recoverable( volume );
	test_uninitialized_slot_is_not_recoverable_not_an_error( volume );

	{
		libfsntfs_error_t *error = NULL;
		libfsntfs_volume_close( volume, &error );
		libfsntfs_volume_free( &volume, &error );
	}

	TEST_SUMMARY();
}
