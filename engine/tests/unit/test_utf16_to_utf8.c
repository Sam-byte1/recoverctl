#include <string.h>

#include "test_framework.h"
#include "../../core/utf16_to_utf8.h"

static void test_pure_ascii_passthrough( void )
{
	/* "HELLO" 的 UTF-16LE 编码 */
	uint8_t input[] = { 'H', 0, 'E', 0, 'L', 0, 'L', 0, 'O', 0 };
	char out[ 32 ];

	TEST_CASE( "纯 ASCII 字符应该原样转换（这是之前简化版本凑巧能工作的那部分）" );

	TEST_ASSERT( utf16le_to_utf8( input, 5, out, sizeof( out ) ) == 5, "应该返回写入了 5 字节" );
	TEST_ASSERT_EQ_STR( out, "HELLO", "结果应该是 HELLO" );
}

static void test_chinese_characters_are_correctly_encoded( void )
{
	/* "测试" 的 UTF-16LE 编码：U+6D4B "测"、U+8BD5 "试" */
	uint8_t input[] = { 0x4B, 0x6D, 0xD5, 0x8B };
	char out[ 32 ];
	int result;

	TEST_CASE( "中文字符应该被正确转成 UTF-8（这是之前简化版本会出错的场景：之前会把这两个字直接截成垃圾字节）" );

	result = utf16le_to_utf8( input, 2, out, sizeof( out ) );
	/* "测试" 的 UTF-8 编码是 6 字节（每个汉字 3 字节） */
	TEST_ASSERT_EQ_INT( result, 6, "应该写入 6 字节（两个 3 字节的 UTF-8 字符）" );
	TEST_ASSERT( memcmp( out, "\xE6\xB5\x8B\xE8\xAF\x95", 6 ) == 0, "应该是'测试'正确的 UTF-8 字节序列" );
	TEST_ASSERT_EQ_INT( out[ 6 ], '\0', "结果应该以 \\0 结尾" );
}

static void test_surrogate_pair_is_correctly_encoded( void )
{
	/* U+1F600（😀，GRINNING FACE）的 UTF-16 代理对编码：0xD83D 0xDE00 */
	uint8_t input[] = { 0x3D, 0xD8, 0x00, 0xDE };
	char out[ 32 ];
	int result;

	TEST_CASE( "代理对（超出 BMP 范围的字符，比如 emoji）应该被正确拼成一个码点再编码成 4 字节 UTF-8" );

	result = utf16le_to_utf8( input, 2, out, sizeof( out ) );
	TEST_ASSERT_EQ_INT( result, 4, "U+1F600 的 UTF-8 编码应该是 4 字节" );
	TEST_ASSERT( memcmp( out, "\xF0\x9F\x98\x80", 4 ) == 0, "应该是正确的 4 字节 UTF-8 序列" );
}

static void test_lone_high_surrogate_becomes_replacement_character( void )
{
	/* 孤立的高代理项，后面没有跟配对的低代理项（后面直接是 'A'） */
	uint8_t input[] = { 0x3D, 0xD8, 'A', 0 };
	char out[ 32 ];

	TEST_CASE( "孤立的高代理项（没有配对的低代理项）应该被替换成 U+FFFD，不应该崩溃或者产生非法 UTF-8" );

	TEST_ASSERT( utf16le_to_utf8( input, 2, out, sizeof( out ) ) > 0, "不应该返回错误" );
	/* U+FFFD 的 UTF-8 编码是 EF BF BD，后面跟 'A' */
	TEST_ASSERT( memcmp( out, "\xEF\xBF\xBD" "A", 4 ) == 0, "应该是替换字符后面跟着正常的 A" );
}

static void test_truncates_cleanly_when_buffer_too_small( void )
{
	/* "测试" 两个汉字，但输出缓冲区只够装 1 个汉字（3字节）+ \0 */
	uint8_t input[] = { 0x4B, 0x6D, 0xD5, 0x8B };
	char out[ 4 ];
	int result;

	TEST_CASE( "输出缓冲区装不下时应该在完整字符边界截断，不应该切出半个字符的残缺字节" );

	result = utf16le_to_utf8( input, 2, out, sizeof( out ) );
	TEST_ASSERT_EQ_INT( result, 3, "应该只写入第一个汉字的 3 字节，第二个装不下就不写" );
	TEST_ASSERT( memcmp( out, "\xE6\xB5\x8B", 3 ) == 0, "应该是'测'的正确 UTF-8 编码" );
	TEST_ASSERT_EQ_INT( out[ 3 ], '\0', "应该正确以 \\0 结尾" );
}

static void test_rejects_null_arguments( void )
{
	char out[ 8 ];

	TEST_CASE( "参数不合法时应该返回 -1，而不是崩溃" );

	TEST_ASSERT( utf16le_to_utf8( NULL, 1, out, sizeof( out ) ) == -1, "utf16le_bytes 为 NULL 应该返回 -1" );
}

int main( void )
{
	test_pure_ascii_passthrough();
	test_chinese_characters_are_correctly_encoded();
	test_surrogate_pair_is_correctly_encoded();
	test_lone_high_surrogate_becomes_replacement_character();
	test_truncates_cleanly_when_buffer_too_small();
	test_rejects_null_arguments();

	TEST_SUMMARY();
}
