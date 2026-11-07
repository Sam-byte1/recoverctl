/*
 * B1（BitLocker，2026-11-05 任务拆解排期）：密钥保护器类型翻译的TDD覆盖。
 */
#include <string.h>

#include "test_framework.h"
#include "../../core/bitlocker_key_protector_type_name.h"

static void test_all_six_known_types( void )
{
	TEST_CASE( "libbde定义的全部6种密钥保护器类型都应该有对应的人话翻译" );

	TEST_ASSERT_EQ_STR( bitlocker_key_protector_type_name( 0x0000 ), "明文密钥（无保护）", "CLEAR_KEY" );
	TEST_ASSERT_EQ_STR( bitlocker_key_protector_type_name( 0x0200 ), "启动密钥（.BEK文件）", "STARTUP_KEY" );
	TEST_ASSERT_EQ_STR( bitlocker_key_protector_type_name( 0x0800 ), "恢复密钥（48位数字）", "RECOVERY_PASSWORD" );
	TEST_ASSERT_EQ_STR( bitlocker_key_protector_type_name( 0x2000 ), "密码", "PASSWORD" );
}

static void test_tpm_types_flagged_as_unsupported( void )
{
	const char *tpm_result;
	const char *tpm_pin_result;

	TEST_CASE( "TPM/TPM+PIN这两种libbde本身不支持解锁的类型，翻译结果里要明确标注不支持" );

	tpm_result = bitlocker_key_protector_type_name( 0x0100 );
	tpm_pin_result = bitlocker_key_protector_type_name( 0x0500 );

	TEST_ASSERT( strstr( tpm_result, "不支持" ) != NULL, "TPM 应该标注不支持解锁" );
	TEST_ASSERT( strstr( tpm_pin_result, "不支持" ) != NULL, "TPM+PIN 应该标注不支持解锁" );
}

static void test_unknown_type_includes_the_raw_value( void )
{
	const char *result;

	TEST_CASE( "未知类型数值不应该返回空字符串，应该把原始数值也带出来" );

	result = bitlocker_key_protector_type_name( 0x9999 );

	TEST_ASSERT( result != NULL, "不应该返回 NULL" );
	TEST_ASSERT( result[ 0 ] != '\0', "不应该返回空字符串" );
	TEST_ASSERT( strstr( result, "9999" ) != NULL, "应该包含原始的十六进制数值" );
}

int main( void )
{
	test_all_six_known_types();
	test_tpm_types_flagged_as_unsupported();
	test_unknown_type_includes_the_raw_value();

	TEST_SUMMARY();
}
