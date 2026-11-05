#include <string.h>

#include "test_framework.h"
#include "../../core/path_builder.h"

static void test_prepend_to_empty_path( void )
{
	char path[ 64 ] = "";

	TEST_CASE( "往空路径拼第一段，结果应该就是这段本身，不应该多出反斜杠" );

	TEST_ASSERT( path_builder_prepend_segment( path, sizeof( path ), "file.txt" ) == 1,
	             "拼接不应报错" );
	TEST_ASSERT_EQ_STR( path, "file.txt", "结果应该是 file.txt" );
}

static void test_prepend_builds_nested_path_from_leaf_to_root( void )
{
	char path[ 64 ] = "";

	TEST_CASE( "模拟从叶子往根走：先拼文件名，再依次拼每一层父目录，顺序应该正确" );

	TEST_ASSERT( path_builder_prepend_segment( path, sizeof( path ), "another_file" ) == 1, "第一次拼接不应报错" );
	TEST_ASSERT_EQ_STR( path, "another_file", "拼接后应该是 another_file" );

	TEST_ASSERT( path_builder_prepend_segment( path, sizeof( path ), "a_directory" ) == 1, "第二次拼接不应报错" );
	TEST_ASSERT_EQ_STR( path, "a_directory\\another_file", "拼接后应该是 a_directory\\another_file" );
}

static void test_rejects_when_result_too_long( void )
{
	char path[ 8 ] = "abc";

	TEST_CASE( "拼接结果超出缓冲区容量时应该返回 -1，且不破坏原有内容" );

	TEST_ASSERT( path_builder_prepend_segment( path, sizeof( path ), "this_segment_is_way_too_long" ) == -1,
	             "超长拼接应该返回 -1" );
	TEST_ASSERT_EQ_STR( path, "abc", "失败后原有内容应该保持不变" );
}

static void test_rejects_null_arguments( void )
{
	char path[ 16 ] = "";

	TEST_CASE( "path 或 segment 为 NULL 时应该返回 -1，而不是崩溃" );

	TEST_ASSERT( path_builder_prepend_segment( NULL, sizeof( path ), "x" ) == -1, "path 为 NULL 应该返回 -1" );
	TEST_ASSERT( path_builder_prepend_segment( path, sizeof( path ), NULL ) == -1, "segment 为 NULL 应该返回 -1" );
}

static void test_append_to_empty_path( void )
{
	char path[ 64 ] = "";

	TEST_CASE( "往空路径拼第一段，结果应该就是这段本身，不应该多出反斜杠" );

	TEST_ASSERT( path_builder_append_segment( path, sizeof( path ), "SUBDIR" ) == 1,
	             "拼接不应报错" );
	TEST_ASSERT_EQ_STR( path, "SUBDIR", "结果应该是 SUBDIR" );
}

static void test_append_builds_nested_path_from_root_to_leaf( void )
{
	char path[ 64 ] = "";

	TEST_CASE( "模拟从根往叶子走：依次拼每一层子目录，顺序应该正确（和 prepend 相反的方向）" );

	TEST_ASSERT( path_builder_append_segment( path, sizeof( path ), "SUBDIR" ) == 1, "第一次拼接不应报错" );
	TEST_ASSERT_EQ_STR( path, "SUBDIR", "拼接后应该是 SUBDIR" );

	TEST_ASSERT( path_builder_append_segment( path, sizeof( path ), "DEEPER" ) == 1, "第二次拼接不应报错" );
	TEST_ASSERT_EQ_STR( path, "SUBDIR\\DEEPER", "拼接后应该是 SUBDIR\\DEEPER" );

	TEST_ASSERT( path_builder_append_segment( path, sizeof( path ), "EVEN_DEEPER" ) == 1, "第三次拼接不应报错" );
	TEST_ASSERT_EQ_STR( path, "SUBDIR\\DEEPER\\EVEN_DEEPER", "拼接后应该是 SUBDIR\\DEEPER\\EVEN_DEEPER" );
}

static void test_append_rejects_when_result_too_long( void )
{
	char path[ 8 ] = "abc";

	TEST_CASE( "拼接结果超出缓冲区容量时应该返回 -1，且不破坏原有内容" );

	TEST_ASSERT( path_builder_append_segment( path, sizeof( path ), "this_segment_is_way_too_long" ) == -1,
	             "超长拼接应该返回 -1" );
	TEST_ASSERT_EQ_STR( path, "abc", "失败后原有内容应该保持不变" );
}

static void test_append_rejects_null_arguments( void )
{
	char path[ 16 ] = "";

	TEST_CASE( "path 或 segment 为 NULL 时应该返回 -1，而不是崩溃" );

	TEST_ASSERT( path_builder_append_segment( NULL, sizeof( path ), "x" ) == -1, "path 为 NULL 应该返回 -1" );
	TEST_ASSERT( path_builder_append_segment( path, sizeof( path ), NULL ) == -1, "segment 为 NULL 应该返回 -1" );
}

int main( void )
{
	test_prepend_to_empty_path();
	test_prepend_builds_nested_path_from_leaf_to_root();
	test_rejects_when_result_too_long();
	test_rejects_null_arguments();

	test_append_to_empty_path();
	test_append_builds_nested_path_from_root_to_leaf();
	test_append_rejects_when_result_too_long();
	test_append_rejects_null_arguments();

	TEST_SUMMARY();
}
