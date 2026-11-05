#include "test_framework.h"
#include "../../core/ntfs_file_reference.h"

static void test_extracts_low_48_bits( void )
{
	TEST_CASE( "应该只取低 48 位作为 MFT 索引，忽略高 16 位的序列号" );

	TEST_ASSERT( ntfs_file_reference_to_mft_index( 0x0001000000000005ULL ) == 5,
	             "序列号=1、索引=5 的引用应该解出索引 5" );
	TEST_ASSERT( ntfs_file_reference_to_mft_index( 0x000F000000000040ULL ) == 64,
	             "序列号=0xF、索引=64(0x40) 的引用应该解出索引 64" );
	TEST_ASSERT( ntfs_file_reference_to_mft_index( 0x0000000000000043ULL ) == 67,
	             "序列号=0 的引用应该直接解出索引 67(0x43)" );
}

static void test_zero_reference( void )
{
	TEST_CASE( "全零的引用应该解出索引 0" );

	TEST_ASSERT( ntfs_file_reference_to_mft_index( 0 ) == 0, "全零应该解出 0" );
}

int main( void )
{
	test_extracts_low_48_bits();
	test_zero_reference();

	TEST_SUMMARY();
}
