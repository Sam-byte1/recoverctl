/*
 * B4（BitLocker，2026-11-05 任务拆解排期）：恢复密钥规范化 + FVEK/TWEAK
 * 十六进制解码的TDD覆盖。
 */
#include <string.h>

#include "test_framework.h"
#include "../../core/bitlocker_credential.h"

static void test_normalize_regroups_dashes_every_6_digits( void )
{
	char out[ 64 ];

	TEST_CASE( "恢复密钥规范化：不管用户输入的短横线位置对不对，都应该重新按每6位数字插入短横线"
	           "（B9真机验证发现libbde要的是`XXXXXX-XXXXXX-...`格式，不是纯数字，见头文件注释）" );

	TEST_ASSERT( bitlocker_credential_normalize_recovery_key( "123456-789012-345678", out, sizeof( out ) ) == 1,
	             "应该成功" );
	TEST_ASSERT_EQ_STR( out, "123456-789012-345678", "应该重新规范成每6位一组、短横线分隔的格式" );
}

static void test_normalize_strips_spaces_and_newlines( void )
{
	char out[ 64 ];

	TEST_CASE( "恢复密钥规范化：应该去掉空格和换行这类常见的粘贴噪音，再重新按6位分组插入短横线" );

	TEST_ASSERT( bitlocker_credential_normalize_recovery_key( "111111 222222\n333333", out, sizeof( out ) ) == 1,
	             "应该成功" );
	TEST_ASSERT_EQ_STR( out, "111111-222222-333333", "空格和换行都应该被去掉，数字重新按6位分组" );
}

static void test_normalize_real_device_recovery_password( void )
{
	char out[ 64 ];

	TEST_CASE( "恢复密钥规范化：B9真机验证用过的真实恢复密码（manage-bde生成），规范化之后应该和"
	           "原始格式完全一致——这是直接复现B9第一次真机验证失败的那个输入" );

	TEST_ASSERT( bitlocker_credential_normalize_recovery_key(
	             "481767-564927-534611-676214-345433-345235-169653-013662", out, sizeof( out ) ) == 1,
	             "应该成功" );
	TEST_ASSERT_EQ_STR( out, "481767-564927-534611-676214-345433-345235-169653-013662",
	             "8段、每段6位数字、短横线分隔，和manage-bde原始输出完全一致" );
}

static void test_normalize_output_buffer_too_small( void )
{
	char tiny_out[ 4 ];

	TEST_CASE( "恢复密钥规范化：输出缓冲区装不下时应该返回-1，而不是截断或溢出" );

	TEST_ASSERT( bitlocker_credential_normalize_recovery_key( "123456789012345678", tiny_out, sizeof( tiny_out ) ) == -1,
	             "应该返回 -1" );
}

static void test_normalize_null_arguments_rejected( void )
{
	char out[ 16 ];

	TEST_CASE( "恢复密钥规范化：空指针参数应该被拒绝，而不是崩溃" );

	TEST_ASSERT( bitlocker_credential_normalize_recovery_key( NULL, out, sizeof( out ) ) == -1, "input为NULL应该返回-1" );
	TEST_ASSERT( bitlocker_credential_normalize_recovery_key( "123456", NULL, 16 ) == -1, "out为NULL应该返回-1" );
}

static void test_hex_decode_basic( void )
{
	uint8_t out[ 16 ];
	size_t out_length = 0;

	TEST_CASE( "十六进制解码：正常输入应该解码成正确的字节" );

	TEST_ASSERT( bitlocker_credential_hex_decode( "deadbeef", out, sizeof( out ), &out_length ) == 1,
	             "应该成功" );
	TEST_ASSERT_EQ_INT( (int) out_length, 4, "应该解码出4个字节" );

	if( out_length == 4 )
	{
		TEST_ASSERT( out[ 0 ] == 0xde, "第1字节应该是0xde" );
		TEST_ASSERT( out[ 1 ] == 0xad, "第2字节应该是0xad" );
		TEST_ASSERT( out[ 2 ] == 0xbe, "第3字节应该是0xbe" );
		TEST_ASSERT( out[ 3 ] == 0xef, "第4字节应该是0xef" );
	}
}

static void test_hex_decode_case_insensitive( void )
{
	uint8_t out[ 16 ];
	size_t out_length = 0;

	TEST_CASE( "十六进制解码：应该不区分大小写" );

	TEST_ASSERT( bitlocker_credential_hex_decode( "DEADBEEF", out, sizeof( out ), &out_length ) == 1,
	             "应该成功" );
	if( out_length == 4 )
	{
		TEST_ASSERT( out[ 0 ] == 0xde, "大写形式解码结果应该和小写一致" );
	}
}

static void test_hex_decode_empty_string_is_valid( void )
{
	uint8_t out[ 16 ];
	size_t out_length = 999;

	TEST_CASE( "十六进制解码：空字符串是合法输入（对应没有TWEAK密钥的场景），应该解码出0字节" );

	TEST_ASSERT( bitlocker_credential_hex_decode( "", out, sizeof( out ), &out_length ) == 1,
	             "应该成功" );
	TEST_ASSERT_EQ_INT( (int) out_length, 0, "应该是0字节" );
}

static void test_hex_decode_odd_length_rejected( void )
{
	uint8_t out[ 16 ];
	size_t out_length = 0;

	TEST_CASE( "十六进制解码：奇数长度应该被拒绝" );

	TEST_ASSERT( bitlocker_credential_hex_decode( "abc", out, sizeof( out ), &out_length ) == -1,
	             "应该返回 -1" );
}

static void test_hex_decode_invalid_char_rejected( void )
{
	uint8_t out[ 16 ];
	size_t out_length = 0;

	TEST_CASE( "十六进制解码：出现非十六进制字符应该被拒绝" );

	TEST_ASSERT( bitlocker_credential_hex_decode( "zz00", out, sizeof( out ), &out_length ) == -1,
	             "应该返回 -1" );
}

static void test_hex_decode_output_too_small_rejected( void )
{
	uint8_t tiny_out[ 1 ];
	size_t out_length = 0;

	TEST_CASE( "十六进制解码：输出缓冲区装不下时应该返回-1" );

	TEST_ASSERT( bitlocker_credential_hex_decode( "deadbeef", tiny_out, sizeof( tiny_out ), &out_length ) == -1,
	             "应该返回 -1" );
}

int main( void )
{
	test_normalize_regroups_dashes_every_6_digits();
	test_normalize_strips_spaces_and_newlines();
	test_normalize_real_device_recovery_password();
	test_normalize_output_buffer_too_small();
	test_normalize_null_arguments_rejected();
	test_hex_decode_basic();
	test_hex_decode_case_insensitive();
	test_hex_decode_empty_string_is_valid();
	test_hex_decode_odd_length_rejected();
	test_hex_decode_invalid_char_rejected();
	test_hex_decode_output_too_small_rejected();

	TEST_SUMMARY();
}
