/*
 * D2（P4，2026-11-05 第28步排期）：命令串拼接的 TDD 覆盖。
 * 这条规则（必须显式带分区表类型 token）是第27步实测直接得出的结论，
 * 测试用例的命名和注释有意呼应那次实测的真实命令串。
 */
#include <string.h>

#include "test_framework.h"
#include "../../core/photorec_command_builder.h"

static void test_with_partition_none_token( void )
{
	char buf[ 64 ];

	TEST_CASE( "无可识别分区表场景（第27步实测用的token）应该拼成 partition_none,search" );

	TEST_ASSERT( photorec_command_builder_build_cmd_run( "partition_none", buf, sizeof( buf ) ) == 1,
	             "应该成功" );
	TEST_ASSERT_EQ_STR( buf, "partition_none,search", "拼接结果应该匹配" );
}

static void test_with_partition_i386_token( void )
{
	char buf[ 64 ];

	TEST_CASE( "标准MBR分区表场景应该拼成 partition_i386,search" );

	TEST_ASSERT( photorec_command_builder_build_cmd_run( "partition_i386", buf, sizeof( buf ) ) == 1,
	             "应该成功" );
	TEST_ASSERT_EQ_STR( buf, "partition_i386,search", "拼接结果应该匹配" );
}

static void test_null_token_falls_back_to_bare_search( void )
{
	char buf[ 64 ];

	TEST_CASE( "token 为 NULL 时应该拼出裸的 search（调用方自己的选择，不是隐藏默认值）" );

	TEST_ASSERT( photorec_command_builder_build_cmd_run( NULL, buf, sizeof( buf ) ) == 1,
	             "应该成功" );
	TEST_ASSERT_EQ_STR( buf, "search", "拼接结果应该是裸的 search" );
}

static void test_empty_token_falls_back_to_bare_search( void )
{
	char buf[ 64 ];

	TEST_CASE( "token 为空字符串时同样应该拼出裸的 search" );

	TEST_ASSERT( photorec_command_builder_build_cmd_run( "", buf, sizeof( buf ) ) == 1,
	             "应该成功" );
	TEST_ASSERT_EQ_STR( buf, "search", "拼接结果应该是裸的 search" );
}

static void test_buffer_too_small_is_rejected( void )
{
	char tiny_buf[ 4 ];

	TEST_CASE( "输出缓冲区装不下结果时应该返回 -1，而不是截断或者溢出" );

	TEST_ASSERT( photorec_command_builder_build_cmd_run( "partition_gpt", tiny_buf, sizeof( tiny_buf ) ) == -1,
	             "应该返回 -1" );
}

static void test_null_out_buf_is_rejected( void )
{
	TEST_CASE( "out_buf 为 NULL 应该返回 -1，而不是崩溃" );

	TEST_ASSERT( photorec_command_builder_build_cmd_run( "partition_none", NULL, 64 ) == -1,
	             "应该返回 -1" );
}

int main( void )
{
	test_with_partition_none_token();
	test_with_partition_i386_token();
	test_null_token_falls_back_to_bare_search();
	test_empty_token_falls_back_to_bare_search();
	test_buffer_too_small_is_rejected();
	test_null_out_buf_is_rejected();

	TEST_SUMMARY();
}
